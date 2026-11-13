#!/usr/bin/env python3
"""Analyze passive DXMT TBDR diagnostics; byte proxies are NOT measured bandwidth."""
import argparse
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path
import re
import statistics


def parse_fields(text):
    result = {}
    for key, value in re.findall(r"(\w+)=([^\s]+)", text):
        if key in {"queue", "role", "resource", "allocation", "native", "view", "resolve", "label"}:
            result[key] = value
        else:
            try:
                result[key] = float(value) if "." in value else int(value)
            except ValueError:
                result[key] = value
    return result


def union_ns(intervals):
    intervals = sorted((a, b) for a, b in intervals if b > a > 0)
    if not intervals:
        return 0
    total, start, end = 0, *intervals[0]
    for a, b in intervals[1:]:
        if a > end:
            total += end-start
            start, end = a, b
        else:
            end = max(end, b)
    return total+end-start


def percentiles(values):
    values = sorted(values)
    if not values:
        return {}
    return {"min": values[0], "p10": values[round((len(values)-1)*.1)],
            "median": statistics.median(values), "p90": values[round((len(values)-1)*.9)], "max": values[-1]}


def analyze(lines, min_frame=120, max_frame=600, require_present=True, format_names=None):
    cbs, attachments, allocations, labels = {}, [], [], []
    duplicate = 0
    for line in lines:
        match = re.search(r"TBDR (cb|attachment|allocation|render|pfv_upgrade) (.*)", line)
        if not match:
            continue
        kind, tail = match.groups()
        row = parse_fields(tail)
        if kind == "cb":
            key = row["queue"], row["seq"]
            duplicate += key in cbs
            cbs[key] = row
        elif kind == "attachment":
            attachments.append(row)
        elif kind == "allocation":
            allocations.append(row)
        elif kind == "render":
            labels.append(dict(frame=row["frame"], seq=row["seq"], label=tail.split("label=", 1)[-1]))
    if not cbs:
        raise ValueError("No TBDR command-buffer rows")
    queues = Counter()
    for (queue, _), row in cbs.items():
        queues[queue] += row["render_after"]
    primary = queues.most_common(1)[0][0]
    groups = defaultdict(list)
    for (queue, _), row in cbs.items():
        if queue == primary and row["frame"] >= min_frame and (max_frame is None or row["frame"] < max_frame):
            groups[row["frame"]].append(row)
    frames = []
    incomplete = invalid = 0
    for frame, rows in sorted(groups.items()):
        if require_present and not sum(r["present"] for r in rows):
            incomplete += 1
            continue
        intervals = [(r["gpu_start_ns"], r["gpu_end_ns"]) for r in rows
                     if r["status"] == 4 and r["gpu_end_ns"] > r["gpu_start_ns"] > 0]
        invalid += len(rows)-len(intervals)
        if not intervals:
            continue
        merged = union_ns(intervals)
        envelope = max(b for _, b in intervals)-min(a for a, _ in intervals)
        total = sum(b-a for a, b in intervals)
        result = dict(frame=frame, command_buffers=len(rows), start_ns=min(a for a, _ in intervals),
                      cb_interval_union_ms=merged/1e6, cb_interval_sum_ms=total/1e6,
                      envelope_ms=envelope/1e6, inter_cb_gap_ms=(envelope-merged)/1e6,
                      interval_overlap_ms=(total-merged)/1e6)
        for key in rows[0]:
            if key not in {"queue", "seq", "frame", "gpu_start_ns", "gpu_end_ns", "status"}:
                result[key] = sum(r[key] for r in rows)
        frames.append(result)
    selected = {r["frame"] for r in frames}
    profiles = {}
    for row in attachments:
        if row["frame"] not in selected:
            continue
        key = tuple(row.get(k) for k in ["role", "width", "height", "samples", "format", "usage", "load", "store"])
        item = profiles.setdefault(key, dict(zip(["role", "width", "height", "samples", "format", "usage", "load", "store"], key)))
        item["observations"] = item.get("observations", 0)+1
        item["footprint_proxy_sum"] = item.get("footprint_proxy_sum", 0)+row["footprint"]
        item["with_resolve"] = item.get("with_resolve", 0)+(row["resolve"] != "0")
    for row in profiles.values():
        row["format_name"] = (format_names or {}).get(row["format"], str(row["format"]))
    stats = {key: percentiles([r[key] for r in frames]) for key in frames[0] if key not in {"frame", "start_ns"}} if frames else {}
    periods = [(b["start_ns"]-a["start_ns"])/1e6 for a, b in zip(frames, frames[1:]) if b["frame"] == a["frame"]+1]
    return dict(primary_queue=primary, parsed_command_buffers=len(cbs), duplicate_rows=duplicate,
                selected_frames=len(frames), omitted_no_present=incomplete, invalid_gpu_intervals=invalid,
                frame_range=[min(selected), max(selected)] if selected else [], statistics=stats,
                frame_start_period_ms=percentiles(periods), frames=frames,
                attachment_profiles=sorted(profiles.values(), key=lambda x: x["footprint_proxy_sum"], reverse=True),
                large_allocations=allocations,
                render_label_counts=[dict(label=k, observations=v) for k, v in Counter(r["label"] for r in labels if r["frame"] in selected).most_common()],
                limitations=["GPU intervals cover DXMT command buffers, including internal waits; union is not shader-active time.",
                             "Envelope includes inter-buffer gaps; summing overlapping intervals overcounts elapsed time.",
                             "Attachment byte proxies are nominal uncompressed work, NOT measured DRAM bytes/bandwidth.",
                             "Depth32Float_Stencil8 proxy uses nominal 8-byte packed payload; actual Metal storage may differ.",
                             "Presenter/scaler internals, resource-initializer queue and service compositor are outside attachment proxies.",
                             "Render/clear/resolve counts are DXMT nodes, not complete native Metal encoder counts.",
                             "Detail rows have a 20,000-row budget; allocation pointer reuse and multiple queues limit joins.",
                             "Merge rejection counts are optimizer attempts, not counts of removable passes.",
                             "Recorded frame IDs follow the existing DXMT Present boundary; simulated headset caps presentation."])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--min-frame", type=int, default=120)
    parser.add_argument("--max-frame", type=int, default=600)
    parser.add_argument("--frames-csv", type=Path)
    parser.add_argument("--format-header", type=Path, default=Path(__file__).resolve().parent.parent/"build-in-process/dxmt-foveation-source/src/winemetal/winemetal.h")
    args = parser.parse_args()
    names = {}
    if args.format_header.exists():
        names = {int(value): name for name, value in re.findall(r"WMTPixelFormat(\w+)\s*=\s*(\d+)", args.format_header.read_text())}
    with args.log.open(errors="replace") as file:
        result = analyze(file, args.min_frame, args.max_frame, format_names=names)
    if args.frames_csv and result["frames"]:
        with args.frames_csv.open("w") as file:
            writer = csv.DictWriter(file, fieldnames=list(result["frames"][0]))
            writer.writeheader()
            writer.writerows(result["frames"])
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()

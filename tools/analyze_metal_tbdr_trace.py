#!/usr/bin/env python3
"""Summarize exported xctrace GPU tables without copying broad trace/environment data."""
import argparse
from collections import Counter, defaultdict
import json
import re
from pathlib import Path
import statistics
import xml.etree.ElementTree as ET


def rows(path):
    cache, stack, columns = {}, [], []

    def decode(element):
        if "ref" in element.attrib:
            return cache.get(element.attrib["ref"], (None, ""))
        children = [decode(c) for c in element]
        text = element.text.strip() if element.text else ""
        formatted = element.get("fmt", "")
        value = text or formatted or " ".join(str(c[0]) for c in children if c[0] is not None)
        if element.tag in {"start-time", "duration", "event-time", "uint64", "uint32", "gpu-frame-number", "size-in-bytes", "metal-command-buffer-id", "metal-nesting-level", "metal-encoder-id"}:
            try:
                value = int(text)
            except ValueError:
                pass
        result = value, formatted
        if "id" in element.attrib:
            cache[element.attrib["id"]] = result
        return result

    with Path(path).open("rb") as file:
        for event, element in ET.iterparse(file, events=["start", "end"]):
            if event == "start":
                stack.append(element)
                continue
            if element.tag == "schema":
                columns = [c.findtext("mnemonic") for c in element.findall("col")]
            elif element.tag == "row":
                yield dict(zip(columns, [decode(c) for c in element]))
                if len(stack) > 1:
                    stack[-2].remove(element)
                element.clear()
            stack.pop()


def analyze(root, process_name="EXD-Win64-Shipping.exe"):
    root = Path(root)
    target_pid=None
    if (root/"xctrace.log").exists():
        match=re.search(r"Attaching to:.*\((\d+)\)",(root/"xctrace.log").read_text())
        if match:target_pid=int(match[1])
    target=f"{process_name} ({target_pid})" if target_pid else process_name
    encoder_labels, encoder_types = {}, Counter()
    for row in rows(root/"metal-application-encoders-list.xml"):
        if target not in row.get("process", ("", ""))[1]:
            continue
        key = row.get("encoder-id", (None, ""))[0]
        label = row.get("encoder-label", ("", ""))[0]
        if label:
            encoder_labels[key] = label
        encoder_types[row.get("event-type", ("unknown", ""))[0]] += 1
    groups = defaultdict(list)
    channels = defaultdict(list)
    matched = total = 0
    frame_ids = set()
    for row in rows(root/"metal-gpu-intervals.xml"):
        if target not in row.get("process", ("", ""))[1]:
            continue
        duration = row["duration"][0]
        if not isinstance(duration, int):
            continue
        channel = str(row["channel-name"][0])
        frame_ids.add(row["frame-number"][0])
        key = row.get("encoder-id", (None, ""))[0]
        label = row.get("channel-subtitle", ("", ""))[0] or encoder_labels.get(key, "")
        total += 1
        matched += bool(label)
        groups[(channel, str(label) if label else "<unlabelled>")].append(duration/1e6)
        channels[channel].append(duration/1e6)
    profiles = [dict(channel=c, label=l, intervals=len(values), sum_ms=sum(values), median_ms=statistics.median(values),
                     p90_ms=sorted(values)[round((len(values)-1)*.9)]) for (c, l), values in groups.items()]
    allocations = []
    for row in rows(root/"metal-current-allocated-size.xml"):
        if target in row.get("process", ("", ""))[1]:
            allocations.append(row["current-allocated-size"][0])
    counters = []
    if (root/"gpu-counter-info.xml").exists():
        for row in rows(root/"gpu-counter-info.xml"):
            counters.append({k: value[0] for k, value in row.items() if k in {"counter-id", "name", "description", "type"}})
    return dict(process=process_name, target_pid=target_pid, gpu_intervals=total, intervals_with_label=matched, trace_gpu_frame_ids=len(frame_ids),
                encoder_event_types=dict(encoder_types),
                channel_totals={name: dict(intervals=len(values), sum_ms=sum(values), median_ms=statistics.median(values)) for name, values in channels.items()},
                pass_signatures=sorted(profiles, key=lambda x: x["sum_ms"], reverse=True),
                allocated_size_bytes=dict(min=min(allocations), median=statistics.median(allocations), max=max(allocations)) if allocations else {},
                recorded_counter_metadata=counters,
                limitations=["Tiler and renderer intervals overlap; do not add channel totals into frame time.",
                             "Pass labels group matching signatures, not unique shader/functional identity.",
                             "Trace frame IDs differ from DXMT Present IDs; do not join separate captures by frame number.",
                             "currentAllocatedSize is Metal device allocation size, not measured resident VRAM.",
                             "Only the recorded counter names are available; no DRAM bandwidth is inferred.",
                             "This is a profiler run, not an uninstrumented performance comparison."])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.directory), indent=2))


if __name__ == "__main__":
    main()

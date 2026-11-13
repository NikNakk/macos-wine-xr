#!/usr/bin/env python3
"""Summarize explicit DXMT depth/MSAA experiments; never label mapped depth compatible."""
import argparse
import csv
import json
import math
from pathlib import Path
import re


def fields(line):
    result = {}
    for key, value in re.findall(r"(\w+)=([^\s]+)", line):
        if key == "case":
            result[key] = value
        else:
            result[key] = float(value) if "." in value else int(value)
    return result


def analyze(path):
    text = Path(path).read_text()
    rows = [fields(line) for line in text.splitlines() if line.startswith("DEPTH_RESULT ")]
    baselines = [fields(line) for line in text.splitlines() if line.startswith("DEPTH_BASELINE ")]
    replays = [fields(line) for line in text.splitlines() if line.startswith("DEPTH_REPLAY ")]
    expected = {"identity-nearest", "identity-bilinear", "identity-min", "nearest", "bilinear", "min",
                "nearest-full-depth", "bilinear-full-depth", "min-full-depth"}
    if len(rows) != 9 or {r["case"] for r in rows} != expected or len(baselines) != 1 or len(replays) != 1:
        raise ValueError(f"{path}: incomplete experiment")
    baseline, replay = baselines[0], replays[0]
    if replay["wrong"] or "PASS experiment completed" not in text:
        raise ValueError(f"{path}: depth replay/control failed")
    maps = re.findall(r"SCENE render[^\n]* map=([\da-f]+)", text)
    mapped = sum(int(value, 16) != 0 for value in maps)
    if mapped != 24:
        raise ValueError(f"{path}: expected 24 explicitly mapped passes; got {mapped}")
    for row in rows:
        if (row["size"], row["samples"]) != (baseline["size"], baseline["samples"]):
            raise ValueError(f"{path}: mixed dimensions/sample counts")
        if row["checked_samples"] != row["size"] ** 2 * row["samples"] or row["center_bad"]:
            raise ValueError(f"{path}: sample count/full-rate centre control failed")
        if row["case"].startswith("identity-"):
            keys = ["depth_abs_1e5", "max_depth_error", "color_bad", "coverage_bad", "threshold_bad", "resolved_pixels_bad"]
            if any(row[k] for k in keys):
                raise ValueError(f"{path}: identity round trip failed")
        elif row["case"].endswith("-full-depth"):
            if any(row[k] for k in ["depth_abs_1e5", "max_depth_error", "coverage_bad", "threshold_bad"]):
                raise ValueError(f"{path}: full-depth alternative failed")
        elif not row["depth_abs_1e5"] or not row["threshold_bad"]:
            raise ValueError(f"{path}: expected mapped-depth counterexample missing")
        if row["case"] == "bilinear" and row["plane_bad"]:
            raise ValueError(f"{path}: smooth-plane reconstruction control failed")
        components = [row[k] for k in ["scene_gpu_ms", "reconstruct_gpu_ms", "resolve_gpu_ms", "depth_prepass_gpu_ms"]]
        if any(not math.isfinite(v) or v < 0 for v in components) or abs(sum(components)-row["total_gpu_ms"]) > .000005:
            raise ValueError(f"{path}: invalid GPU component sum")
        row["saving_percent_vs_baseline"] = round(100*(1-row["total_gpu_ms"]/baseline["total_gpu_ms"]), 3)
    return {"log": str(path), "controls_passed": True, "mapped_render_passes": mapped,
            "baseline": baseline, "depth_replay": replay, "results": rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+")
    parser.add_argument("--csv", type=Path)
    args = parser.parse_args()
    reports = [analyze(path) for path in args.logs]
    if args.csv:
        rows = [dict(log=report["log"], **row) for report in reports for row in report["results"]]
        with args.csv.open("w") as file:
            writer = csv.DictWriter(file, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    print(json.dumps({"component_sums_only": True,
                      "notes": ["Independent scene/reconstruction/resolve/depth-replay medians; not a joined frame timing.",
                                "Mapped-depth errors are a counterexample, not accepted compatibility.",
                                "Full-depth cases preserve depth; peripheral colour/coverage quality remains approximate.",
                                "No EXD, live gaze, CPU overhead or MetalFX performance claim."],
                      "experiments": reports}, indent=2))


if __name__ == "__main__":
    main()

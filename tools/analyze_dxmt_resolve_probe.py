#!/usr/bin/env python3
"""Validate resolve-first fixture controls and summarize component timings/quality."""
import argparse
import csv
import json
import math
from pathlib import Path
import re


def analyze(path):
    text = Path(path).read_text()
    lines = re.findall(r"^RESOLVE_FIRST .+$", text, re.MULTILINE)
    if len(lines) != 1 or "PASS resolve-first control" not in text:
        raise ValueError(f"{path}: missing completed experiment")
    row = {key: float(value) if "." in value else int(value)
           for key, value in re.findall(r"(\w+)=([^\s]+)", lines[0])}
    if any(row[k] for k in ["identity_bad", "center_bad", "depth_wrong", "final_depth_wrong", "source_preserved_bad"]):
        raise ValueError(f"{path}: identity/centre/exact-depth control failed")
    if row["pixels"] != row["size"]**2 or row["depth_samples"] != row["pixels"]*row["samples"]:
        raise ValueError(f"{path}: incomplete pixel/sample inspection")
    if row["rate"] == 1 and any(row[k] for k in ["color_bad", "thin_roi_bad", "mae", "max_color_error", "logical_source_color_bad", "logical_source_depth_bad"]):
        raise ValueError(f"{path}: full-rate output control failed")
    if row["rate"] < 1 and (not row["logical_source_color_bad"] or not row["logical_source_depth_bad"]):
        raise ValueError(f"{path}: expected source-layout counterexample missing")
    mapped = sum(int(value, 16) != 0 for value in re.findall(r"SCENE render[^\n]* map=([\da-f]+)", text))
    if mapped != 24:
        raise ValueError(f"{path}: expected 24 mapped scene passes, found {mapped}")
    for key, value in row.items():
        if not math.isfinite(value) or value < 0:
            raise ValueError(f"{path}: invalid {key}")
    total = row["scene_gpu_ms"]+row["depth_prepass_gpu_ms"]+row["joined_color_gpu_ms"]
    baseline = row["baseline_scene_gpu_ms"]+row["baseline_native_resolve_gpu_ms"]
    if abs(total-row["total_gpu_ms"]) > .000005 or abs(baseline-row["baseline_gpu_ms"]) > .000005:
        raise ValueError(f"{path}: invalid component sum")
    row.update(log=str(path), color_bad_percent=round(100*row["color_bad"]/row["pixels"], 4),
               saving_percent=round(100*(1-row["total_gpu_ms"]/row["baseline_gpu_ms"]), 3))
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+")
    parser.add_argument("--csv", type=Path)
    args = parser.parse_args()
    rows = [analyze(path) for path in args.logs]
    if args.csv:
        with args.csv.open("w") as file:
            writer = csv.DictWriter(file, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    print(json.dumps({"whole_frame_timing": False,
                      "notes": ["Resolve+unwarp joined_color_gpu_ms is measured in one native GPU command buffer.",
                                "Scene and full-resolution depth replay are independent D3D11 component medians.",
                                "Baseline native resolve uses the same timing method; D3D resolve time is diagnostic only.",
                                "Native phases rotate order and discard three warmup rounds, then median21.",
                                "Raw MSAA source remains in mapped coordinates: destination reconstruction alone is not transparent.",
                                "Colour is approximate; no EXD/stereo/gaze/CPU/MetalFX claim."],
                      "results": rows}, indent=2))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Sample the running Home process without launching or stopping XR apps."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--pid", type=int, help="macOS PID (not the Wine PID); auto-detects steamtours if omitted")
parser.add_argument("--seconds", type=int, default=5)
parser.add_argument("--interval-ms", type=int, default=10)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--wine-log", type=Path, help="stderr log from the prefix's first Wine launch")
args = parser.parse_args()
if not 1 <= args.seconds <= 30 or not 1 <= args.interval_ms <= 1000:
    parser.error("use 1–30 seconds and 1–1000 ms intervals")
processes = subprocess.check_output(["ps", "-axo", "pid=,comm="], text=True)
if args.pid is None:
    candidates = []
    for line in processes.splitlines():
        parts = line.strip().split(None, 1)
        if len(parts) == 2 and parts[1].replace("\\", "/").rsplit("/", 1)[-1].lower() in {"steamtours", "steamtours.exe"}:
            candidates.append(int(parts[0]))
    if len(candidates) != 1:
        parser.error("could not uniquely detect Home; pass its macOS PID with --pid")
    args.pid = candidates[0]
if args.pid <= 0:
    parser.error("PID must be positive")
args.output.mkdir(parents=True, exist_ok=False)
metadata = {"pid": args.pid, "seconds": args.seconds, "interval_ms": args.interval_ms,
            "start_utc": datetime.now(timezone.utc).isoformat(), "wine_log": str(args.wine_log) if args.wine_log else None,
            "limitation": "CPU sampling can perturb timing; this is an attribution capture, not an unbiased pacing benchmark."}
(args.output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
print(f"Sampling Home macOS PID {args.pid} for {args.seconds}s; applications stay running.", flush=True)
result = subprocess.run(["/usr/bin/sample", str(args.pid), str(args.seconds), str(args.interval_ms),
                         "-file", str(args.output / "home-stacks.txt")], capture_output=True, text=True)
(args.output / "sampler-output.txt").write_text(result.stdout + result.stderr)
metadata.update(end_utc=datetime.now(timezone.utc).isoformat(), sample_exit_code=result.returncode)
if args.wine_log:
    lines = args.wine_log.read_text(errors="replace").splitlines()
    selected = [line for line in lines if line.startswith("mwxr-keyed") or "KeyedMutex owner phases:" in line
                or "KeyedMutex ownership interval:" in line or "KeyedMutex owned waits:" in line
                or "ReleaseSync timing:" in line]
    (args.output / "ownership.txt").write_text("\n".join(selected) + "\n")
(args.output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
print(f"Capture saved to {args.output}")
raise SystemExit(result.returncode)

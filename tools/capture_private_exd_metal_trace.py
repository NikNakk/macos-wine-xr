#!/usr/bin/env python3
"""Record a bounded Metal System Trace of the EXD process of a TBDR audit run.

Paths as scripts/exd-paths.zsh: MWXR_EXD_PREFIX, MWXR_EXD_WINE and MWXR_EXD_RUNS, defaulting
to the experiment prefix, the foveation runtime and build-in-process/exd-steam/runs."""
import os
from pathlib import Path
import re
import subprocess
import time

repo = Path(__file__).resolve().parent.parent
prefix = Path(os.environ.get("MWXR_EXD_PREFIX", repo/"build-in-process/exd-steam/prefix"))
wine = Path(os.environ.get("MWXR_EXD_WINE", repo/"build-in-process/wine-foveation-unified"))
runs = Path(os.environ.get("MWXR_EXD_RUNS", repo/"build-in-process/exd-steam/runs"))
out = runs/time.strftime("tbdr-%Y%m%d-%H%M%S-system-trace")
out.mkdir(parents=True)
env = dict(os.environ, MWXR_FOVEATION_RUN_DIR=str(out), MWXR_TBDR_SECONDS="60")
start_frame = int(os.environ.get("MWXR_TBDR_TRACE_START_FRAME", "180"))
trace_seconds = int(os.environ.get("MWXR_TBDR_TRACE_SECONDS", "10"))
if not 0 <= start_frame <= 1800 or not 1 <= trace_seconds <= 20:
    raise SystemExit("trace start must be 0..1800 and duration 1..20 seconds")
runner = subprocess.Popen([str(repo/"scripts/run-exd-tbdr-audit.zsh"), "trace"], env=env)
print(f"EXD system-trace run: {out}", flush=True)
status = 1
try:
    deadline = time.monotonic()+45
    pid = None
    while time.monotonic() < deadline and runner.poll() is None:
        log = out/"game/game.log"
        frames = []
        if log.exists():
            with log.open("rb") as file:
                file.seek(max(0, log.stat().st_size-32768))
                tail = file.read().decode(errors="replace")
            frames = [int(value) for value in re.findall(r"TBDR cb[^\n]* frame=(\d+)", tail)]
        if frames and max(frames) >= start_frame:
            processes = subprocess.check_output(["ps", "-axo", "pid=,command="], text=True)
            matches = []
            for line in processes.splitlines():
                # Wine shows a Windows program by its Windows path (Z:\...\EXD-Win64-Shipping.exe).
                if re.match(r"\s*\d+\s+[A-Za-z]:\\.*EXD-Win64-Shipping\.exe", line):
                    matches.append(int(line.strip().split(None, 1)[0]))
            if len(matches) == 1:
                pid = matches[0]
                break
        time.sleep(.25)
    if pid is None:
        raise RuntimeError("A unique warmed EXD process was not found")
    print(f"Profiling EXD PID {pid} for {trace_seconds}s", flush=True)
    with (out/"xctrace.log").open("w") as file:
        status = subprocess.run(["xcrun", "xctrace", "record", "--template", "Metal System Trace",
                                 "--attach", str(pid), "--time-limit", f"{trace_seconds}s", "--no-prompt",
                                 "--output", str(out/"metal.trace")], stdout=file, stderr=subprocess.STDOUT).returncode
    print(f"xctrace exit={status}; {out/'xctrace.log'}", flush=True)
finally:
    runner.terminate()
    try:
        runner.wait(timeout=20)
    except subprocess.TimeoutExpired:
        runner.kill()
        runner.wait()
        subprocess.run([str(wine/"bin/wineserver"), "-k"], env=dict(os.environ, WINEPREFIX=str(prefix)))
raise SystemExit(status)

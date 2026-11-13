#!/usr/bin/env python3
"""Record a bounded Metal System Trace of only the task's private EXD process."""
import os
from pathlib import Path
import re
import subprocess
import time

repo = Path(__file__).resolve().parent.parent
out = repo/"build-in-process/exd-ngx"/time.strftime("tbdr-%Y%m%d-%H%M%S-system-trace")
out.mkdir(parents=True)
env = dict(os.environ, MWXR_FOVEATION_RUN_DIR=str(out), MWXR_TBDR_SECONDS="60")
start_frame = int(os.environ.get("MWXR_TBDR_TRACE_START_FRAME", "180"))
trace_seconds = int(os.environ.get("MWXR_TBDR_TRACE_SECONDS", "10"))
if not 0 <= start_frame <= 1800 or not 1 <= trace_seconds <= 20:
    raise SystemExit("trace start must be 0..1800 and duration 1..20 seconds")
runner = subprocess.Popen([str(repo/"scripts/run-exd-tbdr-audit.zsh"), "trace"], env=env)
print(f"Private EXD system-trace run: {out}", flush=True)
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
                if "EXD-Win64-Shipping.exe" in line and "exd-ngx" in line:
                    matches.append(int(line.strip().split(None, 1)[0]))
            if len(matches) == 1:
                pid = matches[0]
                break
        time.sleep(.25)
    if pid is None:
        raise RuntimeError("A unique warmed private EXD process was not found")
    print(f"Profiling private EXD PID {pid} for {trace_seconds}s", flush=True)
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
        subprocess.run([str(repo/"build-in-process/exd-ngx/wine/bin/wineserver"), "-k"],
                       env=dict(os.environ, WINEPREFIX=str(repo/"build-in-process/exd-ngx/prefix")))
raise SystemExit(status)

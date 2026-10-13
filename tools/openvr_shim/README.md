# OpenVR frame-loop tracer

The optional proxy forwards Valve exports and instruments compositor pose
queries and submissions. Pose correction remains separately opt-in and is
not required for timing diagnostics.

`MWXR_OPENVR_SHIM_INSTALL=1` asks the normal launcher to install the proxy
beside Home temporarily, saving/restoring Valve's DLL on exit. Add
`MWXR_OPENVR_SHIM_TRACE=1` for complete per-call intervals in
`<MWXR_OPENVR_SHIM_LOG>.trace`; the launcher supplies the aggregate log path.
Restart Steam if necessary so Home inherits those environment variables.

```sh
MWXR_OPENVR_SHIM_INSTALL=1 MWXR_OPENVR_SHIM_TRACE=1 MWXR_OPENVR_SHIM_FIX_POSE=0 ~/Windows/run-mwxr.zsh
```

Records cover WaitGetPoses, GetLastPoses, GetLastPoseForDevice and Submit,
with PID/TID, begin/end microseconds, call duration, eye/texture and error.
The existing five-second aggregate statistics remain. File output is buffered
at 64 KiB and explicitly flushed roughly once per second. Flush intervals
are logged so overlaps can be excluded; file writes and trace-lock contention
can perturb timing. A partial tail may remain buffered on abrupt termination.
This is attribution, not an unbiased pacing benchmark.

On the local CrossOver macOS build, both QPC and Wine server ownership
history use mach_continuous_time in 100 ns units; QueryPerformanceFrequency
is 10 MHz. Trace timestamps use the reported frequency to convert to us,
allowing joins with server tick_us. Do not assume this shared epoch for an
unverified Wine implementation. DXMT's GetTickCount64 timestamps are coarser;
prefer server grant/release boundaries and fence identity for precise joins.

Build the existing openvr-shim directory. The explicit target
`openvr_trace_forwarding_test` verifies all four wrappers retain arguments,
return codes and pose values; it runs in an isolated Wine prefix and writes
its trace beneath Wine's Z:\tmp mapping. Controlled 25 ms pose waits and a
5 ms submission verify nonzero duration records. The test passed on
2026-10-11. Hardware tracing is pending because the first attempt lost the
PS VR2 display before Home launched. Valve's original DLL was restored and
hash-verified. Evidence: Monado `doc/results/openvr-owned-trace-20261011/`.

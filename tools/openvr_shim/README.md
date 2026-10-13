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

Records cover WaitGetPoses, GetLastPoses, GetLastPoseForDevice, Submit and
PostPresentHandoff, with PID/TID, begin/end microseconds, call duration,
eye/texture and error. The five-second log also counts the application's
PostPresentHandoff calls.

The application keeps SteamVR's sync texture from its last Submit until it
calls PostPresentHandoff or its next WaitGetPoses, and the compositor waits
for it meanwhile. SteamVR Home only releases it in WaitGetPoses, 4 to 7 ms
after its last Submit. `MWXR_OPENVR_SHIM_HANDOFF=1` makes the shim call
PostPresentHandoff itself, on the submitting thread, as soon as both eyes of a
frame are submitted without error, unless the application has already called
it for that frame (WaitGetPoses starts a new frame). Those calls are traced as
`InjectedPostPresentHandoff`. It is an experiment, off by default: Valve's
contract is that the application has finished all rendering by then, and Home
may still draw its desktop window afterwards.
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

The same fixture covers the handoff (passed 2026-10-13): no handoff after a
failed Submit, one injected handoff once both eyes are submitted and none for
a repeated eye, none after the application's own handoff (which is forwarded
once), and a new one in the next frame.

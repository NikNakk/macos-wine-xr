# Minimal presentation-baseline synchronization build, 2026-10-10

Built from DXMT `steamvr-in-process` commit `756a657` with protected-context
Signal/Wait locks and bounded ReleaseSync phase diagnostics. The reproducible
patch is in DXMT `docs/patches/steamvr-in-process-sync-diagnostics.patch`.

- `interop.txt`: production D3D11/Metal fence and shared-texture probe, zero failures.
- `window.txt`: two-window DXGI create/present/resize/destroy passed.
- `keyed-gpu.txt`: 200 alternating GPU frames passed between two devices in one process.
- `keyed-gpu-test.cpp`: standalone source for the last test; it does not test cross-process ownership.
- `installed-manifest.json`: matching runtime and prefix modules, verified after installation.

The graphics probe source is in `tests/in_process/graphics_interop_probe_*`;
the window fixture is `tests/windows/dxmt_window_probe.cpp`. The runtime was
staged with DXMT `tests/remote-presentation/stage_runtime.py`. These isolated
tests do not establish headset recovery. Previous installed files are backed
up in `/tmp/monado-sync-minimal-baseline-backup`.

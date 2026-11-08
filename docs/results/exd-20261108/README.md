# EXD simulated-headset measurements, 2026-11-08

Steam was restarted cleanly through Steam Play. Its first restart via
LaunchServices still passed `XRT_MACOS_CLIENT_COMPOSITOR=1`, despite launchd
reporting 0. That first EXD run fell back to service composition because no
PS VR2 display was connected and was excluded from the comparison.
Steam Play was then launched directly with an explicit environment value of
0. Steam's captured setup environment and the game's startup log confirmed 0.

Both measured runs used EXD 1.1.5, DX11, direct OpenXR, the existing game
settings and arguments, and Monado's simulated HMD with its 60 Hz debug-image
target. The baseline was launched by Steam; the scale comparison used the same
rig game runner against the running Steam client with an explicit scale
override. No graphics settings or save files were manually changed.

Each result excludes the first 30 seconds of delivered frames and measures
the next 60 seconds from Monado's `delivered` event timestamps.

| Metric | Runtime default scale | Explicit runtime scale 70% |
| --- | ---: | ---: |
| Submitted texture size (both eyes) | 1792×1008 | 1256×704 |
| Delivered frames/s | 59.77 | 59.75 |
| Median frame interval | 16.67 ms | 16.67 ms |
| p95 interval | 17.16 ms | 17.26 ms |
| p99 interval | 18.39 ms | 18.91 ms |
| Maximum interval | 41.57 ms | 41.24 ms |
| Intervals over 25 ms | 16 | 15 |
| Intervals over 50 ms | 0 | 0 |
| `vk_cmd_submit_locked failed: VK_NOT_READY` in game log | 0 | 0 |

Both runs reached the simulated target's cap. The lower scale demonstrably
changed the submitted texture size, but cannot show a throughput improvement
when both are capped. The old `-vrscale 70` remained in both launches; its
effect is still unverified. The actual per-pass render sizes were not captured.

The Mac was locked throughout, so the startup scene could not be inspected
or navigated. These are unattended startup measurements, not a gameplay or
physical-headset benchmark. The debug-image target also does not establish
actual display presentation cadence. A real headset uses much larger render
targets; these results cannot establish 90 fps capability or prove the old
hosted-compositor problem is fixed.

`gpu_actual_ms_*` in the JSON files are Monado's interval from frame delivery
to observed GPU completion; they are not a hardware GPU execution profile.

Source CSVs are archived as gzip files, with game logs and JSON summaries.
To recompute from an original trace:

```sh
python3 docs/results/exd-20261108/summarize.py PATH_TO_APP_PACING_CSV
```

EXD was stopped after both samples; Steam remains running with the explicit
service-compositor environment. Saved Steam launch options were left unchanged.

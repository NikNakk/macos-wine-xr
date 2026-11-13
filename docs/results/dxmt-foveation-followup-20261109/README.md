# DXMT fixed base-render follow-up — 2026-11-09

[Assessment and reproduction](../../dxmt-base-render-foveation.md).

The automatic experiment uses an unchanged ordinary D3D11 executable with no
native/DXMT/foveation calls. It selects a shader-whitelisted single clear/draw
pass, corrects `SV_Position` during translation, renders with a Metal rate map
and reconstructs before ordinary consumers. It remains opt-in, colour-only,
single-sample and fixed-centre. It is not EXD or general engine compatibility.

- `automatic/`: complete final matrix, JSON and CSV. Baseline 3.651500 ms;
  automatic map **including reconstruction** 1.020833 ms. Each run uses 24
  independently timestamped single draws, discards 3, takes median21. Disjoint
  status/frequency are checked. All cases validate 4,194,304 ordinary-consumer
  pixels within 3/255 per-channel tolerance. Enabled log requires exactly 24
  mapped render passes and 24 automatic unwarps. Eight fallback/default controls
  require zero of each. Metal validation enabled throughout.
- `dxmt-auto-{baseline,enabled}-{2,3}.log`: earlier independent A/B repeats.
- `dxmt-final-position-{512,2048}.log`: explicit component timings and ordinary
  `Load` readback. Four 2048² cases check 16,777,216 pixels, zero outside tolerance.
  Final ordinary scene 3.436917 ms; mapped scene 0.856417 + unwarp 0.075750 ms.
  This is a different synthetic shader/scheduling path from the automatic test;
  do not use it to decompose the automatic timing row.
- `dxmt-final-{target,identity}-control.log`: preserved per-target interop path
  and unmapped translated position control, passing.
- `dxmt-final-position-negative.log`: expected exit1 with correction disabled;
  reconstruction alone cannot repair physical-coordinate shader calculations.
- `dxmt-final-ngx-control.log`: packed depth/stencil ROI copy; both-eye distinct
  colours; output padding; display-resolution motion; foreign parameter proxy;
  invalid origins rejected without output writes. Passing.
- `dxmt-final-ngx-negative.log`: expected exit1 with `DXMT_NGX_SUBRECTS=0`;
  detects the legacy input/output ROI contract violation.
- `EXD-Win64-Shipping_{nvngx,d3d11}.log.gz`: isolated simulated-headset capture
  `scene-20261109-080909`, earlier ROI-only build before position/automatic work.
  NGX diagnostics sample first32/every512 evaluations, through seq6144. Exact
  build hashes for that earlier runtime were not captured; do not attribute it
  to the final automatic-prototype DLL hashes. No game binaries/saves included.
- `exd-roi-summary.json`: sampled ROI shapes, not a count of all evaluations.
  Game-side Metal validation was off due to the retained pre-existing blit
  assertion; these traces are call/provenance diagnostics, not quality passes.
- Build logs and `manifest.json`: final source, patch, binary and evidence hashes.
  All seven ordered patches applied to the pinned base and reproduced 306 src
  files byte-for-byte. There are no upstream commits or installed-runtime edits.

Hardware/OS: M5, macOS27.0.1 (26A434). The workload is a position ramp with 128
sine iterations. Timing comparisons demonstrate a shading/reconstruction
mechanism, not a meaningful EXD scene benchmark. CPU submission figures exclude
worker work and resource/PSO creation; no total CPU overhead is claimed. The
prototype adds a full-size 16MiB hidden target and ~24KiB LUTs at2048², not smaller
physical allocations. EXD depth/MSAA, MRT/derivatives, stereo/temporal/UI quality,
live gaze and A/B/C/D game GPU/FPS comparisons remain open.

The earlier 64-opaque-draw timing method and its net-loss conclusion are retired;
see the warning in the [historical evidence](../dxmt-base-foveation-20261109/README.md).

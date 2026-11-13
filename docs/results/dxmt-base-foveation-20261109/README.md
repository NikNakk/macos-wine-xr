# DXMT base-render foveation evidence — 2026-11-09

**Historical, superseded results.** The 64-opaque-draw timing method cannot
estimate single-draw costs on a tile-based renderer; its performance values and
net-loss conclusion are retired. The shader-coordinate counterexample remains
valid without the later correction. See the [follow-up](../dxmt-foveation-followup-20261109/README.md)
for corrected timings, automatic reconstruction and NGX ROI results.

See [the assessment](../../dxmt-base-render-foveation.md) for conclusions,
source revisions, reproduction and limitations.

- `base-foveation-final-position-control.log`: same SV_Position shader without
  rate maps; expected exit 0.
- `base-foveation-final-position-reconstructed.log`: mapped render followed by
  ordinary-coordinate reconstruction; expected exit 1, demonstrating the
  unresolved shader-coordinate problem.
- `base-foveation-final-target-regression.log`: existing target-specific map
  retention/disable/unrelated-target regression; expected exit 0.
- `base-foveation-final-timing.log`: D3D11 scene GPU timestamps and native GPU
  unwarp timing, pixel checks and partial CPU submission measurements.
- Earlier logs retain the two initial timing runs and initial position failure.
- `exd-classic-scene.log.gz`: bounded 200,000-event EXD capture, Classic DLSS,
  private simulated headset, game-side Metal validation off. Contains NGX
  subrects and resource/pass provenance. No per-pass GPU durations.
- `exd-summary.json`: analyzer output including explicit attribution caveats.
- `exd-metal-validation-failure.log`: validation assertion from the first
  attempt, before the longer diagnostic capture; it is not a passing run.
- Build logs distinguish helper/probe compilation from private DXMT compilation.
- `manifest.json`: artifact hashes and base revisions; `reference-revisions.json`
  pins the source audits. No copied game binaries or saves are included here.

The microbenchmark's 64 repeated synthetic colour draws are not an EXD scene.
Its retained logical-sized allocation is not a compressed-allocation memory
benchmark. CPU numbers exclude worker-thread encoding and resource creation.
A/B/C/D game comparisons, real-headset/gaze validation and image-quality cases
(edges, occlusion, SSR/SSAO, UI, stereo/temporal stability) remain unmeasured.
No transparent foveation switch or FPS improvement is claimed.

# Depth/MSAA evidence — 2026-11-09

[Assessment, implementation boundaries and reproduction](../../dxmt-depth-msaa-foveation.md).

- `primary/`: packaged final 512²/1024² × 2×/4× test matrix, JSON and CSV, plus
  the expected diagnostic-disabled import rejection. Each case has three
  identity round-trip controls, bit-identical depth-only replay, 24 explicit
  mapped render passes, three reconstructed-depth counterexamples, and three
  exact-depth/colour-only alternatives. Metal validation enabled throughout.
- `repeats/`: initial final-shader 1024² runs plus two independent repeats per
  sample count. Combined with `primary/`, four trials define the reported range.
- `all-summary.{json,csv}`: combined primary and repeat measurements. Sums are
  **separately measured component medians**, not a single joined GPU frame.
  No EXD, MetalFX, total CPU, resident-memory or headset FPS claim is made.
- `images/`: native-size 1024²/2× screenshots after the ordinary depth-tested
  overlay and resolve. Difference masks are orange where RGB differs by more
  than 3/255 from baseline. Full-rate centre is correct; peripheral strips and
  silhouettes remain visibly degraded. These are synthetic, monocular fixtures.
- `automatic-after-depth/`, target and NGX regression logs: existing opt-in
  colour-only automatic path, target-specific map interface and NGX ROI probe
  still pass with the new attachment diagnostic disabled.
- Build/patch-validation records and `manifest.json`: final source, patch,
  fixture, helper, runtime and evidence hashes. All eight patches apply to pinned
  DXMT 4c3a2f5ea661bd258c14cf8ccb6924791cc75a77 and reproduce 306 src files.

Controls are required to pass; mapped-depth mismatches are deliberately measured
and **not accepted as correct semantics**. Exact-depth alternatives require zero
per-sample depth/coverage/threshold errors but still have peripheral colour error.
The production automatic profile remains single-sample/colour-only.

1024² four-trial ranges: baseline 2× 1.048917–1.054793ms, bilinear colour + exact
full depth 0.602542–0.804916ms; baseline 4× 1.056833–1.073833ms, corresponding
alternative 0.945501–1.237877ms. Do not select only the fast4× trial.

The fixture's trivial VS and synthetic 128-sine PS favour depth-only replay;
real alpha/discard/custom-depth/coverage/stencil/side-effect shaders need explicit
analysis. There is no automatic replay or EXD compatibility profile in this work.

# Resolve-first and rate-sweep evidence — 2026-11-09

[Assessment, controls and implementation limits](../../dxmt-resolve-first-foveation.md).

- `primary/`: 16 final 1024² cases, peripheral axis rates 0.25/0.5/0.75/1.0,
  2×/4× MSAA, normal/reversed scene order. Includes JSON/CSV. Metal validation on.
  Every case checks 1,048,576 ordinary destination pixels, all original depth
  samples, immutable physical MSAA source contents and 24 nonzero-map passes.
- Full-rate and unmapped controls have zero colour/source-layout differences.
  All rates retain bit-identical full-resolution depth and correct centre output.
- For rate<1, raw MSAA logical source reads intentionally fail against baseline:
  this demonstrates the missing source-materialization contract. Passing the
  experiment does **not** label arbitrary application source accesses compatible.
- `images/`: native-size ordinary resolved scene, each rate, difference masks,
  and nearest-neighbour 3× crops of thin peripheral strips. Orange marks >3/255
  RGB differences. Rate 0.5 still loses the thinnest strips; 0.75 is closer.
- `fixture-change.patch` and `previous-fixture-source/`: exact changes from the
  preceding depth/MSAA fixture. The paired helper ABI changed; rebuild both ends.
  No additional DXMT production-source patch was needed.
- Regression logs retain the previous per-sample depth, target-specific and NGX
  ROI checks. All passed. Build/patch checks and `manifest.json` identify the
  actual source, helper/executable, runtime and evidence hashes.

Native resolve-only/unwarp-only/joined phases rotate order, discard three warmup
rounds and take median21. Resolve+unwarp runs in one native GPU command buffer.
Scene and depth replay remain independent D3D component medians. Their sum is
**not a joined whole-frame timing**. Baseline native resolve uses the matching
native timing method; the D3D-query resolve duration is diagnostic only.

At rate 0.5, two-order component totals are 0.642–0.644ms (2×) and 0.693–0.695ms
(4×), versus 1.009–1.062ms baseline. The full-rate control costs more, as expected.
The 0.25 case is order-sensitive; do not cherry-pick its faster run. These are
synthetic 21-triangle/128-sine shader results, not EXD/stereo/gaze/CPU/MetalFX/FPS
measurements. No game profile or installed runtime/service was changed.

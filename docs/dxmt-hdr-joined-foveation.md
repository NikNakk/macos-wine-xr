# Joined HDR foveation with exact stencil replay

9 November 2026. Follow-up to the [EXD replay audit](dxmt-exd-replay-audit.md).

**The complete restricted four-traversal path retains a net GPU benefit with stencil preservation included.** At peripheral axis rate 0.5, median paired reductions are about 35% at 2× MSAA and 24% at 4×. The gentler 0.75 rate saves about 13% at 2×, but only ~0.7% at 4×, which is not a useful demonstrated margin. This remains a synthetic mono fixture, not an EXD or headset FPS result.

The explicit coverage-only replay shows no consistent practical advantage over the original colour shader with colour writes disabled. No game-shader rewrite or capture/replay feature was implemented. All changes in this stage are confined to the fixture, analysis and documentation; private DXMT patches remain 0001–0013.

The [MSAA source-materialisation follow-up](dxmt-msaa-source-foveation.md) now
includes eager per-sample writeback and ordinary MSAA read/load consumers in the
measured path. It retains a smaller net benefit in the restricted fixture.

## What is measured

The baseline sequence contains:

1. Ordinary masked depth prepass.
2. Ordinary HDR colour shading with depth read-only and stencil writes.
3. Native hardware MSAA colour resolve.
4. A stencil-tested HDR colour consumer reading the conventional resolved texture.
5. Native resolve of that consumer's MSAA output.

The foveated sequence retains the ordinary prepass and adds a mapped depth prepass, mapped HDR colour shading, an ordinary full-resolution stencil-only replay, and colour reconstruction before the same consumer. These are **four scene-geometry traversals**, versus two for baseline. Reconstruction and the final consumer also use fullscreen triangles.

All colour targets use `R11G11B10_FLOAT`; depth/stencil uses combined D32Float/Stencil8 with read-only-depth views during main shading/replay. Main stencil replaces the high nibble, preserving unrelated low bits; the later consumer compares `0x81`. MSAA colour resolves are native Metal operations through D3D11 `ResolveSubresource`.

Reconstruction uses an ordinary fullscreen D3D11 pixel shader writing R11G11B10 HDR. This avoids assuming typed UAV writes to that format. It reuses the existing rate-map coordinate export, cached clamped axis LUT and bilinear mapping. The consumer reads the resulting ordinary single-sample texture through `Texture.Load`, then applies the exact ordinary stencil buffer while rendering to its own MSAA HDR target.

### Replay comparison

Three modes are recorded:

- **Baseline** as above.
- **Original shader replay:** the original fixture colour shader runs with render-target colour writes disabled; discard/coverage and stencil behaviour remain intact.
- **Coverage-only replay:** a hand-written control using the fixture's identical discard/coverage function, with no colour calculation. This is not a DXMT-generated variant of an arbitrary game shader.

The fixture adds a 64² immutable R8 alpha texture, linear `SampleBias`, discard and explicit coverage output for the masked material. There is only one mip; mip-selection/derivative behaviour across a real game's texture hierarchy is not established. Colour shading has a synthetic 128-sine workload independent of coverage. The vertex shader and four-triangle scene are intentionally small.

## Correctness gates

Before and after timing, all 16 final cases require:

- Every ordinary retained depth sample and every stencil sample equal the independent one-pass reference.
- Mapped prepass/main results equal the mapped one-pass reference in the active physical layout, including HDR colour, depth and stencil.
- Baseline and full-rate reconstructed outputs equal the independent final reference bit for bit.
- Original-shader and coverage-only replay produce identical final images.
- HDR values remain above 1 through resolve, reconstruction and the consumer.
- **Direct per-sample readback of the actual timed consumer output** matches the independent stencil predicate exactly. This is supplemented by a separate marker draw; the result does not rely only on that marker.

All these gates pass with Metal validation enabled. The central ROI has zero out-of-tolerance colour pixels in the reduced-rate cases. Peripheral colour remains approximate and is reported separately below.

## Timing protocol

M5 / macOS 27.0.1, logical 1024², 2×/4× MSAA, rates 1, 0.5 and 0.75. Each process records 65 rounds containing all three modes in rotating order. The first 24 rounds are warm-up; 41 remain for measurement. Reduced rates have three process trials, including reversed starting order; full-rate controls have two.

One D3D11 GPU timestamp interval brackets each **complete sequence**, including prepasses, stencil replay, native resolves, HDR reconstruction and the stencil-tested colour consumer. There are no CPU completion waits/readbacks, native helper calls or phase timestamps inside that interval. Fixture shaders/resources/PSOs are prepared beforehand; normal driver uploads, barriers and queue behaviour are included. Timestamp frequency, disjoint state and ordering are checked.

The primary runs disable audit logging. A separate run confirms **269 actually mapped render passes**, including reference/control work and both foveated timing modes. It is excluded from the performance table.

The table includes ranges of mode-wise whole-sequence medians across final trials. Its reduction column uses each trial's **median paired-round percentage**, which is more informative when timings co-vary. It is not a sum of component medians or a claim that all passes execute simultaneously.

| MSAA | Axis rate | Baseline whole-sequence median range | Original-shader replay path | Median paired reduction |
|---|---|---:|---:|---:|
| 2× | 0.5 | 1.150–1.322 ms | 0.743–0.845 ms | **35.3–35.5%** |
| 2× | 0.75 | 1.151 ms | 1.002–1.003 ms | **12.8–13.0%** |
| 4× | 0.5 | 1.268–1.269 ms | 0.963–0.966 ms | **24.1–24.2%** |
| 4× | 0.75 | 1.267–1.268 ms | 1.257–1.263 ms | ~0.7%, effectively a tie for this investigation |
| 2× | 1.0 control | 1.151–1.152 ms | 1.297–1.303 ms | **12.5–13.1% slower** |
| 4× | 1.0 control | 1.267 ms | 1.532–1.565 ms | **23.6% slower** |

One 2×/0.5 trial runs slower in both baseline and foveated modes; all trials are retained. Some 4× timing distributions are also broad. The archived raw intervals and p10/p90 values expose this variation rather than discarding selected rounds. No clock state or cause was measured.

Across reduced-rate trials, the paired median original-minus-coverage replay difference ranges approximately −1.1 to +4.0 microseconds, changing direction. This provides **no consistent material reason to build a coverage-only rewrite for this fixture**. It is consistent with the resulting pipeline avoiding unused colour work, but timing alone does not prove compiler dead-code elimination or establish the behaviour of EXD's actual shaders.

These percentages must not be compared directly with the earlier ~43–45% D32/RGBA8 fixture as an isolated stencil cost. This experiment also changes scene, alpha sampling, formats, reconstruction and the consumer. Its own baseline is the valid comparison.

## Image quality

Final image error is measured in linear HDR values, after the stencil-tested consumer. A pixel is flagged if any channel differs by more than `max(0.03, 0.02 × abs(reference))`. This threshold differs from the earlier 8-bit RGB tests; the percentages are not directly interchangeable.

| MSAA | Rate | Flagged final pixels / 1,048,576 | Mean absolute linear RGB error |
|---|---|---:|---:|
| 2× | 0.5 | 10,686 (1.02%) | 0.002396 |
| 2× | 0.75 | 7,591 (0.72%) | 0.001360 |
| 4× | 0.5 | 11,192 (1.07%) | 0.002965 |
| 4× | 0.75 | 7,169 (0.68%) | 0.001296 |

Stencil activation remains exact: foveated colour approximation does not alter the consumer's permitted sample mask. Image inspection shows edge/band colour differences within that mask. The black region outside it is intentional stencil rejection, not missing rendering from a stale stencil buffer.

[Reference preview](results/dxmt-hdr-joined-20261109/images/2x-baseline.png), [0.5 preview](results/dxmt-hdr-joined-20261109/images/2x-r050.png), [0.5 HDR-error mask](results/dxmt-hdr-joined-20261109/images/2x-r050-difference.png). Previews use the fixed mapping `c/(1+c)`; they are not raw HDR images or a perceptual/headset quality validation. Orange masks use the linear-HDR threshold above. Stereo, eye motion, temporal effects and UI/text remain untested.

## The next EXD boundary

Re-examining the existing bounded EXD trace confirms that the selected main MSAA colour allocation is **not finished after its first single-sample consumer**:

- In frame 1200, later shaders read it and another 2× MSAA pass loads it for 13 draws.
- In frame 1201, later shaders read it, one 2× pass loads it for 12 draws, and another loads it for one draw.

[Resource/allocation provenance](results/dxmt-hdr-joined-20261109/exd-colour-boundary.json) records these uses and labels. Their exact visual purpose is not inferred. A single-sample reconstructed output cannot preserve a later ordinary MSAA source load on its own.

**The next bounded experiment should therefore include full MSAA HDR colour materialisation back into ordinary coordinates**, with exact depth/stencil retained and an ordinary post-materialisation MSAA read/load as a required consumer. Measure that complete cost before building game capture/replay infrastructure. Eager materialisation is a smaller initial contract than a general lazy representation system; later optimisations can defer it only when every intervening use is understood.

The original mapped MSAA source, aliases and exports remain unresolved in the current resolve-first fixture. Real game capture must also preserve input/constant-buffer versions, instancing, draw arguments, queries/predication and side effects. EXD's sampled prepass/vertex work is much larger than this fixture. No EXD draw was redirected and no EXD speedup/FPS claim follows from these results.

## Reproduction and regressions

```sh
# Same prepared paired runtime/build variables as the earlier depth fixture.
scripts/build-dxmt-depth-foveation-probe.zsh
scripts/test-dxmt-hdr-joined.zsh
scripts/run-dxmt-depth-foveation-probe.zsh 2 1024 --joined-hdr 0.5 --reverse
python3 -m unittest discover -s tests/macos -p '*hdr_joined*.py'
```

[Curated results, raw trial rows, images and hashes](results/dxmt-hdr-joined-20261109/summary.json) include all final cases and the independent map audit. The existing six-case HDR/stencil suite and 16-case original joined-prepass suite pass after the shared scene extension. New analyser tests reject stencil/consumer mismatches, replay differences, missing intervals and incorrect warm-up selection. The unchanged private DXMT source still matches its pinned base plus thirteen patches. Steam, EXD and installed runtime settings were not changed.

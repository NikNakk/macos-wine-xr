# Eager MSAA HDR source materialisation

9 November 2026. Follow-up to the [joined HDR/stencil experiment](dxmt-hdr-joined-foveation.md).

**Writing foveated colour back into ordinary MSAA coordinates retains a net GPU benefit in the complete restricted fixture:** about 28% at 2× MSAA and 10% at 4× at peripheral axis rate 0.5. At rate 0.75 the margin is only 5–7% at 2×, and 4× becomes about 11% slower. These are synthetic mono results, not EXD or headset FPS measurements.

The materialised destination is the original D3D11 texture, not a replacement object. Independent typed views, an ordinary `Texture2DMS.Load` shader and a later render pass loading that texture all observe consistent data. Exact ordinary depth/stencil remain intact. No game capture/replay, generic resource-version system or automatic EXD profile has been enabled.

## Why the previous resolved output was insufficient

The [existing EXD colour provenance](results/dxmt-hdr-joined-20261109/exd-colour-boundary.json) shows shader reads and subsequent render-pass loads of the same MSAA colour allocation after the selected main pass. Resolving and reconstructing only a single-sample image leaves that original source in the wrong representation for those uses.

This experiment tests **eager** colour materialisation at the end of the selected pass. It requires no pending mapped representation after that boundary: the original single-mip typed colour source is again ordinary in layout. More general aliases, renames, exports and deferred-context lifetimes remain separate implementation work.

## Implementation and reused infrastructure

The [new source helper](../tests/windows/dxmt_msaa_source_probe.h) extends the existing HDR/stencil fixture. Private DXMT production source and patches 0001–0013 are unchanged in this stage.

The native test helper's existing per-sample map-coordinate generation is now shared with a CPU-only export operation. It queries the same Metal default sample positions and logical-to-physical map used by the earlier depth experiment. Each sample has its own axis coordinates and offset. The D3D11 reconstruction shader:

1. Uses `SV_SampleIndex` to preserve sample-plane identity.
2. Maps that logical sample position to the corresponding physical location.
3. Bilinearly reads four neighbours from **that same MSAA sample plane**.
4. Writes colour into the original `R11G11B10_FLOAT` MSAA render target with no depth/stencil attachment.

It never resolves/broadcasts the samples to perform this writeback. Depth/stencil are not reconstructed. The original full-resolution depth prepass and exact stencil replay remain part of the path.

Two separately created SRV/RTV objects reference the original texture; `GetResource` confirms object identity. The original SRV supplies the source checks, while the alternate views supply the timed shader read and render-pass load. The descriptor retains its dimensions, sample count, format, mip and array layout. These are typed views of one subresource, not a claim about arbitrary reinterpretation or shared-memory aliases.

## Complete timed sequence

Both baseline and foveated paths perform the following ordinary subsequent uses:

- A fullscreen shader reads all original MSAA sample planes and produces a single-sample HDR snapshot.
- A later render pass **loads the same original MSAA colour target**, samples that snapshot, and additively updates a bounded region under stencil comparison `0x81`. It does not clear the target.
- Hardware resolve and the existing stencil-tested HDR consumer follow, including a final hardware resolve.

Baseline renders the ordinary masked depth prepass and main colour/stencil pass before these uses. Foveation renders the retained ordinary prepass, hidden mapped prepass and main shading, exact ordinary stencil replay, then per-sample colour materialisation before the same uses. The previous separate single-sample unwarp is unnecessary after source materialisation and is not also charged.

A D3D11 timestamp pair brackets each **whole sequence**. There are no fixture readbacks, CPU completion waits, resource/shader construction or native helper calls inside it. Driver uploads/barriers and normal queue behaviour remain included. Correctness inspections run separately before and after timing.

## Correctness and source-reuse controls

All 16 final cases pass with Metal validation enabled. They cover logical 1024², 2×/4× MSAA, peripheral rates 1, 0.5 and 0.75, repeated trials and reversed starting order.

The checks establish:

- Full-rate pre-load MSAA colour is bit-identical per sample to ordinary rendering; the full-rate final consumer image is also bit-identical.
- Reduced-rate materialised colour follows an independent CPU bilinear reference evaluated from the mapped sample planes and queried coordinates. Comparison allows `max(1e-5, abs(expected)/32)` for HDR format/interpolation quantisation. This is not a claim of lossless peripheral colour.
- The alternate SRV reads the same pre-load colour/depth values bit for bit.
- The ordinary MSAA snapshot shader agrees with the CPU average within the HDR quantisation tolerance.
- The subsequent load/blend updates agree with the expected prior colour plus snapshot contribution within that tolerance.
- **Untouched samples remain bit-identical**, including samples rejected by stencil.
- Ordinary depth and stencil remain exact, and direct per-sample activation of the final timed consumer matches the independent stencil predicate.

The load pass modifies 157,478 samples and preserves 1,939,674 at 2×. At 4× it modifies 266,144 and preserves 3,928,160. These controls rule out a trivial no-write or whole-target-clear substitute.

A deliberate negative control averages the ordinary source then broadcasts that result into an MSAA texture. Even in full-rate identity cases, it produces 217,122 out-of-tolerance sample differences at 2× and 434,884 at 4×. Separate sample-plane witnesses are nonzero. Thus these controls distinguish genuine per-sample materialisation from resolve-and-broadcast.

A separate audit confirms **135 actually mapped render passes** across reference, correctness and timing work. Materialisation and subsequent ordinary uses have no rate map attached.

## GPU results

M5 / macOS 27.0.1, Metal validation enabled. Each process records 65 baseline/foveated pairs in alternating order, discards 24 warm-up pairs and measures 41. Reduced rates have three process trials; full-rate controls have two. Primary audit logging is disabled.

GPU columns are ranges of whole-sequence medians across trials. The last column is each trial's median paired-round percentage reduction. Raw timings, p10/p90 values and CPU recording durations are retained; CPU recording can include queue backpressure and is not pure CPU work.

| MSAA | Axis rate | Ordinary baseline | Foveated + MSAA source materialisation | Median paired reduction |
|---|---|---:|---:|---:|
| 2× | 0.5 | 1.212–1.213 ms | 0.872–0.873 ms | **27.7–28.1%** |
| 2× | 0.75 | 1.207–1.218 ms | 1.126–1.141 ms | **5.4–7.3%** |
| 4× | 0.5 | 1.380–1.382 ms | 1.233–1.235 ms | **10.3%** |
| 4× | 0.75 | 1.382–1.384 ms | 1.530–1.532 ms | **10.8–11.1% slower** |
| 2× | 1.0 control | 1.214–1.215 ms | 1.417–1.418 ms | **17.0% slower** |
| 4× | 1.0 control | 1.380–1.381 ms | 1.839–1.846 ms | **33.4–34.2% slower** |

The results include the full writeback and later source uses; they are not component-median sums. The source-restoration cost narrows the useful envelope, especially at 4×. Materialisation writes at the full destination sample count even though shading used a smaller physical region. No DRAM counter measurement or specific bandwidth-bottleneck attribution is made.

Do not subtract these numbers from the earlier experiment and call the difference isolated materialisation time: this workload also adds the ordinary snapshot and load/blend consumer. Its own baseline is the valid comparison.

## Image quality and memory scope

After the ordinary load/blend and final consumer, reduced-rate colour is still approximate. The existing linear-HDR threshold is `max(0.03, 0.02 × abs(reference))` per channel.

| MSAA | Rate | Flagged pixels / 1,048,576 | Mean absolute linear RGB error |
|---|---|---:|---:|
| 2× | 0.5 | 11,021 (1.05%) | 0.002449 |
| 2× | 0.75 | 7,690 (0.73%) | 0.001421 |
| 4× | 0.5 | 11,462 (1.09%) | 0.003056 |
| 4× | 0.75 | 7,252 (0.69%) | 0.001346 |

The central ROI has zero flagged pixels. Source layout/view/load compatibility is distinguished from this expected colour loss. Text/UI, temporal behaviour, stereo and eye movements remain untested.

[Reference preview](results/dxmt-msaa-source-20261109/images/2x-baseline.png), [0.5 preview](results/dxmt-msaa-source-20261109/images/2x-r050.png), [0.5 error mask](results/dxmt-msaa-source-20261109/images/2x-r050-difference.png). Previews use `c/(1+c)`; orange masks use the linear-HDR threshold. The black region outside the stencil mask is intentional rejection. Inspection still shows peripheral colour/edge changes within the mask.

All render allocations remain logical-sized. The sample-axis buffer is about 16/32 KiB at 2×/4×; the ordinary read snapshot is 4 MiB. The negative control adds a scratch MSAA colour texture used outside timing. These are nominal resource payloads, not measured resident memory or a claim of allocation savings.

## Integration decision

The eager one-source boundary now has a tested representation contract and measurable net benefit in this restricted scene. **2× MSAA at rate 0.5 is the stronger initial candidate**; the 4× margin is smaller and the gentler 4× case should not be treated as beneficial.

The next implementation step is a bounded opt-in DXMT pass capture/replay prototype, initially for a controlled immediate-context workload. It should retain original shaders and masked coverage, use the measured exact-stencil path, materialise colour eagerly into the original allocation, and fall back on any unsupported state or resource dependency. It must capture draw arguments, state and **resource versions**: holding a COM resource reference alone does not preserve mutable buffer contents.

Only after those controls survive an unmodified D3D11 application should an EXD compatibility profile be attempted. Its larger geometry/vertex work, shader derivatives, input updates, predication/queries, resource aliases/exports and stereo layout can erase the measured margin or require fallback. This fixture does not implement that recorder or establish those behaviours.

## Reproduction and regressions

```sh
# Existing prepared runtime and SDK/build variables from the depth experiment.
scripts/build-dxmt-depth-foveation-probe.zsh
scripts/test-dxmt-msaa-source.zsh
scripts/run-dxmt-depth-foveation-probe.zsh 2 1024 --msaa-source 0.5 --reverse
python3 -m unittest discover -s tests/macos -p '*hdr_joined*.py'
```

Rebuild the paired helper and executable together. The CPU-only sample-axis operation is appended to the existing helper; previous operations and parameter fields are preserved. The earlier native reconstruction now calls the same shared coordinate generator, with its existing depth/MSAA and resolve-first regression suites rerun successfully. The 16-case previous HDR/stencil timing suite also passes. Four timing/source-analysis tests validate interval completeness, source/alias/load controls and sample-plane negative controls.

[Curated trial rows, controls, images and hashes](results/dxmt-msaa-source-20261109/summary.json) retain all final runs. The private DXMT source remains unchanged at its pinned base plus thirteen patches. No Steam/EXD launch or installed runtime settings changed in this stage, and no game FPS result is claimed.

## Ownership follow-up

The [bounded DXMT draw capture gate](dxmt-draw-replay-ownership.md) now passes conventional replay identity for RGBA8/HDR at 1× and 2×, including mutable input versions and exact depth/stencil. This is the first integration prerequisite; multi-draw pass capture, foveated replacement and complete capture-cost timing remain outstanding.

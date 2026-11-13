# Stay-warped rendering: shader read translation

2026-11-11. Follow-up to the [draw replay ownership gate](dxmt-draw-replay-ownership.md).

**A target rendered with a Metal rate map can stay in its physical layout and be
read correctly by later passes.** DXMT translates the coordinates of shader reads
instead of restoring ordinary layout after the pass. In the fixture a second
mapped pass and two ordinary passes read mapped targets through unchanged D3D11
shaders; all 4,194,304 pixels land within half a physical texel per lookup of
the analytic result.

**In the restricted MSAA/stencil fixture, staying warped saves about 42% of GPU
time at peripheral rate 0.5 and 16–17% at 0.75**, at both 2× and 4× MSAA,
including an EXD-style depth max-resolve pass. On the same build the eager
four-traversal path saves 26% (2×) and 8% (4×) at rate 0.5, 6% at 2×/0.75, and
loses at 4×/0.75. These are synthetic mono results on the M5. Nothing was run
against EXD, and no pass selection exists.

## Why

Every earlier experiment restored ordinary layout at the end of the selected
pass. EXD's later passes then needed exact ordinary depth and stencil, which is
what forced four geometry traversals and draw capture/replay. If the scene
targets stay mapped until a late boundary, each draw is rasterised once and
later consumers read through the map. The missing piece was the read side:
[patch 0006](../patches/dxmt/0006-rate-map-position-translation.patch) corrects
`SV_Position`, but a `Load` or `Sample` of a mapped target still used logical
coordinates on physical storage.

## Implementation

[Patch 0015](../patches/dxmt/0015-rate-map-read-translation.patch), on top of
0001–0014. Opt-in: `DXMT_RATE_MAP_READ=1` together with `DXMT_RATE_MAP_POSITION=1`.

- **Layout tag.** At the end of a render pass that meets the existing
  mapped-position conditions, its colour target records the map's coordinate
  buffer. Those conditions are one colour target at the map's logical size,
  single-sample without depth, or 2×/4× MSAA with D32 or combined D32S8 under
  `DXMT_RATE_MAP_ATTACHMENT_PROBE=1` (which now also admits single-sample
  depth, for depth-only resolve targets). Any other single-target pass clears
  the tag. The depth/stencil texture is tagged the same way, and an ordinary
  pass that can write either plane clears it. A resolve copies the source's tag
  to its destination.
- **Forward tables.** The buffer cached on each map gains the physical position
  of every logical pixel boundary per axis, after the existing inverse tables.
- **Per-texture flag.** When a tagged texture is bound to a pixel shader, the top
  bit of its argument-table metadata (the array-length word) is set and the pass
  binds that map's buffer at fragment slot 9. An unmapped pass can therefore read
  a mapped target.
- **Shader translation.** DXBC→AIR translation of `Load` (2D and 2D-MS) and
  `Sample`/`SampleLevel` (2D) maps logical coordinates to physical ones when the
  flag is set, clamped to the active physical region. It is a real branch on
  the metadata the shader already loaded, so an ordinary texture skips the
  table lookups. Depth and per-sample MSAA reads take the same path. The
  original DXBC is unchanged. The translation has its own variant flag
  (`SM50_SHADER_FLAG_RATE_MAP_READ`), set together with the position flag.
- Passes that read different layouts never merge.

## Fixture

[`dxmt_warp_read_probe.cpp`](../tests/windows/dxmt_warp_read_probe.cpp), 2048²
`R16G16B16A16_UNORM`, the existing helper map (central cells at 1.0, periphery
0.25, physical 1008²). All passes are recorded before the first flush.

| Pass | Map | Reads | Writes |
|---|---|---|---|
| A | mapped | — | scene = logical pixel position |
| B | mapped | scene: `Load`, bilinear `Sample`, `Load` with +7 offset, point `SampleLevel` | chain |
| C | ordinary | chain: `Load` | read back |
| D | ordinary | chain: bilinear `Sample` | read back |

Worst error in logical pixels over every pixel, Metal validation enabled:

| Case | Centre (full rate) | Periphery, final `Load` | Periphery, final `Sample` | Offset `Load` |
|---|---:|---:|---:|---:|
| Read translation on | 0.016 | 1.98 | 1.47 | 3.22 |
| Read translation off (expected failure) | 1151 | 2047 | 2047 | 2038 |
| No map (identity control) | 0.016 | — | — | 0.016 |

A nearest read costs up to half a physical texel, which is 2 logical pixels at
rate 0.25. The offset read adds a second such step. The mean error with the
final bilinear read is 0.01 px. The trace confirms two mapped render passes and
three translated reads in the first two cases, and none in the identity control.
The existing ten-case automatic-foveation suite, the 16-case eager MSAA source
suite and the HDR/stencil and joined-prepass suites still pass on the final build.

## Timed MSAA/stencil chain

The existing [MSAA source fixture](dxmt-msaa-source-foveation.md) gained
`--stay-warped`. Logical 1024², `R11G11B10_FLOAT` colour with combined
depth/stencil, masked coverage shaders. With the flag, both the baseline and
the stay-warped sequence also run a depth max-resolve pass modelled on EXD's,
and the consumer reads its result. The stay-warped sequence rasterises
everything once with the map:

1. Depth prepass and main colour/stencil pass (mapped).
2. Fullscreen read of every MSAA colour sample plane into a snapshot (mapped).
3. A pass that loads the MSAA colour and adds the snapshot under stencil
   comparison `0x81` (mapped), then hardware resolve.
4. Fullscreen maximum of the MSAA depth samples, written with `SV_Depth` into a
   single-sample depth target (mapped).
5. The stencil-tested HDR consumer, which loads the resolved colour and scales
   it by the resolved depth (mapped), then hardware resolve.
6. One ordinary bilinear read of the still-mapped final image.

There is no ordinary prepass, no stencil replay and no per-sample writeback.
Steps 2–6 read mapped targets through DXMT; the fixture supplies no lookup
tables. The test asserts six mapped render passes and six translated reads per
sequence from the trace, and that the same run with `DXMT_RATE_MAP_READ=0`
fails (227,356 flagged pixels, 11,332 in the centre).

Whole-sequence D3D11 timestamps, 65 alternating pairs with 24 discarded, three
trials per reduced rate and two per control, Metal validation enabled. The
eager column is the 16-case four-traversal suite rerun on this build; its
sequence has no depth max-resolve, so compare reductions, not milliseconds.

| MSAA | Rate | Baseline | Stay-warped | Median paired reduction | Eager four-traversal, same build |
|---|---|---:|---:|---:|---:|
| 2× | 0.5 | 1.248–1.250 ms | 0.729–0.730 ms | **41.5–41.6%** | 26.4–26.5% |
| 2× | 0.75 | 1.250 ms | 1.034–1.035 ms | **17.1%** | 5.6–6.0% |
| 4× | 0.5 | 1.443–1.448 ms | 0.838–0.839 ms | **41.7–42.0%** | 8.4–8.7% |
| 4× | 0.75 | 1.441–1.447 ms | 1.208–1.209 ms | **15.8–16.5%** | 12.4–12.7% slower |
| 2× | 1.0 control | 1.249–1.250 ms | 1.389–1.390 ms | 11.3–11.5% slower | 17.6–17.7% slower |
| 4× | 1.0 control | 1.440 ms | 1.626–1.629 ms | 12.6–12.8% slower | 34.8–34.9% slower |

The full-rate control is the overhead of the mechanism when nothing is saved:
six translated reads and the final read pass.

The final image is compared with the baseline's using the existing linear-HDR
threshold. The centre region has zero flagged pixels in every case and the
full-rate controls have none anywhere.

| MSAA | Rate | Flagged pixels / 1,048,576 | Mean absolute error | Eager path flagged |
|---|---|---:|---:|---:|
| 2× | 0.5 | 17,983 (1.72%) | 0.005504 | 11,021 |
| 2× | 0.75 | 11,973 (1.14%) | 0.003623 | 7,690 |
| 4× | 0.5 | 17,849 (1.70%) | 0.005912 | 11,462 |
| 4× | 0.75 | 12,061 (1.15%) | 0.002944 | 7,252 |

Staying warped flags about 60% more peripheral pixels than the eager path. The
eager path keeps exact full-resolution depth and stencil; here stencil edges,
resolved depth and the later passes are also at reduced density.
[Baseline](results/dxmt-warp-read-20261111/images/2x-msaa-source-baseline.png),
[rate 0.5](results/dxmt-warp-read-20261111/images/2x-stay-warped-r050.png),
[difference mask](results/dxmt-warp-read-20261111/images/2x-stay-warped-r050-difference.png).

### Cost of the translation on ordinary reads

The first version was branchless: every pixel-shader texture read also read
the coordinate buffer. On that build the eager suite, which never uses read
translation, fell to 19% at 2×/0.5 and a 2.6% loss at 4×/0.5, and its baseline
rose about 2%. With the branch, the eager suite's baselines are back at their
9 November values (1.213 and 1.386 ms against 1.212 and 1.380 ms), and its
reductions are within two points of that day's (26.4% and 8.5% against 28% and
10%). The remaining gap was not investigated.

### Stale shader cache

An early stay-warped run was wrong because DXMT's on-disk shader cache, keyed
by executable name, held variants compiled before read translation existed and
the position flag alone did not distinguish them. The separate variant flag
fixes that, and it moved to a new bit when the branch replaced the branchless
code. A prefix that ran an intermediate build of patch 0015 cannot pick up
stale variants from it.

## Not covered

- MRT targets, arrays and mips.
- Reads outside pixel shaders: compute, UAV, copies, CPU maps and
  `UpdateSubresource` neither translate nor clear the tag.
- `SampleBias`, `SampleGrad`, comparison samples and `Gather`. Texel offsets on
  `Sample` are applied in physical texels. Wrap and mirror addressing behave as
  clamp.
- Depth is only read with `Load` here. Depth used for comparison sampling or
  for reprojection maths that assumes uniform pixel spacing is untested.
- More than one read layout per pass, reduced-size targets needing their own
  maps, and maps that change between frames (history textures).
- Derivatives and LOD selection inside mapped passes.
- The final read is one bilinear pass in the fixture. In the runtime it would
  be Monado's existing rate-map reconstruction, which was not connected.
- Stereo: one mono target with one foveal region.

## Next step

The margin survives the full restricted sequence without capture or replay,
and untagged reads are free, so the remaining gates are about EXD's real frame:

1. Trace which EXD passes read the scene targets outside pixel shaders
   (compute, copies) or at other sizes. Each needs translation, a matching map
   or a restore point. This decides whether the approach reaches a game at all.
2. A two-peak map on a double-wide stereo target in the fixture.
3. Only then a bounded EXD profile, starting from fixed foveation.

## Reproduce

```sh
# Private source: dxmt-foveation-source plus patch 0015, built into its own directory.
export DXMT_SOURCE_DIR="$PWD/build-in-process/dxmt-warp-source"
export MWXR_WINE_SDK=/Users/nickkennedy/Code/wine-crossover/runtime
export MWXR_WINE_SOURCE=/Users/nickkennedy/Code/wine-crossover/sources/wine
export MWXR_IN_PROCESS_WINE="$PWD/build-in-process/wine-warp"
scripts/build-dxmt-warp-read-probe.zsh
scripts/test-dxmt-warp-read.zsh
scripts/build-dxmt-depth-foveation-probe.zsh
scripts/test-dxmt-stay-warped.zsh
```

`build-in-process/wine-warp` is an APFS clone of the private `exd-ngx/wine`
runtime with the new `d3d11.dll`, `dxgi.dll`, `d3d10core.dll`, `winemetal.dll`
and `winemetal.so`. Xcode's Metal compiler was unavailable to the build, so the
unchanged Metal helpers were reused with `tools/reuse_dxmt_shader_cache.py`.
The installed runtimes, the EXD copies and `dxmt-foveation-source` are unchanged.

`build-in-process/prefix-foveation`, removed in the 10 November cleanup, was
recreated because the existing run scripts hard-code it.

[Read fixture results](results/dxmt-warp-read-20261111/summary.json),
[stay-warped trials](results/dxmt-warp-read-20261111/stay-warped-summary.json),
[eager path on this build](results/dxmt-warp-read-20261111/four-traversal-same-build.json),
[hashes](results/dxmt-warp-read-20261111/sha256.txt).

# DXMT depth/MSAA foveation experiment

2026-11-09. The next compatibility gate is tested, with a concrete result:
**Metal accepts mapped D3D11 colour+depth with 2×/4× MSAA, but simply reconstructing
that depth changes subsequent D3D11 depth tests.** Keeping exact full-resolution
depth through a separate depth-only replay removes those depth errors for this
restricted shader. Peripheral colour and thin-geometry errors remain.

The [resolve-first/rate-sweep follow-up](dxmt-resolve-first-foveation.md) now tests
the next colour boundary and identifies the remaining source-representation gate.

This is an explicit diagnostic, **not an extension of the automatic profile**.
The automatic colour-only prototype still rejects depth and MSAA. No installed
Steam/runtime/service settings or game files were changed.

## Implementation and controls

[Patch 0008](../patches/dxmt/0008-explicit-depth-msaa-diagnostic.patch) adds a small,
default-off `DXMT_RATE_MAP_ATTACHMENT_PROBE=1` exception to the existing explicit
map experiment. It allows one colour target plus D32Float depth, 2×/4× MSAA,
one layer, and the existing inverse-coordinate binding. Its native-import
exception is restricted to single-layer RGBA8/R32_TYPELESS MSAA test textures;
exact native descriptor/device/usage validation remains in place. The flag does
not select targets, replay draws or automatically reconstruct anything.

The Windows test uses D3D11 to render 21 overlapping triangles: a sloped plane,
a foreground triangle and thin near/peripheral/central strips. Colour, D32Float
depth and individual MSAA samples are retained. The scene shader has a synthetic
128-sine shading workload, interpolated material colour/UV, and ordinary raster
depth. It has no discard, custom depth, UAV writes, stencil, derivatives or
screen-position-based sampling. This is an orthographic depth-test fixture,
not representative EXD geometry/materials.

The native test helper reuses the existing Metal map pattern: eight cells per
axis, centre two at 1.0 and peripheral cells at 0.25. Physical active dimensions
are queried (288² at 512² and 528² at 1024²). Allocations remain logical-sized.
Each output retains the original MSAA sample count; **no early resolve discards
samples** in this experiment. For each destination sample, a cached axis LUT
maps the logical sample position to physical coordinates. Nearest, bilinear and
conservative minimum-depth filters read matching sample planes. The latter
excludes zero-weight taps so it does not erode the full-rate centre.

Reconstructed colour/depth are imported as ordinary D3D11 textures. Standard
`Texture2DMS.Load` reads every sample; a subsequent D3D11 full-screen cyan draw
uses `LESS` against depth 0.5, followed by ordinary `ResolveSubresource`. Readback
compares that resolved result with the baseline. Colour tolerance is 3/255 per
channel; depth error threshold is 1e-5. Raw depth and coverage classifications
are measured separately from image error.

Controls require:

- Unmapped nearest, bilinear and min round trips: zero depth/colour/coverage/
  threshold/resolved-image mismatches.
- Full-rate central region: zero depth errors for every filter.
- Bilinear isolated smooth-plane region: zero depth errors above1e-5.
- Full-resolution depth replay: every depth sample **bit-identical** to the
  ordinary full-shading draw.
- Exact-depth alternative: zero depth, depth-coverage and threshold mismatches.
- Exactly 24 nonzero-map D3D render encoders per experiment, confirmed in trace.
- Disabled diagnostic switch: original MSAA import restriction still rejects it.

All these controls pass at 512²/1024² with 2×/4× MSAA and Metal validation enabled.
Passing the diagnostic means controls and expected counterexamples were found;
it does **not** mean mapped depth is compatible.

## What breaks in ordinary depth reconstruction

Representative 1024²,2× MSAA results (2,097,152 samples):

| Reconstruction | Depth samples differing >1e-5 | Depth 0.5 comparison changes | Resolved image pixels differing >3/255 |
| --- | ---: | ---: | ---: |
| Unmapped identity, each filter | 0 | 0 | 0 |
| Nearest colour/depth | 880,712 | 18,858 | 16,563 |
| Bilinear colour/depth | 84,896 | 18,029 | 24,565 |
| Minimum depth, bilinear colour | 922,045 | 36,269 | 43,443 |
| Bilinear colour + exact full-resolution depth | **0** | **0** | **15,673** |

Nearest preserves a stored depth value but quantizes slopes. Bilinear recovers
the isolated smooth plane, yet mixes surfaces at discontinuities. Minimum depth
expands occlusion and still quantizes slopes. Maximum observed depth error is
0.78 in this deliberately difficult fixture (near strips against background).
This is not just a final-image scaling problem: later ordinary depth-tested
rendering changes. None of these filters reconstructs lost per-sample geometry.

The exact-depth alternative preserves all 2,097,152 samples and their later depth
comparisons. It does not restore missing colour:64,889 sample colours exceed
3/255 tolerance; after the subsequent depth-tested overlay and resolve,
15,673/1,048,576 pixels differ (~1.49%). At 4× MSAA the corresponding resolved
count is 18,544 (~1.77%). All filters retain the correct full-rate centre.

[Baseline image](results/dxmt-depth-msaa-20261109/images/baseline.png),
[bilinear reconstructed depth](results/dxmt-depth-msaa-20261109/images/bilinear.png),
[bilinear colour with exact depth](results/dxmt-depth-msaa-20261109/images/bilinear-full-depth.png),
[exact-depth path difference mask](results/dxmt-depth-msaa-20261109/images/bilinear-full-depth-difference.png).
Orange pixels in the mask exceed3/255 in at least one RGB channel. Native-size
inspection shows thicker/missing peripheral strips and stepped silhouette edges;
this aggressive fixed-rate pattern is not established as acceptable in a headset.

Apple exposes [sample0/min/max depth resolves](https://developer.apple.com/documentation/metal/mtlmultisampledepthresolvefilter),
but those reduce samples; they are not a lossless inverse of a spatial rate map.
This experiment retains sample planes precisely to avoid hiding that distinction.

## GPU costs and the exact-depth alternative

M5, macOS27.0.1 (26A434), Metal validation on. Scene, depth replay and conventional
resolve each use 24 separate timestamp-bounded operations, discard 3 warmups and
report median 21. Timestamp encoder/barrier overhead can be material at these
small durations. Native reconstruction uses 24 independent command buffers with
the same warmup/median policy. D3D timestamp frequency/disjoint status and native
command completion/GPU duration are checked.

The exact-depth alternative adds an ordinary full-resolution D3D11 depth-only
draw of the same geometry, with PS=null, colour unbound and the same VS/raster/
depth state. It keeps that original depth for all ordinary consumers and
reconstructs **colour only** into an MSAA target. No reconstructed depth is
allocated/written in that case. This is a controlled test, not automatic replay
in DXMT. The initial exploratory version unnecessarily reconstructed unused depth;
its timings are superseded by the colour-only results.

Representative final 1024²,2× MSAA component medians:

| Operation | Ordinary ms | Foveated colour + exact depth ms |
| --- | ---: | ---: |
| Original or mapped scene including clears | 0.944917 | 0.264292 |
| Full-resolution depth-only replay | 0 | 0.103458 |
| Colour materialization | 0 | 0.129917 |
| Conventional MSAA resolve after the test overlay | 0.105834 | 0.104875 |
| Component sum | 1.050751 | 0.602542 |

**These are sums of separately measured medians, not a joined GPU frame time.**
The overlay draw/readback is a correctness
consumer and excluded from these component sums. CPU waits, resource/PSO/LUT
creation, imports, shader compilation and readback are test-only and excluded.
No total CPU overhead, MetalFX time, resident-memory or headset FPS result is claimed.

Across four independent 1024² trials:

| MSAA | Baseline component sum ms | Bilinear colour + exact-depth component sum ms | Conclusion |
| --- | ---: | ---: | --- |
| 2× | 1.048917–1.054793 | 0.602542–0.804916 | Potential net saving survives all trials |
| 4× | 1.056833–1.073833 | 0.945501–1.237877 | Variable; some trials lose performance |

Do not infer a 4× benefit from reduced physical pixels or select only its fastest
trial. Even 2× is a small fragment-heavy fixture with a trivial VS. Real geometry,
skinning, draw count and replay synchronization can erase this saving. The test
keeps full-size MSAA allocations and multiple reference outputs; it is not a
memory-residency demonstration or a physically compressed-allocation implementation.

## Boundaries for an EXD implementation

A credible route exists for **known depth-invariant, fragment-heavy passes**:
retain exact full-resolution depth/stencil where required, use a compact colour/
depth pair for shading, and materialize colour at a proven consumption boundary.
This experiment justifies a further bounded test, not enabling EXD.

The remaining gates are concrete:

1. **Replay semantics.** PS=null is valid only when the PS cannot discard, write
   custom depth/coverage or affect relevant sample/stencil behaviour. VS/GS/UAV/
   query/indirect side effects must not be repeated. Existing public pixel-shader
   reflection exposes target/coverage masks, not a complete safe-replay contract.
   A tested shader profile and conservative analysis are required.
2. **Actual EXD formats and draws.** Its trace used RG11B10Float colour and
   Depth32Float_Stencil8, many draws and 2× MSAA (4× during transitions). The new
   test covers RGBA8 + D32Float, one scene draw, no stencil. Do not generalize it.
3. **Stereo.** EXD's observed targets contain both eye regions. A single centred
   map would focus between eyes. Identify each eye and materialization ROI before
   adding gaze. Independent vertical gaze centres in one double-wide attachment
   also need compatible pass/layer handling; a separable map is insufficient by
   itself.
4. **Materialization boundary.** Investigate resolve-first colour reconstruction
   only where provenance proves that the game will not read/write MSAA colour
   beforehand. Rebuilding full-resolution MSAA colour is costly, particularly 4×.
5. **Quality/rate sweep.** Test gentler peripheral rates, thin-edge preservation,
   temporal gaze movement, actual HUD/text and paired stereo output. The 0.25
   per-axis pattern already fails thin-feature quality here.
6. **Joined scheduling.** Measure a single queued sequence including full-depth
   replay, mapped shading, materialization, resolves and existing MetalFX. Keep
   the feature opt-in and preserve ordinary fallback throughout.

The [next boundary experiment](dxmt-resolve-first-foveation.md) is now complete:
resolve-first colour with exact depth and gentler rates retains potential savings,
but the original MSAA source remains mapped. A bounded representation-state
prototype must pass an ordinary post-resolve MSAA read before automatic use.
Do not build a general replay/representation system until that experiment shows
joined GPU savings and acceptable output. No gaze subsystem was added; reuse the
existing Monado/OpenXR gaze interfaces after eye identity and pass compatibility
are established.

## Reproduce and evidence

Apply patches 0001–0008 to the pinned private base. All eight apply cleanly and
reproduce all 306 tested `src/` files byte-for-byte. Use a private, stopped Wine
runtime with the rebuilt D3D11/DXGI/NGX/Winemetal components and matching Unix
library; never overlay an active or installed runtime.

```sh
export DXMT_SOURCE_DIR="$PWD/build-in-process/dxmt-foveation-source"
export MWXR_WINE_SDK=/Users/nickkennedy/Code/wine-crossover/runtime
export MWXR_WINE_SOURCE=/Users/nickkennedy/Code/wine-crossover/sources/wine
export MWXR_IN_PROCESS_WINE="$PWD/build-in-process/exd-ngx/wine"
scripts/build-dxmt-depth-foveation-probe.zsh
scripts/test-dxmt-depth-foveation.zsh
# Individual sample count / square size:
scripts/run-dxmt-depth-foveation-probe.zsh 2 1024
```

The fixture uses explicit native imports/maps and test-only CPU/GPU waits. It is
not the unchanged-app automatic prototype. Per-sample inspection and ordinary
consumers are D3D11, not a native-only stand-in. The automated analyzer requires
all controls and 24 mapped passes; it never calls the failing reconstructed-depth
cases compatible. The diagnostic-disabled negative control preserves the old
import gate. Existing automatic colour-only, per-target and NGX ROI regressions
were rerun with the new diagnostic disabled.

[Logs, summaries, images and hashes](results/dxmt-depth-msaa-20261109/README.md).

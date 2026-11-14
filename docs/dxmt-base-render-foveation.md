# Transparent DXMT base-render foveation investigation

2026-11-09 follow-up. **A bounded automatic D3D11 prototype now works.** The
unchanged test executable makes ordinary D3D11 calls; DXMT selects one explicitly
whitelisted pass, renders with a Metal rate map and reconstructs the conventional
target before subsequent operations. A normal `Texture.Load` consumer validates
all 4,194,304 pixels within 3/255 per-channel tolerance. On M5 the synthetic 2048² workload measures **3.651500 ms
baseline versus 1.020833 ms including automatic reconstruction** (72% lower).
This establishes the mechanism, not EXD scene performance or general game support.
EXD's observed main scene has depth and MSAA and is deliberately rejected. The
[next depth/MSAA experiment](dxmt-depth-msaa-foveation.md) now demonstrates why
depth reconstruction is unsafe and measures an exact-depth alternative; the
automatic selection rules remain unchanged.

**Correction to the earlier report:** the old timing experiment divided a render
pass containing 64 repeated opaque draws by 64. Tile-based hidden-surface removal
makes that an invalid estimate of one scene draw. Its net-loss conclusion and
performance table are retired. The old logs remain historical evidence, marked
accordingly. The coordinate failure they demonstrated was real; translation now
corrects it for the restricted prototype.

## Audit and preserved work

The Wine/OpenXR repository is on `claude/steamvr-openxr-driver`, starting at
`97d6309`. Its existing uncommitted OpenXR/Metal foveation work and results were
retained. No branches were reset, commits made, or installed runtimes replaced.

The active DXMT base is the clean local `~/Code/dxmt` at
`4c3a2f5ea661bd258c14cf8ccb6924791cc75a77`, rather than upstream DXMT or the
separate dirty `~/Code/dxmt` checkout. Experimental source/builds are in
`build-in-process/dxmt-foveation-{source,build}`. The private source also contains
the preceding session's NGX tracing/feature-forwarding experiment. That work is
now preserved in patch 0003; it is not represented as a newly validated foveation
implementation. Local development remains private; no upstream contribution
was made.

| Area | Current fork implementation and reuse | Missing for transparent rendering |
| --- | --- | --- |
| Pass construction | `d3d11_context_impl.cpp` builds attachments; `dxmt_context.cpp::flushCommands` creates Metal encoders and merges compatible passes | Safe pass selection and isolation across later draws |
| Rate maps | Patches 0001/0002 retain/snapshot maps in command order; Unix operation 155; render descriptor carries the map; map mismatch prevents pass merging | Now has a bounded shader-whitelisted automatic prototype; general selection remains open |
| Per-target binding | `IDXMTNativeContext2` supports one DEFAULT, single-mip/sample/layer colour target without depth | MRT, depth, MSAA, arrays, alias propagation |
| Resources | `Texture` descriptors, `TextureAllocation`, `TextureViewRef`, view keys, renaming and on-demand PixelFormatView | Logical/physical representation state shared across all views/subresources |
| Access/copies/resolves | Central `ArgumentEncodingContext::access`, dependency tracking and resolve-merge logic | Materialization before every incompatible read/copy/UAV/CPU access, including deferred command lists |
| Lifetimes/sync | Retained allocations/views, command queue fences/events, encoder dependency graph | Retain intermediate/map/decoder resources until the last GPU consumer; track representation across renames |
| Shader translation | DXBC/AIR conversion; existing shaders remain ordinary D3D11 shaders | Inverse-map `SV_Position.xy` is now prototyped; derivative/LOD and general sampling policies remain open |
| NGX/MetalFX | `nvngx.cpp` implements NGX calls; context caches temporal scalers and consumes colour/depth/motion/exposure | Opt-in per-resource ROI copying now tested; scene-depth/MSAA integration remains open |
| OpenXR/gaze | Existing FB/META/MNDX transport and experimental Windows companion; prior fixed stereo pixel tests | An ordinary game doesn't request that companion; scene resources are upstream of XR swapchains |
| Profiling | D3D11 timestamp/disjoint queries, encoder labels and frame statistics | Captured per-scene GPU durations and total frame costs in EXD |

The old target-specific proof is documented in [dxmt-foveation.md](dxmt-foveation.md).
It renders into compact regions of full-sized allocations and requires explicit
cooperation; it is not transparent application support.

### Alyx/KosmicKrisp precedent inspected locally

Read `kk-integration` at `b386420` and the current Monado integration at
`a185d42d5`. The older private Monado binaries used by the simulator experiment
remain the matching pair built in the previous session; they were not replaced
with binaries from the newer source tree.

`kk_fdm.c` requires application-declared Vulkan subsampled attachments, checks
colour/depth/resolve compatibility, consumes application FDM/offset information
and caches maps/parameter resources. `nir_to_msl.c` has explicit rate-map decoder
operations. This is an existing rendering contract, not evidence that arbitrary
D3D11 textures can be silently reinterpreted. The application may use subsampled
samplers and engine-compatible pass layouts.

Monado's `macos-openxr-foveation.md` associates immutable maps with each released
image/layer, retains old revisions for outstanding images and projects gaze
through each eye's asymmetric FOV. Its cache revision changes only when the
actual pattern changes. The application-owned investigation already records an
older small-workload case saving about 0.45 ms in rendering but spending 1.54 ms
on reconstruction. Those are historical diagnostic results, not Alyx timings.
This investigation's new measurements below are separate.

## Comparison with the requested projects

Sources were inspected locally at these pinned revisions:

| Project | Identification and stereo | Limits and transferable lessons |
| --- | --- | --- |
| [OpenXR Toolkit](https://github.com/mbucchia/OpenXR-Toolkit/tree/6b9ecb69a4b2dc714b14a86407868af315d02531) | `vrs.cpp` filters aspect ratio, size and array count, tracks render scales and handles double-wide masks. `frameanalyzer.cpp` uses XR swapchain identity, copy-out, acquisition/release and eye prediction | ForwardRender/DeferredCopy are tracking heuristics, not proof of the engine's shading architecture. Explicit eye hints and fixed-centre fallback on invalid gaze. Sub-target/UI-sized passes are filtered, but arbitrary HUD draws in an eligible pass are not semantically identified |
| [vrperfkit](https://github.com/fholger/vrperfkit/tree/a52f8a45d330d0b66206aee85165db715e4482cd) | `d3d11_variable_rate_shading.cpp` intercepts OM binding; matches known eye dimensions with +2 pixel tolerance; rejects square targets; handles combined/array/separate eye layouts | Separate-eye order is guessed from observed counts, with manual overrides and skip entries. Accepts MSAA RTV types; hardware retains normal pixel/depth/sample layout. Fixed foveation; no general G-buffer or HUD classification |
| [PimaxMagic4All](https://github.com/mbucchia/PimaxMagic4All/tree/1fabc11cdf1fe8545e61dabef36b72db7525a156) | Emulates PVR configuration/projection/gaze using OpenVR/OpenXR; exposes controls to Pimax's injector | The actual renderer interception is proprietary `LibMagicD3D1164.dll`, supplied separately. Its pass, MSAA and UI heuristics cannot be established from this repository; it is not an open implementation to port |

VRS reduces fragment invocations while retaining conventional framebuffer
coordinates and coverage/depth storage. These projects' resource/eye heuristics
are useful inputs to diagnostics, **not sufficient permission to apply a Metal
rate map**. None proves that an arbitrary same-sized pass is safe. MSAA-compatible
VRS does not establish MSAA-compatible compact Metal resource semantics.

## What is implemented now

At the colour-only checkpoint, the seven ordered [private DXMT patches](../patches/dxmt/)
reproduced the pinned base and all306 tested `src/` files. The later optional
depth/MSAA diagnostic adds patch0008; all eight now reproduce the current
306 source files byte-for-byte. Existing uncommitted
repository work and the original DXMT checkouts remain intact.

| Patch | Function |
| --- | --- |
| 0001 / 0002 | Existing retained per-pass and per-target Metal map transport |
| 0003 | Preserved NGX feature forwarding and opt-in traces |
| 0004 | Bounded scene resource/provenance diagnostics |
| 0005 | Opt-in NGX colour/depth/motion/output ROI validation, GPU copies and cached scratch textures; virtual parameter access |
| 0006 | Opt-in DXBC→AIR `SV_Position.xy` correction using the existing map's cached inverse coordinate buffer |
| 0007 | Bounded automatic pass selection, hidden colour target and ordinary compute reconstruction |
| 0008 | Optional explicit-map depth/MSAA diagnostic; does not expand automatic selection |

The application shader/DXBC is unchanged. Patch 0006 changes its AIR translation:
physical pixel centres are mapped back into logical coordinates; Z and the existing
W conversion remain intact. A reserved fragment buffer slot carries the immutable
inverse LUT. A zero-header buffer selects the identity path on ordinary passes.
The compiler flag participates in existing shader variant keys. Native call 156
caches the inverse buffer on its immutable map; new call 157 builds a fixed-centre
map, forward LUT and reconstruction PSO. PE and Unix components must be paired.

`d3d11_context_impl.cpp::SwitchToRenderEncoder` checks the explicit profile.
`ArgumentEncodingContext::endPass` verifies the completed command stream before
redirecting attachments, then appends a normal compute encoder. Hidden/original
accesses use existing allocation retention, resource dependencies and fence
tracking; the LUT and PSO are retained by the queued compute node. No new CPU/GPU
wait is introduced. Maps, LUTs, PSO and one hidden texture are reused at unchanged
size. Full reconstruction occurs at the selected pass boundary, so later D3D11
reads/copies see conventional storage; no lazy representation state is required
for this narrow scope.

The prototype requires **all** of the following:

- `DXMT_BASE_FOVEATION=1` and `DXMT_RATE_MAP_POSITION=1`.
- Exact full-DXBC SHA1 strings in `DXMT_BASE_FOVEATION_VS` and
  `DXMT_BASE_FOVEATION_PS`, and exact `DXMT_BASE_FOVEATION_SIZE=WIDTHxHEIGHT`.
- Immediate context, one direct/indexed draw, an adjacent full clear, one
  RGBA8Unorm 2D target with one mip/layer/sample and ShaderWrite capability.
- No DSV, geometry/tessellation/stream output, visibility query, indirect draws,
  bound UAVs or shader UAV effects; full viewport and disabled scissor/blending/
  logic operations/alpha-to-coverage.
- A supported single-layer map and successful helper/resource creation.

All other cases keep ordinary rendering. Shader hashes are an **explicit tested
compatibility profile**, not proof that arbitrary shader semantics are safe. Do
not whitelist arbitrary shaders without correctness and performance tests. In
particular this is not a dimension-based global foveation switch.

The hidden texture is still allocated at logical dimensions; the map's smaller
active physical region reduces shading. This avoids introducing smaller-allocation
scissor/viewport changes into the first automatic experiment. At 2048² it adds
one 16 MiB RGBA8 target plus 24,464 bytes of forward/inverse coordinates, excluding
Metal object/PSO/driver overhead. It demonstrates no allocation-memory saving.

## Feasibility and remaining semantic limits

**Apple's documented API:** [rate maps](https://developer.apple.com/documentation/metal/mtlrasterizationratemap)
map a logical viewport to physical attachment coordinates; support, physical
sizes and conversions must be queried. Apple documents a
[separate scaling/reconstruction step](https://developer.apple.com/documentation/metal/scaling-variable-rasterization-rate-content).
MRT and layered maps exist in the API, but this does not preserve arbitrary D3D11
resource semantics. No conventional NVAPI/D3D12 coarse-shading-rate alternative
was found in the inspected Metal SDK. Tile shaders and vertex amplification do
not replace it.

**Demonstrated through DXMT:** retained maps, logical `SV_Position.xy` correction,
reduced shading, full conventional colour reconstruction, an ordinary downstream
`Load`, automatic selection/materialization, and net GPU savings for the selected
synthetic shader. Disable correction and the same position shader fails after
reconstruction. Merely setting the render-pass map is insufficient.

**Not established and excluded from automatic selection:**

- Derivatives, implicit/explicit texture LOD and gradients: coordinate correction
  alone does not restore full-resolution derivatives or sampling density.
  Selected-pass `Load`/`Sample` patterns need semantic tests; the validated `Load`
  consumer runs after materialization.
- G-buffer normals, material IDs, motion vectors and depth: consistent MRT maps
  are possible, but bilinear RGBA reconstruction is not valid for every payload.
- Depth/stencil coverage, discontinuities, comparison sampling, custom depth,
  MSAA sample positions/coverage and resolves: information is lost at reduced
  density. SSR/SSAO/reprojection need dedicated tests and policies.
- UAV atomics/side effects, partial-load passes, arrays, multiview and combined-eye
  targets. A central map on a double-wide stereo texture would be wrong.
- General multi-draw scene passes and deferred contexts. The prototype rejects
  them rather than introducing a global physical/logical resource system.

**Engine-specific inference:** EXD's large matching colour/depth targets and lack
of observed MRTs are consistent with forward rendering. Dimensions/order cannot
establish that a shader is safe, exclude HUD draws or reliably identify eyes.
A stable profile requires shader/pass fingerprints, resource provenance and GPU
timings. No EXD profile has been enabled.

## Measurements and correctness

M5, macOS 27.0.1 (26A434), Metal validation enabled for the focused probes. The
ordinary test's shader outputs logical position UV plus 128 dependent sine
iterations. It is intentionally a synthetic fragment-heavy workload. Each run
uses 24 separate single-draw, timestamp-bounded passes, discards 3 warmups and
reports the median of 21. Frequency and disjoint status are checked.

| Final automatic test | GPU ms | Mapped passes / reconstructions | Ordinary pixels wrong |
| --- | ---: | ---: | ---: |
| Baseline, correction enabled but no map | 3.651500 | 0 / 0 | 0 / 4,194,304 |
| Automatic fixed map + reconstruction | 1.020833 | 24 / 24 | 0 / 4,194,304 |
| Default flags disabled | 3.579708 | 0 / 0 | 0 / 4,194,304 |

The fair A/B pair retains the same translator flag. Earlier independent repeats
were baseline 3.651208–3.651833 and automatic 1.019458–1.022333 ms. The automatic
interval contains the ordinary clear, hidden render and reconstruction. Its
normal downstream consumer/readback is checked but excluded from timing.

A separate explicit-map probe isolates components; these are different shader/
scheduling cases and must **not** be used as a decomposition of the automatic row:

| 2048² explicit probe, final case | Scene GPU ms | Unwarp GPU ms | Sum ms |
| --- | ---: | ---: | ---: |
| Ordinary | 3.436917 | 0 | 3.436917 |
| Fixed map, active 1008² | 0.856417 | 0.075750 | 0.932167 |

Four explicit cases validate 16,777,216 ordinary `Load` pixels, zero wrong. The
512² test also passes; its warmed final case is 0.111375 + 0.007208 ms, versus
0.341875 ms ordinary. Earlier cases vary, so do not treat the small-target run
as a controlled performance study. Separate native-helper timings use three
warmups/median21; its waits are test-only.

Final fallback/default controls each check 4,194,304 pixels, require zero mapped
encoders and zero automatic reconstructions, and pass: two draws, missing
ShaderWrite capability, depth bound, 4× MSAA + ordinary resolve, missing clear,
unselected PS, position flag disabled and all defaults disabled. Existing
per-target map retention/disable/unrelated-target and unmapped identity tests
also pass. Negative position and NGX ROI controls fail as expected.

CPU submission medians in the final matrix are 0.0006 ms baseline and 0.0009 ms
automatic, but these omit worker encoding and one-off creation and can include
queue backpressure. They are **not total CPU overhead**. No full-game resident
memory, copy/resolve GPU breakdown, MetalFX GPU time, total EXD frame time or
headset FPS comparison is available. Synthetic A/C are measured; EXD A/B/C/D
and combined base-render/MetalFX foveation remain unmeasured.

## EXD and the NGX subrect fix

The earlier bounded private simulated-headset capture observed 32,898 render
encoders, 1,464 MetalFX calls, repeated 1200×672 colour+depth **2× MSAA** scene
passes, and earlier 896×504 **4× MSAA** passes. No multi-colour G-buffer was
observed in that capture. Resolved inputs were RG11B10Float colour,
Depth32Float_Stencil8 depth and RG32Float motion; output was padded
1200×1008 RGBA16Float. There is no GPU-duration-ranked scene list yet.

NGX content is 598×672 per eye, with colour/depth origins X=0 and X=600.
Declared output is 896×1007, not the padded allocation extent. The old shim
ignored both input origins and declared output extents. Patch 0005 adds
`DXMT_NGX_SUBRECTS=1`: validate independent resource ROIs, crop into cached GPU
scratch textures, run MetalFX at declared extents, copy output back while
preserving padding. It handles display-resolution motion separately and uses
virtual NGX parameter methods so third-party parameter overlays work. No CPU
readback/wait is used. Default-off preserves the previous route.

The focused NGX probe passes packed depth/stencil ROI copies, distinct left/right
colours, padded output preservation, display-resolution motion, foreign parameter
proxy and invalid/overflowing/unauthorized output origins. The disabled-fix
negative control detects the legacy bug. A subsequent isolated EXD trace records
sampled ROI diagnostics through evaluation 6,144 with the correct per-eye origins and
896×1007 motion/output source extents; this confirms calls, not temporal image
quality or performance. Classic route was used; Advanced/Cheeky game validation
remains separate.

The first EXD diagnostic hit a pre-existing linear→sRGB blit validation assertion;
the longer traces used game-side validation off and are **not correctness passes**.
Only cloned game/prefix/runtime files and a private simulated service were used.
The ordinary Steam launch and installed service were not changed.

## Reproduce

Apply patches 0001–0007 in order to the pinned private DXMT base. Build with
`enable_nvngx=true`, `enable_nvapi=true`, and overlay only a stopped **private**
runtime with the DLLs and matching `src/winemetal/unix/winemetal.so`. The missing
offline Metal compiler component was handled using verified unchanged helper
artifacts via `tools/reuse_dxmt_shader_cache.py`; changed C++/ObjC/AIR code was
rebuilt normally. Runtime Metal library compilation is used for the tiny unwarp
kernel. Single-layer map support is required; macOS 27 is the tested version,
not a claimed minimum deployment target.

```sh
export DXMT_SOURCE_DIR="$PWD/build-in-process/dxmt-foveation-source"
export MWXR_WINE_SDK=/Users/nickkennedy/Code/wine-crossover/runtime
export MWXR_WINE_SOURCE=/Users/nickkennedy/Code/wine-crossover/sources/wine
export MWXR_IN_PROCESS_WINE="$PWD/build-in-process/exd-ngx/wine"
MWXR_FOVEATION_PROBE_SIZE=2048 \
 MWXR_FOVEATION_BUILD="$PWD/build-in-process/foveation-probe-2048" \
 scripts/build-dxmt-foveation-probe.zsh
scripts/test-dxmt-base-foveation.zsh
scripts/build-dxmt-ngx-probe.zsh
scripts/run-dxmt-ngx-probe.zsh
```

The matrix extracts the shader hashes from the ordinary baseline; it does not
require any DXMT API in the executable. For explicit component timing use
`DXMT_RATE_MAP_POSITION=1` with `scripts/run-dxmt-foveation-probe.zsh
--position --benchmark --reconstruct --load-consumer`, selecting the matching
probe size/root. With the flag zero the position case is an expected failure.

For EXD tracing, `scripts/run-exd-scene-trace.zsh` requires prepared physical
private copies at `build-in-process/exd-ngx/{game,prefix,wine}`, matching private
simulator builds, macOS Steam and the Steam bridge. It rejects symlinked roots.
The private Engine.ini uses `r.WarnOfBadDrivers=0`; the private save selected
DLSS Quality. Keep the game's signed feature DLL intact. Follow the
[DXMT vendor-extension setup](https://github.com/3Shain/dxmt/wiki/Vendor-Extensions)
for signed core aliases and paired private builtin shims. `EXD_SCENE_DLSS=classic`
is the default; `advanced` selects the experimental feature-forwarding route.

```sh
DXMT_NGX_SUBRECTS=1 MTL_DEBUG_LAYER=0 scripts/run-exd-scene-trace.zsh
python3 tools/analyze_dxmt_scene_trace.py /path/to/game.log > summary.json
PYTHONPYCACHEPREFIX=/private/tmp/mwxr-python-cache python3 tests/macos/test_dxmt_scene_trace.py
```

`MTL_DEBUG_LAYER=0` is solely the diagnostic workaround for the retained EXD
assertion, never a correctness pass. The analyzer joins view aliases by
allocation and warns about pointer reuse, queue assumptions, encoder merges,
depth provenance and truncation. It does not establish stable scene/eye identity
or choose a pass automatically. Capture GPU timings/labels for candidate scene,
resolve, reconstruction and MetalFX passes before an EXD performance comparison.

## Next bounded experiment and gaze route

Keep the automatic profile disabled for EXD. The [depth/MSAA gate](dxmt-depth-msaa-foveation.md)
is now tested: nearest/bilinear/min depth reconstruction changes ordinary depth
comparisons; an exact-depth replay avoids those errors for a restricted shader.
The 2× fixture retains a potential net saving, while 4× is inconsistent, and thin
peripheral colour remains incorrect. The [resolve-first/rate-sweep follow-up](dxmt-resolve-first-foveation.md) now
measures that boundary with gentler peripheral rates. Its destination works, but
the original MSAA source still needs representation/materialization tracking
before automatic integration. Actual EXD formats,
stencil, many draws, shader side effects, stereo identity and joined GPU frame
timing remain gates.
After a compatible stereo scene passes, reuse existing Monado FB/META gaze
policy, per-eye asymmetric-FOV projection, invalid-sample fixed fallback and
immutable map caching. Associate predicted frame time and exact map revision
with scene resources through reconstruction. Add configurable centre/periphery
rates and bounded revision caches rather than a second PSVR2 gaze subsystem.
The fixed map currently uses central two cells at 1.0 and peripheral cells at
0.25 along each of eight-cell axes. Live gaze, prediction, per-eye association,
real-headset image quality and EXD GPU/FPS savings remain unimplemented/unverified.

[Current raw results and hashes](results/dxmt-foveation-followup-20261109/README.md).
[Historical audit/trace evidence](results/dxmt-base-foveation-20261109/README.md).

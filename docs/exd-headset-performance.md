# EXD on PS VR2: settings comparison, 9 November 2026

EXD's loaded scene runs at **about 14 fps** at the PS VR2's default render size.
MSAA off, DLSS→MetalFX and low quality settings each change that by under 10%.
Only render resolution makes a large difference. At 40% viewport scale with MSAA
off it reaches about 75 fps; 90 Hz has not been reached in any tested setting.

## Setup

- Apple M5, macOS 27.0.1, PS VR2 at 90 Hz on the installed Monado service
  (`v25.1.0-2491-ge468db58f`, matching x86_64 client). Service compositor.
- Codex's private EXD copy under `build-in-process/exd-ngx`, with its DXMT
  patch series (including the NGX per-eye fix, `DXMT_NGX_SUBRECTS=1` for DLSS).
- At 100% viewport scale Monado recommends 2800×2856 per eye; EXD renders one
  5600×2856 side-by-side target. That is about 9× the pixels of the simulated
  headset (1792×1008) used in the [TBDR audit](dxmt-tbdr-audit.md).
- Each run is 90 s from launch. The headset sat still, so the view was fixed.
  The scene changes by itself after about 10–15 s; "loaded" below is the main
  level (≥55 render passes per frame), skipping its first 60 frames.
- Time is the DXMT audit's per-frame union of command-buffer GPU intervals
  (`DXMT_TBDR_AUDIT=1`, no detail). Frame period is from successive frames.

## Results (loaded scene, medians)

| Viewport scale | Settings | GPU ms | Frame period ms | ≈ fps |
|---:|---|---:|---:|---:|
| 100% | 2× MSAA, upscaler off (current) | 71.0 | 71.3 | 14.0 |
| 100% | 4× MSAA | 82.3 | 82.5 | 12.1 |
| 100% | MSAA off | 66.3 | 66.5 | 15.0 |
| 100% | DLSS Quality → MetalFX, 2× MSAA | 65.9 | 66.1 | 15.1 |
| 100% | 2× MSAA, low quality, volumetric fog off | 69.0 | 69.2 | 14.4 |
| 50% | 2× MSAA | 20.1 | 20.3 | 49 |
| 50% | DLSS Quality → MetalFX, 2× MSAA | 18.0 | 18.3 | 55 |
| 50% | MSAA off | 17.2 | 17.4 | 57 |
| 40% | MSAA off | 12.9 | 13.3 | 75 |

Follow-up sweep (same method):

| Scale | 2× MSAA | MSAA off | DLSS + 2× MSAA | DLSS + MSAA off |
|---:|---:|---:|---:|---:|
| 60% | 29.1 ms | 23.5 ms | 26.8 ms | — |
| 50% | 20.1 ms | 17.2 ms | 18.0 ms | 16.9 ms |
| 40% | 14.5 ms | 12.9 ms | 12.9 ms | 11.9 ms |

`r.SeparateTranslucency=0` (user `Engine.ini`) made the loaded scene slower:
76.8 ms against a repeated 100%/2× MSAA baseline of 72.5 ms.

The 90 Hz budget is 11.1 ms. Before the level loads, the simpler opening scene
holds ~85 fps at 100% with 2× MSAA. With DLSS it drops to ~36 fps (27.6 ms GPU).

## Findings

**Cost is per-pixel shading.** A quarter of the pixels (50%) cuts loaded GPU
time from 71 to 20 ms. MSAA off removes ~40% of nominal attachment traffic but
only 7% of the time, so bandwidth is not the limit.

**Where the time goes.** A 5-second Metal System Trace of the loaded scene
(100%, 2× MSAA, about 72 frames) gives roughly, per frame:

| Pass | ms per frame |
|---|---:|
| Main opaque scene: 2× MSAA `RG11B10Float`, ~32 draws | ~39 |
| 3-draw pass into 2× MSAA `RGBA16Float` with depth (likely separate translucency) | ~13.5 |
| Compute (6 dispatches) | ~4.3 |
| 48-draw 2× MSAA BGRA8 pass with resolve | ~2.6 |
| Depth max-resolve fullscreen pass | ~2.3 |

These are fragment-channel interval sums. Channels overlap, so the rows are
not exactly additive.

**DLSS→MetalFX.** The per-eye fix works on hardware: the audit shows two
MetalFX temporal scaling passes per frame. But temporal MetalFX at a
2800×2856-per-eye output costs roughly 20 ms per frame, which cancels the
saving from the lower internal resolution. At 50% scale it is slightly worse
than MSAA off.

**Quality settings.** Low shadows/effects/post/foliage/shading/view distance
plus volumetric fog off removed five passes but saved only 2 ms.

**EXD's `MSAASampleCount` save value is not a sample count.** `0` is off,
`2` is 2×, and `1` gives **4×** (verified from attachment sample counts).

## What this means

- To play now: lower viewport scale and turn MSAA off. 40% gives ~75 fps;
  something near 35% is needed for 90 Hz (estimated, untested), which will look
  soft. Whether Monado's reprojection makes 50% (~57 fps) acceptable needs
  someone wearing the headset.
- The main opaque pass is fragment-bound at full resolution, so foveated
  shading targets the right cost. The transparent DXMT foveation prototype is
  not close to handling EXD, though (see the
  [MSAA source](dxmt-msaa-source-foveation.md) and
  [draw replay](dxmt-draw-replay-ownership.md) notes).
- The 13.5 ms 3-draw pass is worth identifying. If it is separate
  translucency, `-ExecCmds="r.SeparateTranslucency 0"` is a cheap test.
- Not measured: visual quality, reprojection behaviour, CPU time.

## Quad views (fixed inset)

EXD's Unreal 4.27 build supports `XR_VARJO_quad_views`: two wide context views
plus two higher-density inset views, which the runtime blends together.

- The bridge now advertises and forwards `XR_VARJO_quad_views` when Monado has
  it (`MWXR_QUAD_VIEWS=0` hides it) and accepts 4-view projection layers. EXD
  enables the extension as soon as it is offered.
- Monado emulates quad views for a stereo headset only with its compute
  compositor. The installed LaunchAgent sets `XRT_COMPOSITOR_COMPUTE=0`, so these
  runs used a private on-demand job with the same binary and environment plus
  `XRT_COMPOSITOR_COMPUTE=1` (`MWXR_EXD_SERVICE_ENV`). The LaunchAgent was not changed.
- Unreal 4.27 takes the first configuration the runtime lists that it supports,
  and Monado lists stereo first, so the bridge moves quad views to the front
  (`MWXR_QUAD_VIEWS=stereo-first` keeps Monado's order).
- Monado's emulated inset is centred, has half the context field of view, and
  is given the same recommended size as the context view. It does not follow
  gaze. `MWXR_QUAD_CONTEXT_SCALE` / `MWXR_QUAD_INSET_SCALE` scale the recommended
  sizes. Relative to the 100% stereo density, an inset factor of 0.5 is full density.

Loaded scene, MSAA off, compute compositor:

| Views (per eye) | Density: context / inset centre | GPU ms | ≈ fps |
|---|---|---:|---:|
| Stereo 100% (reference, service compositor) | 100% | 66.3 | 15 |
| Quad 1400×1428 + 1400×1428 (Monado default at 50%) | 50% / 100% | 46.3 | 21.5 |
| Quad 1120×1142 + 1400×1428 | 40% / 100% | 34.7 | 28.6 |
| Quad 980×1000 + 1120×1142 | 35% / 80% | 32.7 | 30.4 |
| Quad, all views 10% | — | 8.1 | — |
| Stereo 10% | — | 5.0 | — |

The two extra views cost about 3 ms of fixed work. Most of the remaining cost
is pixels: the context views are still shaded underneath the insets, inset views
get no hidden-area mask, and the inset covers the busiest part of the screen.
A fixed centred inset at full density cuts the 100% cost roughly in half, but at
~29 fps it is still slower than flat 50–60% stereo. Image quality, inset
blending and seams have not been checked by eye.

**Compositor cost.** Monado's own GPU time per composite (its
`compositor_gpu` timing CSV, end minus start, 90 composites per second):

| Compositor | Layer | Median ms | p90 ms |
|---|---|---:|---:|
| Graphics (installed default) | stereo projection | 0.6–1.1 | 1.1–1.7 |
| Compute | stereo projection | 1.5–1.8 | 2.0–2.3 |
| Compute | quad views | 5.4–5.9 | 6.1–7.1 |

A single projection layer takes the compute compositor's one-pass fast path
(distortion and timewarp together). A quad-view layer does not: it goes through
the general layer squasher, a full-resolution pass per eye that samples both
the context and inset views, then a separate distortion pass. At 90 Hz,
5.5 ms per composite is about half the GPU, shared with the game, so part of the
quad-view game time above is this contention. The contention costs the game more
than the compositor's own time: at 50% stereo, the compute compositor adds about
0.7 ms per composite but the game's loaded frame goes from 17.2 to 19.2 ms.

**Quad-view fast path (built, not yet on the headset).** Opt-in
`XRT_COMPOSITOR_QUAD_VIEW_FAST_PATH=1` in the service (compute compositor only)
sends a context-plus-inset frame through the one-pass distortion shader, which
blends the inset itself with the squasher's 5% edge fade. It applies when the
frame is exactly those two plain projection layers, each inset view has its
context view's pose (true for Monado's emulated inset), and there is no colour
scale, chroma key or foveation map. Because the poses match, the inset UV is a
scale and offset of the context UV, so no second timewarp is needed.

| Check | Result |
|---|---|
| GPU test (new): inset placed from angles alone, both timewarp paths | marker exact, gradients within 3 code values, outside pixels identical |
| Offscreen benchmark, PS VR2 sizes (2800×2856 intermediate, 2000×2040 output) | squasher route 4.50 ms → fast path 1.20 ms; plain stereo distortion 0.98 ms |
| EXD on the simulated headset (desktop window, 3078×1730) | fast path used on every quad frame; composite 4.23 → 2.90 ms |

Expected on the PS VR2: about 5.5 ms → about 2 ms per composite, plus whatever
the game gains from less contention (estimate). Not yet checked by eye; this
session could not capture the screen. Existing render tests show the same three
failures with and without the change.

**Graphics compositor.** The same option now also works on the graphics
compositor, which the LaunchAgent uses. The distortion mesh's source binding is
an array of two (source, then inset); the vertex shader passes inset UVs, which
interpolate like the context UVs because they are affine in them, and the
fragment shader blends with the same edge fade. With the option on, Monado offers
emulated quad views on the graphics compositor too. A new GPU test checks it the
same way as the compute one (both timewarp paths: marker where the angles say,
gradients within 1.5 source texels, outside pixels identical), and the existing
graphics mesh foveation test still passes.

EXD on the simulated headset, graphics compositor, fast path on: quad views
selected, fast path on every quad frame, `use_compute` 0.

| Compositor, simulated headset | Composite (idle game) | Composite (GPU-bound) | Game frame (GPU-bound) |
|---|---:|---:|---:|
| Compute, layer squasher | 4.23 ms | 1.40 ms | 21.6 ms (46.3 fps) |
| Compute, fast path | 2.90 ms | 1.06 ms | 21.4 ms (46.8 fps) |
| Graphics, fast path | 0.77 ms | 0.55 ms | 20.4 ms (49.0 fps) |
| Graphics, stereo (reference) | 0.35 ms | — | — |

"GPU-bound" is 4× MSAA with all four views at 1280×1440 (Monado's maximum),
90 s each. The idle runs are 60 fps-capped, probably with lower GPU clocks,
which likely explains their larger composite times. The game recovers at least
the GPU time the compositor saves (1.2 ms per frame for about 1 ms of compositor
time). The simulator composites at about 52 Hz into a small window; on the
PS VR2, at 90 Hz with 5.5 ms quad composites, the saving should be much larger.

The graphics route also avoids the compute compositor's problems on this setup:
WSI shimmer, no direct-to-display, and the extra fragment output pass.

**Visibility mask.** The bridge offers `XR_KHR_visibility_mask` when Monado
has it (`MWXR_VISIBILITY_MASK=0` hides it) and translates its event's session.
EXD fetches the hidden and visible meshes for both eyes as soon as it is
offered, and Unreal draws the hidden mesh: masking more of the view cuts GPU
time (below).

Before this, the PS VR2 driver had no mask of its own, so Monado returned its
default four corner triangles, about 3% of each view. The `claude/quad-gaze-inset`
branch now gives the PS VR2 a mask computed at startup from the headset's own
distortion calibration. The lens distortion is radial, so for each direction
from the lens centre the driver finds the furthest panel point it keeps and
the image point that panel point reads (furthest of the three colour channels).
Everything beyond that outline is hidden. Service options:

- `PSVR2_VISIBILITY_MASK_R2` (default 1.0): squared lens radius kept. The
  driver's distortion table ends at 1, so the default hides only the image
  read by panel corners beyond it. 0 keeps the whole panel.
- `PSVR2_VISIBILITY_MASK_MARGIN` (default 0.03): extra room as a fraction of
  the radius, so reprojection near the edge still finds rendered pixels.
- `PSVR2_VISIBILITY_MASK=0`: back to Monado's corners. The driver also falls
  back to them if the calibration cannot give an outline (the driver's
  fallback calibration for old headsets has no lens scale).

Share of each eye's render hidden, from typical calibration values (the
offline test prints these; the real headset's calibration produced a lens mask
too, see below):

| Lens radius kept (r²) | No margin | 3% margin |
|---|---:|---:|
| Whole panel | 0.1% | 0.1% |
| 1.0 (default) | 6.4% | 4.6% |
| 0.9 | 28.4% | 25.8% |
| 0.8 | 46.7% | 44.0% |
| 0.7 | 59.3% | 57.1% |

The outer ring of the lens (r² 0.9–1) reads over a fifth of the render
because the distortion squeezes the periphery. A mask there costs little
visible detail if the ring is hard to see, but where the lens edge sits needs
checking by eye: try r² 0.9 and 0.8 and look for a black rim.

Measured on the PS VR2 on 10 November, headset connected but not worn (so
it rested looking at one view), 2× MSAA, 100% scale, graphics compositor, one
90 s run each. Frames 1200–1800 were the same heavy scene in every run (the
corners run later moved to a lighter view, so whole-run medians do not
compare):

| Mask | Hidden | GPU per frame |
|---|---:|---:|
| Monado corners | ~3% | 57.4 ms |
| Lens, r² 1.0 (default) | ~5% | 58.7 ms |
| Lens, r² 0.8 | ~44% | 43.9 ms |

The r² 0.8 mask saved 24% of GPU time. The default's difference is within run
noise. Offline test: `tests/tests_visibility_mask` (mask areas and winding,
and a brute-force check that the outline holds every image point the panel
reads). In the simulator, `XRT_VISIBILITY_MASK_ELLIPSE=S` gives drivers that
use Monado's generic mask an ellipse instead (S = 1 touches the view's edges).

The compute compositor has other costs on this setup. Every compute run logs
`framebufferOnly=0`, and earlier display traces showed such drawables are
composited by WindowServer rather than sent direct to the display. On 16 October
the user isolated `XRT_COMPOSITOR_COMPUTE=1` as the cause of moving-head
shimmer with the WSI presenter. The later private-image and fragment-output
fix was never accepted visually, which is why the LaunchAgent pins the graphics
compositor (see `monado-2/doc/macos-wsi-vulkan-passthrough.md`).

**Gaze-following inset (built, not yet run on the headset).** A Monado change
on branch `claude/quad-gaze-inset` (worktree
`~/Code/alyx-native/workspace/monado-quad-gaze`, from the installed commit
`e468db58f`) adds three client options for emulated quad views:

- `OXR_QUAD_INSET_GAZE=1` points the inset at the runtime-private eye gaze
  (latest sample, held 250 ms through blinks, then recentred). The inset is
  shifted, not shrunk, to stay inside the context view.
- `OXR_QUAD_INSET_DEGREES=N` makes the inset N° wide and tall (default: half
  the context FOV).
- `OXR_QUAD_INSET_LOG=1` logs the inset FOV about once a second.

The compositor already draws the inset at the FOV the game submits and blends
its edges, so only the client library changes. The service and x86_64 client
are built with the installed build's settings and version string
(`build/monado-quad-gaze-{arm64,x86_64}` in `alyx-native`). One run with the
headset disconnected fell back to the simulated HMD; there the option warned
about the missing eye tracker and used a centred 30° inset, as intended.

The gaze inset moves only when the game calls `xrLocateViews`, so at ~30 fps
it lags a saccade by roughly 70–100 ms. The inset needs a margin around the
fovea for that.

## Reproduce

```sh
scripts/run-exd-headset-compare.zsh msaa2 90       # also msaa0, msaa4, dlss, dlss-msaa0, dlss-nofix
MWXR_VIEWPORT_SCALE_PERCENTAGE=50 scripts/run-exd-headset-compare.zsh msaa0 90
MWXR_EXD_LOW=1 scripts/run-exd-headset-compare.zsh msaa2 90
MWXR_EXD_DETAIL_FRAME=1300 scripts/run-exd-headset-compare.zsh msaa2 60   # per-pass attachments
# Quad views: bridge build with quad-view support, private compute-compositor service.
MWXR_EXD_BUILD=quad-gate MWXR_EXD_SERVICE_ENV="XRT_COMPOSITOR_COMPUTE=1" \
  MWXR_QUAD_CONTEXT_SCALE=0.4 MWXR_QUAD_INSET_SCALE=0.5 scripts/run-exd-headset-compare.zsh msaa0 90
# Gaze-following inset: private job runs the modified service, game loads the matching client.
B=~/Code/alyx-native/build
MWXR_EXD_BUILD=quad-gate MWXR_EXD_SERVICE_ENV="XRT_COMPOSITOR_COMPUTE=1" \
  MWXR_EXD_SERVICE_BIN=$B/monado-quad-gaze-arm64/src/xrt/targets/service/monado-service \
  MWXR_EXD_RUNTIME_JSON=$B/monado-quad-gaze-x86_64/openxr_monado-dev.json \
  OXR_QUAD_INSET_GAZE=1 OXR_QUAD_INSET_DEGREES=30 OXR_QUAD_INSET_LOG=1 \
  MWXR_QUAD_CONTEXT_SCALE=0.4 MWXR_QUAD_INSET_SCALE=0.25 scripts/run-exd-headset-compare.zsh msaa0 90
# Add XRT_COMPOSITOR_QUAD_VIEW_FAST_PATH=1 to MWXR_EXD_SERVICE_ENV for the fast path; with it,
# XRT_COMPOSITOR_COMPUTE=0 (the graphics compositor) also gets quad views. Without a
# headset, also add SIMULATED_DESKTOP=1 XRT_MACOS_REFRESH_RATE_HZ=0 (simulated HMD in a window).
# Offscreen tests (in alyx-native/build/monado-quad-gaze-arm64, KosmicKrisp env from the LaunchAgent):
#   tests/tests_render_compute 'Quad-view inset blends over its context in the distortion pass'
#   tests/tests_render_compute '[quad_benchmark]'
#   tests/tests_visibility_mask
# PS VR2 lens mask: add PSVR2_VISIBILITY_MASK_R2=0.8 (or 0.9) to MWXR_EXD_SERVICE_ENV with the
# modified service, e.g.
MWXR_EXD_BUILD=quad-gate MWXR_QUAD_VIEWS=0 MWXR_EXD_SERVICE_ENV="PSVR2_LOG=info PSVR2_VISIBILITY_MASK_R2=0.8" \
  MWXR_EXD_SERVICE_BIN=$B/monado-quad-gaze-arm64/src/xrt/targets/service/monado-service \
  MWXR_EXD_RUNTIME_JSON=$B/monado-quad-gaze-x86_64/openxr_monado-dev.json scripts/run-exd-headset-compare.zsh msaa2 90
python3 tools/analyze_dxmt_tbdr_audit.py RUN/game/game.log --min-frame 0 --max-frame 100000 --frames-csv frames.csv
```

The script edits copies of the private save with `tools/exd_settings.py`,
restores the save and `GameUserSettings.ini` afterwards, and copies Monado's
timing CSVs into the run directory. Runs are in
`build-in-process/exd-ngx/headset-20261109-*`.

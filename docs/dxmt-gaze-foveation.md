# Gaze-directed stay-warped foveation

2026-11-12. Follow-up to [per-resource read specialisation](dxmt-resource-read.md)
and the [fixed PS VR2 profile](dxmt-fixed-psvr2.md).

**The OpenXR bridge can now build the rate map each frame and centre it on
Monado's gaze.** A synthetic moving gaze runs EXD cleanly on the simulated
headset: 505 distinct maps in 100 s, no layout warnings, no frame-time
hitches. **Real gaze has not been run**: the simulator has no eye tracker and
the gaze-capable Monado service did not start under the launcher's simulated
environment.

## Design

The map is built in the bridge, not taken from Monado's Metal map. Monado's
packed map has 16 samples across both eyes and a profile in texture
fractions; the profile that looked good on the headset is angular, 64×32 and
centred on the lens axis. So Monado supplies only the gaze centre.

- `MWXR_GAZE_FOVEATION="outer[,core,edge[,step]]"` (defaults 0.3, 12°, 27°, 2°).
  Full rate within `core` degrees of the centre, smoothstep to `outer` at
  `edge` degrees, per axis, through each eye's own FOV and image rectangle as
  the game last submitted them. Nothing is specific to the PS VR2.
- The bridge enables `XR_FB_foveation`, `XR_FB_swapchain_update_state`,
  `XR_FB_foveation_configuration` and `XR_META_foveation_eye_tracked` natively
  (the game asks for none), applies an eye-tracked profile to the projection
  swapchain at each acquire, and reads the centre with
  `xrGetFoveationEyeTrackedStateMETA`. It never queries Monado's Metal map, so
  the compositor keeps treating submitted images as ordinary.
- The centre is quantised to `step` degrees with hysteresis. Maps are cached
  (256 entries). Invalid gaze keeps the last centre for 30 frames, then falls
  back to straight ahead with the fixed profile's 20°/35° core.
- Without runtime support, or with `MWXR_GAZE_FOVEATION_FIXED=1`, the centre
  is always straight ahead: the fixed profile, derived from the submitted FOV
  instead of hard-coded angles. `MWXR_GAZE_FOVEATION_TEST=1` sweeps a
  synthetic gaze.
- The map reaches DXMT through the existing
  `IDXMTNativeContext1::SetRasterizationRateMap`. `DXMT_STAY_WARPED=external`
  (any value that is not `WxH`) keeps the stay-warped rules without DXMT
  building its own map. **No DXMT change was needed**; patches 0001–0021 are
  as they were.

## Checks (simulated headset, 1792×1008, 2× MSAA, outer 0.3)

| Case | Result |
|---|---|
| Synthetic gaze, 100 s | 505 maps built, 3–12 mapped passes per command buffer, 0 "second layout" warnings |
| Frame period, frames 1500–5000 | median 16.8 ms (60 Hz cap), 99th percentile 17.8 ms, worst 34.8 ms |
| Fixed fallback image vs baseline | centre of each eye 0.33–0.34 mean difference (two baselines differ by 0.48) |
| Synthetic-gaze image | coherent; sharp region follows the synthetic centre |

## Not verified

- Runtime gaze on the PS VR2: profile creation, per-frame update, validity.
- That Monado leaves images unfoveated when a profile is applied but no map is
  queried. Its header says so; it was not observed.
- A pass reading two different layouts is still left untranslated by DXMT.
  None occurred in the title scene with the map changing every few frames.
- Latency: gaze is sampled at swapchain acquire.

## Run on the PS VR2

Gaze needs a Monado service and x86_64 client with the foveation extensions.
The installed pair (v25.1.0-2491) was built with them off. A private pair is
built from the same source line (`~/Code/alyx-native/workspace/monado-claude-upstream-clean`,
v25.1.0-2500) with the five foveation gates on, so the run keeps the installed
settings: WSI presenter, graphics compositor, KosmicKrisp.

```sh
MWXR_EXD_EXPERIMENT_ROOT="$PWD/build-in-process/exd-warp" MWXR_EXD_TAG=psvr2-gaze \
MWXR_EXD_BUILD=gaze-gate MWXR_EXD_STAY_WARPED=external DXMT_RATE_MAP_RESOURCE_READ=1 \
MWXR_GAZE_FOVEATION=0.3 MWXR_EXD_SERVICE_ENV="PSVR2_GAZE_STREAMS=1" \
MWXR_EXD_SERVICE_BIN="$PWD/build-in-process/gaze-wsi-arm64/src/xrt/targets/service/monado-service" \
MWXR_EXD_RUNTIME_JSON="$PWD/build-in-process/gaze-wsi-x64/openxr_monado-dev.json" \
  scripts/run-exd-headset-compare.zsh msaa2 600
```

That service, started alone with the LaunchAgent's environment, presents to
the PS VR2 through WSI (2026-11-12). The game has not been run against it.

The first headset attempt used the `monado-2` checkout's service instead and
showed nothing: its `wsi` presenter requires `VK_GOOGLE_display_timing`, which
it does not get from KosmicKrisp, and its native presenter needs MoltenVK and
the compute compositor. Do not use it for comparisons with the fixed runs.

Dropping the last three variables uses the installed service and gives the
fixed straight-ahead profile through the same path.

Build: `scripts/build-in-process-gate.zsh` into `build-in-process/gaze-gate`;
Monado from `build-in-process/gaze-wsi-options-{arm64,x86_64}.cmake` (the
installed-line caches plus the gates) into `gaze-wsi-arm64` and `gaze-wsi-x64`.

## First PS VR2 gaze run (2026-11-12)

`build-in-process/exd-warp/headset-20261112-075650-msaa2-psvr2-gaze`: private
installed-line service (WSI, graphics compositor, KosmicKrisp), outer 0.3,
core 12°, edge 27°, 5600×2856, 2× MSAA.

- Runtime gaze was used and valid; 845 maps in about 12,700 acquires. Validity
  dropped 57 times, each for a few frames while worn; the 30-frame hold covered
  them. No "second layout" warnings, no new DXMT warnings.
- Wearer: no seams, frame rate felt good. Some shadows changed with eye
  movement, and also with head movement while looking at them, so the cause is
  not established as foveation.
- GPU time per frame, median per 1000 frames (different play each run, so
  not a controlled comparison):

| Run | Range of medians, ms | Frames per second |
|---|---|---|
| Baseline | 55.7–66.0 | 15–18 |
| Uniform map, 0.5 | 29.7–40.4 | 24–27 |
| Fixed PS VR2 profile, 0.3 | 29.2–31.7 | 31–35 |
| Gaze, 0.3, 12°/27° | 21.2–34.6 | 29–46 |

Gaze had no multi-second stalls after loading and a 99th percentile frame
period within 7 ms of the median in every window. Whether it is faster than
the fixed profile on the same scene has not been measured.

## Other games

2026-11-12. `MWXR_FOVEATION` makes this a per-launch option
([foveation.md](foveation.md)). DXMT patch 0022 adds what OpenVR games need:
xrizer copies each eye texture into an array swapchain, and a stay-warped
source is now drawn through its map into the swapchain image instead of being
copied texel for texel. Other whole-image copies pass the layout on.
wineopenxr accepts a swapchain with one array layer per eye.

Simulated headset only (896×1007 per eye), runs under
`build-in-process/game-checks`:

| Game | Result |
|---|---|
| EXD (regression, new DXMT, synthetic gaze) | 398 maps in 100 s, no new warnings, image coherent. |
| Half-Life: Alyx, `ALYX_FIXED_PERCENT=100`, fixed | Renders both eyes into one 1792×1007 texture. 7 mapped passes per frame, no rate-map warnings, image coherent and un-warped. |
| Half-Life: Alyx, adaptive fidelity (the default), fixed | 1290×725 texture with an eye rectangle per fidelity level (448×503 to 645×725). 7 mapped passes per frame, no warnings; the submitted rectangle matches the baseline's. |
| Audio Trip (Unity), fixed | Inconclusive. It renders one 985×1107, 4× MSAA pass per eye (1.1× the recommended size) but created no swapchain and submitted no frame in 90 s on the simulator, so no map was ever set. Whether it foveates once it submits is not known. |
| Assetto Corsa, race started directly (`acs.exe` with a `race.ini`), fixed and synthetic gaze | One 896×1007 texture per eye. 12–16 mapped passes per command buffer, no rate-map warnings, 398 maps in 90 s under synthetic gaze. Images match the baseline in content; against it the centre differs by 4.8 and the left edge by 8.6 (clouds move between runs). |
| Hyperbolica (Unity 2021, OpenXR plugin, `startXR`), fixed | Runs, not foveated (`mapped_render=0`). One 896×1008 two-layer, 4× MSAA pass (82 draws, single-pass instanced), resolved per layer, then one two-instance draw into the two-layer swapchain. Image identical to the baseline (mean difference 0.07). Only the VR warning screen was reached; it needs a controller. |
| RAGER (Unity URP, OpenXR plugin), fixed | Runs, not foveated (`mapped_render=0`). Same shape with 8× MSAA: three two-layer MSAA passes and three two-layer single-sample passes per frame, the last into the swapchain. Only the disclaimer screen was reached. |

Those two rows are DXMT patches 0001–0022. Patch 0023 (in the default
foveation runtimes since 2026-11-12, after these checks) maps passes with up to two layers, accepts 8× MSAA and translates `Sample`,
`SampleLevel` and `Load` on 2D array textures:

| Game, patch 0023 | Result |
|---|---|
| Hyperbolica, fixed | 1 mapped pass per frame (the 82-draw scene pass). Left and right eye each match their baseline: mean difference 0.08, centre 0.001, corner 0.39. Metal applies the one-layer map to both layers. |
| RAGER, fixed | 5 mapped passes per frame, mean difference 0.06 from the baseline. |
| RAGER, synthetic gaze | 398 maps in 95 s, 5 mapped passes, no warnings, mean difference 0.64. |
| Assetto Corsa, Half-Life: Alyx (fixed), EXD (synthetic gaze) | As before; no new warnings. Assetto Corsa and Alyx images match their earlier foveated runs (1.6 and 4.5, animated scenes). EXD's image was not captured. |

Both Unity games were only seen at their opening screens (a warning panel and a
disclaimer), which need a controller press. Effects that read the scene or depth
texture were not exercised.

Integer loads (`Load`) from array textures are translated too (RAGER resolves
its 8× MSAA target with them). An earlier build with that change showed a broken
Assetto Corsa image; Assetto Corsa has no array loads, the same code is correct
there with the cache off and on a fresh flag bit (cold and warm), and the broken
runs all used shader variants cached under one flag bit during the first run.
What made that first run's variants wrong was not found. Bisect translation
changes with `DXMT_SHADER_CACHE=0`.

The simulated service fails with `WSI display 1 is inactive` when the Mac's
display is asleep; the runs keep it awake with `caffeinate -d -u`.

Open points:

- Alyx: with the foveation settings on, even at rate 1, the area outside the
  hidden-area mask comes out black; the baseline shows scene there. Not
  explained. It is outside the visible area if the mask fits the headset.
- The first version of this check passed while foveating only six intermediate
  passes: the map was sized to the swapchain (896 wide) and Alyx's eye texture is
  1792 wide, so the copy was never exercised. Sizing the map from the copy source
  exposed that the copy shader was missing from DXMT (this build copies a
  prebuilt internal shader library; there is no Metal toolchain installed), and
  warped images reached the swapchain. winemetal now compiles that shader from
  source when the library lacks it.
- GPU time on the simulator is not a guide: adaptive Alyx was 12.7 ms with
  foveation and 13.9 ms without (median), fixed mode 14.4 ms with.
- Nothing here has been worn. Real gaze with an array swapchain is untested.

Assetto Corsa was run in a private copy of its prefix
(`build-in-process/ac-check`) with its own documents folder, the install's
default `cfg` files, `MODE=OPENVR` and a named car skin (an unknown skin gives a
random car colour per run). The real settings in `~/Documents/Assetto Corsa`
are not readable from the session that ran these checks, so the user's own
video settings (anti-aliasing, post-processing) were not what was tested.

## Optimisation notes (2026-11-12, simulator)

- **Skipped passes are a small share in EXD.** Per frame at 2560×1440: 19 of 21
  full-size passes are mapped (the other two are the swapchain); the unmapped
  rest is three half-size and four quarter-size passes, the bloom chain and
  shadow maps, under 5% of the full-size passes' pixels. Mapping them is not
  worth the work for EXD.
- **Restricting a mapped pass to the map's physical size does not work.** Setting
  the pass's render-target width and height to the physical size gave GPU
  command-buffer errors from frame ~780; Metal takes those in logical terms.
- **The stock simulator cannot rank optimisations.** Its display is hard-coded
  at 1280×720 and the maximum image at twice that, so EXD's GPU time stayed at
  13–15 ms whatever was changed: frame time there is set by pacing.
- Handing the warped image to the compositor would save about one full-size
  pass of roughly twenty per frame in EXD, so it is a small gain, not a large
  one.

## High-resolution simulator (2026-11-12)

A private service built from a copy of the installed Monado line
(`build-in-process/monado-sim-src`, change in
`build-in-process/sim-hires-monado.patch`, build `sim-hires-arm64`) reads
`SIMULATED_DISPLAY_WIDTH` and `SIMULATED_DISPLAY_HEIGHT`. With 4000×2040 and the
usual 140% scale EXD gets the PS VR2's 5600×2856 swapchain, and the installed
x86_64 client works against it. The installed service is unchanged.

```sh
MWXR_EXD_SERVICE_BIN="$PWD/build-in-process/sim-hires-arm64/src/xrt/targets/service/monado-service" \
MWXR_EXD_RUNTIME_JSON="$HOME/Code/alyx-native/build/monado-system-x86_64/openxr_monado-dev.json" \
MWXR_EXD_SERVICE_ENV="SIMULATED_DESKTOP=1 XRT_MACOS_REFRESH_RATE_HZ=0 SIMULATED_DISPLAY_WIDTH=4000 SIMULATED_DISPLAY_HEIGHT=2040" \
  scripts/run-exd-headset-compare.zsh msaa2 120
```

EXD title scene, 2× MSAA, static pose, GPU busy time per frame (union of DXMT's
command-buffer intervals, second half of each run). The simulator's field of
view is 85° per eye with no lens, so a profile covers a different share of the
image than on the PS VR2.

| Run | Median, ms | 10th–90th percentile |
|---|---|---|
| Baseline (three runs) | 59.6, 59.9, 62.6 | 58.1–63.3 |
| Fixed fallback: 0.3, 20°/35° core (three runs) | 37.0, 38.3, 39.7 | 36.4–41.3 |
| Held gaze, `0.3,12,27` (the worn profile) | 28.8 | 27.0–29.5 |
| Held gaze, `0.25,10,22` | 24.2 | 22.5–25.2 |
| Held gaze, `0.1,4,8` (extreme) | 13.3 | 11.9–14.6 |

"Held gaze" is `MWXR_GAZE_FOVEATION_TEST=2`: a synthetic gaze held straight
ahead, so the given core and edge apply. `MWXR_GAZE_FOVEATION_FIXED=1` always
uses the 20°/35° core whatever the profile says; an earlier version of this
section mistook two such runs (`0.25,10,22` at 37.2 ms and `0.1,4,8` at 33.9 ms)
for narrower profiles and reported a 34 ms floor that does not exist.

The baseline is in the range measured on the headset (55.7–66.0 ms).

### Where the time goes

`DXMT_PASS_TIMING=1` (patch 0024, diagnostic) samples Metal's stage-boundary
timestamps for every render pass. GPU ms per frame, vertex plus fragment
(`build-in-process/exd-fov/passes.py` tabulates a run):

| Pass (all 5600×2856 unless noted) | Baseline | Fixed 20°/35° | 12°/27° | 10°/22° | 4°/8° |
|---|---|---|---|---|---|
| Main scene pass, 2× MSAA, 26 draws | 34.7 | 19.8 | 14.3 | 11.5 | 4.2 |
| Second 2× MSAA colour pass, 4 draws | 11.4 | 6.5 | 5.1 | 3.7 | 1.1 |
| Four single-sample full-size passes | 2.1 | 1.9 | 1.4 | 1.1 | 0.5 |
| Other mapped full-size passes (9) | 9.3 | 6.2 | 4.6 | 3.6 | 1.2 |
| Two draws into the swapchain (unmapped) | 1.0 | 1.2 | 1.3 | 1.3 | 1.5 |
| Two 2800×1428 passes (unmapped) | 0.5 | 0.7 | 0.7 | 0.7 | 0.9 |
| Everything else (bloom chain, shadows, small targets) | about 4 | about 3 | about 3 | about 3 | about 3 |
| Sum of sampled passes | 63.5 | 39.7 | 30.4 | 24.8 | 12.6 |

- Every mapped pass shrinks with the profile; the main scene pass goes from
  34.7 ms to 4.2 ms. There is no large cost tied to the full-size targets, so
  allocating them at the physical size would gain little.
- The two scene colour passes are about two thirds of the frame at every
  profile, and that is shading, not MSAA (see below). The profile's core size is the main lever: 12°/27° to 10°/22° saves
  4.6 ms (16%).
- The fixed fallback, used while gaze is lost, costs 8 ms more than the worn
  gaze profile.
- MSAA is already cheap. With MSAA off the baseline is 52.0 ms against 59.6 ms
  at 2×, and the 12°/27° gaze profile 25.8 ms against 28.8 ms; the main scene
  pass is 31.5 ms against 34.7 ms (baseline) and 13.1 ms against 14.3 ms. EXD's
  2× colour targets are compressible (no `PixelFormatView`) and its colour
  resolve is already merged into the pass, so the Alyx-style gain (replacing a
  compute-shader resolve) has nothing comparable to recover here: at most 3 ms.
- 4× MSAA costs about 3 ms more than 2× once foveated (single runs):

  | | MSAA off | 2× | 4× |
  |---|---|---|---|
  | Baseline | 52.0 | 59.6 | 66.1 |
  | Held gaze, `0.3,12,27` | 25.8 | 28.8 | 31.7 |
  | Held gaze, `0.25,10,22` | not run | 24.2 | 26.2 |

  The 4× foveated image matches its baseline (mean difference 2.3, centres 1.9
  to 2.0, the same range as the 2× pair), with no warnings. At 4× the main scene
  pass is 15.5 ms against 14.3 ms; the rest is in the extra multisampled passes
  and the resolve (1.7 ms against 0.9 ms).
- The unmapped work is small: the swapchain draws 1.0–1.5 ms (the most a
  compositor hand-off could save) and the half-size passes under 1 ms.

Restricting mapped passes to the map's physical size was tried again here with
scissor rectangles clamped to match. It ran without GPU errors at 26.1 ms, but
the image was cropped to the top-left 72% × 70%: Metal takes the pass size and
scissor rectangles in logical coordinates, so the time came from drawing less.
Reverted; the default runtimes never had it. Whether targets allocated at the
physical size would work, as Apple's own use of rate maps does, is untested.


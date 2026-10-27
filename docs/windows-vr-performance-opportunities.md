# Windows VR performance: where the time goes and what to try next

2026-10-26. A review of the existing evidence (this repository, Monado's
`doc/macos-steamvr-keyed-ownership-followup.md` and related notes, the native
Alyx workspace in `~/Code/alyx-native`) plus a look at DXMT's render-pass code.
Nothing here is newly measured. Each item says what evidence it rests on.

## Summary

- **The translation-layer question is already settled.** Windows D3D11 games
  reach Metal through DXMT alone, and SteamVR's compositor and the driver use
  the same DXMT device, with zero copies into Monado. DXVK → MoltenVK or
  KosmicKrisp would add a second layer and gain nothing on this path. KosmicKrisp
  still lacks the Metal-object interop Monado needs. Vulkan is still useful as a
  *diagnostic* comparison (below), not as a shipping path.
- **Windows games are GPU-bound now, not sync-bound.** After the DXMT timestamp
  and D3D10-classification fixes (2026-10-14), Home and Alyx get real GPU times
  and their auto-fidelity drops to level 0. Alyx is still over budget: 11.6 /
  16.4 / 29.8 ms GPU (p10/50/90) at 1020×1040 per eye, 120 Hz, through xrizer.
  Through SteamVR it gets 41.5 frames/s with pre-submit GPU at 11–18 ms. The
  long frame gaps match the CPU waiting on GPU backpressure in DXMT.
- **SteamVR's double composition is not the throughput bottleneck.** The Metal
  System Trace of Home showed `vrcompositor` at about 0.6 ms/s of GPU and the
  bridge driver at about 8 ms/s, against 628 ms/s for Home. At matched
  resolution, SteamVR (41.5 frames/s) was not slower than xrizer (34.9). Bypassing
  the compositor would mostly cut **latency** (vrcompositor and driver
  command buffers start 22–32 ms after submission, queued behind the game) and
  lift SteamVR's resolve-size cap. It would not raise the frame rate.
- **The native Alyx result transfers only partly.** VeRantyx's native branch
  and the `alyx-native` workspace run Valve's **Linux ARM64** build (made for the
  Steam Frame) through Klepton and MoltenVK: no Rosetta, no Wine, Vulkan. No
  other SteamVR title has such a build. What does transfer: the measured GPU
  feedback design, timeline handoff, and the gaze/FDM work (for Vulkan titles).

So the biggest levers, in order: make DXMT's GPU work cheaper, render fewer
pixels, hold a steady frame cadence, and only then remove the extra compositor.

## 1. Measure DXMT's GPU overhead against a matched native run (do this first)

Nothing yet measures how much of Alyx's 16 ms is DXMT overhead and how much is
the M5's limit. Alyx is the one game that allows a direct comparison:

| Run | Graphics path |
| --- | --- |
| A | Windows Alyx, D3D11 → DXMT (current) |
| B | Linux ARM64 Alyx, Vulkan → MoltenVK (`~/Code/alyx-native/run-alyx-gpu-timing`) |
| C (diagnostic only) | Windows Alyx with `-vulkan` → winevulkan → MoltenVK |

Use the same save, `+vr_fidelity_level_auto 0 +vr_fidelity_level 0`, the same
per-eye size (set by `OXR_VIEWPORT_SCALE_PERCENTAGE`), the same refresh rate and
a thermal cool-down gate, as earlier runs required. Compare GPU ms per frame
from Metal System Trace (per process, as in the 2026-10-14 trace) rather than
from the games' own timers. Run C separates "Source 2's D3D11 renderer costs
more" from "DXMT costs more"; it is not a proposal to ship DXVK.

[`tools/alyx_gpu_compare`](../tools/alyx_gpu_compare/README.md) runs A, B
and C in alternating order on one offscreen Monado build with the remote
driver, the same save and the session-recording replay. It reports GPU ms
per frame from Metal System Trace and frame timing from Monado's pacing
trace.

### First result, 2026-10-26 (not matched: see the correction)

> **Correction, later on 2026-10-26.** These runs were not matched. The sizes
> below are the OpenXR swapchain sizes, which xrizer always creates at the
> runtime's recommendation; each game renders at its own size and scales into
> it. Without `-msaa N` on the command line, the Windows build ignored the
> fidelity table and rendered its scene at 2800×2856 per eye with **8× MSAA**
> (DXMT texture log), while native rendered 1344×1344 with 2× from its table.
> Windows was doing about four times the pixels at four times the samples, so
> the 2.3× below says little about DXMT. The runner now passes
> `-msaa N +vr_msaa N` to both and reports each game's own render size; see
> "Matched result" below.

Run `~/Code/alyx-native/runs/gpu-compare-20261026-scale50-b`: A and B, A B B A
order, offscreen Monado remote HMD at 4000×2040 and 120 Hz, the same autosave
(`a2_pistol` in every run, checked from each game's console), fidelity auto
off, a one-level override at `vr_render_target_scale` 0.5 with 2× MSAA, head
still, 10 s GPU traces after 150 s.

| | Size per eye (submitted) | GPU ms/frame | GPU ms per Mpixel | Frames/s | Gaps > 50 ms |
| --- | --- | ---: | ---: | ---: | ---: |
| A: Windows, D3D11 → DXMT | 2800×2856 | 58.1, 60.8 | 3.72 | 12.7 | 127 |
| B: native ARM64, Vulkan → MoltenVK | 2784×2784 | 25.6, 24.0 | 1.60 | 33.9 | 4 |

The Windows path needs about 2.3 times the GPU time per pixel. The two
repetitions of each agree within 5%; every run started nominal and ended
"fair" thermally. A Windows run that by mistake stayed in the start-up map
cost about the same per frame (55.6 ms), which suggests much of the extra cost
does not depend on the scene.

What this does not yet show:

- **Whether it is DXMT or the game.** The two are different builds: Valve's
  ARM64 build was made for the Steam Frame's mobile GPU and may render
  differently from the Windows D3D11 renderer. C (`-vulkan` under Wine) would
  separate the two, but wineopenxr offers only D3D11 sessions, so it cannot
  run yet. The next step is a per-encoder breakdown of run A (Metal System
  Trace with the Metal Application instrument and `kk-tests/perf/gpu_passes2.py`),
  checked against the DXMT candidates below.
- The two builds' fidelity levels are not the same sizes: with the native
  game root's override table, level 0 rendered 672×672 per eye, against
  2268×2313 for Windows' level 0. Earlier native-versus-Windows frame-rate
  comparisons "at level 0" compared different amounts of work.

### Where the time goes, 2026-10-26

> These traces and the capture below are of the same unmatched Windows runs
> (2800×2856 per eye, 8× MSAA): the per-encoder costs are real for that
> workload, but larger than at the native settings.

Run `~/Code/alyx-native/runs/gpu-compare-20261026-encoders`: one run of each,
same settings, 8 s full Metal System Traces (kept for now). The same gap again:
54.7 against 22.3 ms of GPU per frame. Split by kind of work, per frame
(channel sums, which overlap a little):

| Per frame | Windows, DXMT (15.8 fps) | Native, MoltenVK (39.2 fps) |
| --- | ---: | ---: |
| Fragment | ~44 ms | ~22 ms |
| Vertex | ~11 ms | ~3.6 ms |
| Compute | ~14.5 ms | ~0.6 ms |
| Standalone clear passes | ~4 (3.1 ms) | none (clears are load actions) |

- **Compute is the largest single difference.** Native's compute dispatches
  are all short (p90 0.62 ms, maximum 1.15 ms). Under DXMT the first compute
  encoder of a command buffer is usually 1–2 ms, but about one in ten runs
  12–18 ms; about 2.4 such encoders per frame. The shader profiler sampled
  almost nothing (it needs Instruments' shader timeline), but all 21 samples
  it took were in one game compute shader, `cs_af359480_9069ca78…`.
- **Clears:** DXMT's standalone `ClearPass` encoders (clears it could not fold
  into the next render pass) are mostly tiny, but p90 2.7 ms and up to
  17.7 ms.
- Fragment and vertex work are also two to three times native's, so the
  compute shader alone does not explain the gap.
- DXMT's encoder labels are positional (`Render Command 4` is the fifth
  encoder in its command buffer), so render passes cannot be named from the
  trace, and the trace's only GPU counter was ray-tracing unit activity.

**DXMT only counts frames at a desktop `Present`.** Alyx in VR (`-nowindow`)
never presents, so DXMT's frame counter stays at 0: its per-frame statistics
never run, its frame-latency limit never engages, and `DXMT_CAPTURE_FRAME`
cannot pick a frame to capture. wineopenxr should mark a DXMT frame boundary
at `xrEndFrame` (DXMT's `CommandQueue::PresentBoundary`, through its native
interface). That makes single-frame Metal captures possible, the definitive
way to see which shader `cs_af359480…` is, what the long clears clear, and
each render pass's load and store actions.

So the DXMT items below are worth the effort, starting from a frame capture.

### Compression, MSAA settings and the pause, 2026-10-26 evening

- **`PixelFormatView` usage costs 2.8× on an MSAA pass.**
  `tools/alyx_gpu_compare/msaa_usage_bench.m`: 5600×2856, 2× MSAA RGBA16F,
  render + store, then a compute resolve reading every sample. M5 medians:
  1.51–1.55 ms with RenderTarget|ShaderRead, 4.23–4.45 ms with
  `PixelFormatView` (draw+store 0.55 → 1.86 ms, resolve read 0.96 → 2.38 ms).
  `ShaderWrite` costs nothing. Metal stores such textures uncompressed.
- **DXMT gives `PixelFormatView` to every texture created with a typeless DXGI
  format** (`d3d11_resource_helper.cpp`), and Alyx creates almost all its large
  render targets typeless: the scene colour (`R16G16B16A16_TYPELESS`), its
  depth-stencil (`R24G8_TYPELESS`), `R8G8B8A8_TYPELESS` targets and most
  smaller ones (`DXMT_LOG_TEXTURE_USAGE=1`, added on `claude/xr-frame-boundary`).
  Default-usage textures are private storage, so compression is otherwise
  possible. Fix candidate: create without `PixelFormatView` while every view
  uses the storage format, and add it (reallocating) only when a view needs
  another format.
- **The Windows build needs `-msaa N +vr_msaa N` on its command line**, or it
  renders its scene at the full recommended size with 8× MSAA whatever its
  fidelity table says (see the correction above).
- **Alyx pauses itself about 6 s after loading the save**, in every run so far
  ("Dr Nick paused the game"). The earlier runs measured the paused view,
  which kept rendering; in the first matched run (`gpu-compare-20261026-matched`,
  Windows 1400×1428 and native 1440×1440 per eye, 2× MSAA) both games stopped
  submitting frames after the pause, so it has no frame rates. The desktop was
  also in use: `WindowServer` used 220–330 ms of GPU a second. Next run:
  unpause (a replayed trigger press, as the native harness does) and keep the
  screen idle.

### Towards an on-demand `PixelFormatView`, 2026-10-26

- **Metal's rules** (`tools/alyx_gpu_compare/view_rules_probe.m`, validation
  layer on, M5): without `PixelFormatView`, a view may use the storage format
  or its sRGB/linear twin (RGBA8, BGRA8). Float ↔ uint, RGBA8 → R32Uint, and a
  stencil view of a depth-stencil texture are rejected. Without the validation
  layer Metal returns those views anyway, so dropping the usage is not safe in
  general (reinterpreting compressed data is undefined).
- **What Alyx reinterprets** (DXMT logs every new view whose format differs
  from storage other than by sRGB): of its large textures, only the
  depth-stencil targets (plane views, which DXMT also needs for its own clears),
  one 512×3072 RGBA16F (a uint view, probably a UAV clear) and one 1024×1024.
  Every large colour target, including the MSAA scene colour, is viewed only in
  its storage format or sRGB twin.
- **Design for the safe version:** colour textures that need no CPU access and
  are not shared are created without `PixelFormatView`. The first
  reinterpreting view or operation (D3D11 view creation, uint clears, copies,
  format resolves, recorded on the application thread) queues a copy into a new
  allocation with the usage, then a rename, in the immediate context's stream:
  commands find a texture's allocation at encode time, as DXMT's Map-discard
  renames already rely on. Depth-stencil textures keep the usage.
- **Measuring first:** `DXMT_PIXEL_FORMAT_VIEW_EXPERIMENT=strip` (measurement
  only, wrong for those few textures) drops the usage from colour typeless
  textures; the runner's `wine-dxmt-strip` variant sets it.
- **Unpausing:** `--replay session-test-synth/session.jsonl --replay-from 40
  --replay-duration 20` presses the right trigger at 45 and 55 s of the
  recording, about 30 s after the save loads (`--replay-start-after`, default
  40 s); the runner records pause events and flags a run that ends paused.
- **Open problem:** with `-msaa 2 +vr_msaa 2` and the runner's one-level
  fidelity override, both builds stop submitting frames a few seconds after
  loading (Monado sees no layers; the game keeps calling `xrWaitFrame`). The
  native harness passes the same options with its seven-level table at level 5
  and works. Until that is understood, `--no-msaa-args` keeps the Windows
  build at full size with 8× MSAA, which suits a Windows-only A/B.
- **Why frames stopped with the MSAA options:** xrizer rejected every frame of
  the game's own with `VRCompositorError_InvalidBounds` (25,884 warnings in one
  Windows run; the same on native). The only frames Monado got were xrizer's
  loading scene. Isolated with short Windows runs:
  `-msaa 2` changes nothing (still full size, 8×); `+vr_msaa 2` is what makes
  the Windows build honour the fidelity table, and with a table scale below 1
  (0.5, one or two levels) Alyx's submit bounds fail xrizer's check (every bound
  in [0, 1], min ≠ max; probably a viewport relative to a smaller texture). At
  scale 1.0 the bounds are valid. So the runner now keeps the table scale at 1,
  passes only `+vr_msaa N`, and sets the size through the runtime's
  recommendation per client (`OXR_VIEWPORT_SCALE_PERCENTAGE`: 50% gives Windows
  1400×1428; native renders a square from the recommendation, 1344×1344 at 50%,
  so it gets 53%). xrizer should accept or clamp these bounds as SteamVR
  presumably does; not yet changed.
### Matched result, 2026-10-26

Run `~/Code/alyx-native/runs/gpu-compare-20261026-matched-2`: A B B A, `a2_pistol`
in every run, unpaused by the replay, fidelity table at scale 1 with
`+vr_msaa 2`, size from the runtime's recommendation, no `InvalidBounds`, 2×
MSAA confirmed for Windows by the DXMT texture log, frames delivered
throughout, every run starting nominal and ending "fair" thermally.

| | Render size per eye | GPU ms/frame | GPU ms per Mpixel | Frames/s (during trace) | Frames/s (100–150 s) |
| --- | --- | ---: | ---: | ---: | ---: |
| Windows, D3D11 → DXMT | 1400×1428 (2.0 Mpx) | 14.6, 14.1 | 3.60 | 37.7 | 51.1 |
| Native ARM64, Vulkan → MoltenVK | 1440×1440 (2.07 Mpx) | 7.8, 9.1 | 2.03 | 62.6 | 83.4 |

**At matched settings the Windows path needs about 1.8× the GPU time per
pixel**, not the 2.3–2.5× of the unmatched runs. This still includes the
renderers' differences (the D3D11 build's compute MSAA resolve against the
Vulkan build's render-pass resolve, and whatever else the Steam Frame build
does differently), so it is an upper bound on DXMT's share. At this size
both games kept the GPU busy only about 53% of the time, so neither is purely
GPU-bound: frame rate also depends on the CPU side, Rosetta and Wine for
Windows.

- **Result of the strip A/B** (`~/Code/alyx-native/runs/gpu-compare-20261026-pfv-strip`,
  Windows only, full size with 8× MSAA, unpaused by the replay, frame rate
  100–150 s after the first frame): normal 12.6 fps, stripped 12.8 and 14.0 fps.
  The second normal run read 5.6 fps, an outlier (the desktop was in use; both
  normal runs unpaused correctly). The colour targets did lose the usage
  (0x15 → 0x5; depth kept it). So compressing Alyx's colour targets gains at
  most about 10% here, far less than the 2.8× of the bandwidth-only benchmark:
  this workload is not dominated by colour-target bandwidth. The safe
  on-demand version is worth doing as a general DXMT improvement, not as the
  answer to Alyx's GPU cost. (xctrace hung finishing the trace in both normal
  runs and in neither stripped run; the runner recovered each time.)

### Frame boundary and the first capture, 2026-10-26

- DXMT branch `claude/xr-frame-boundary` (`~/Code/dxmt-d3d10fix`, on
  `435b2eb`) adds `IDXMTNativeDevice4::EndFrame()`: what `Present` does,
  without presenting. Installed in the rig's runtime and prefix (previous
  `d3d11.dll` in `~/Windows/backups/dxmt-steamvr-gpu-timing-435b2eb-20261026-163303`).
- wineopenxr calls it after each successful `xrEndFrame` when
  `MWXR_DXMT_FRAME_BOUNDARY=1`; off by default, because it also turns on DXMT's
  maximum frame latency. Built in `build-in-process/gate-frame-boundary`; the
  installed `gate` is unchanged.
- Capture: `MTL_CAPTURE_ENABLED=1 DXMT_CAPTURE_EXECUTABLE=hlvr DXMT_CAPTURE_FRAME=3000`
  (the executable name without `.exe`). The first use asks for macOS
  developer-tools permission, and every Metal process under Wine waits until
  it is answered. DXMT writes the capture to the game's working directory as
  seen by Windows, here `game/bin/win64`.
- Result: `~/Code/alyx-native/runs/dxmt-capture-20261026-c/hlvr_F3000.gputrace`,
  5.1 GB, one frame of `a2_pistol` at the matched size. Open it in Xcode to
  read per-pass GPU time, attachments with their load and store actions, and
  the shaders behind the long compute encoders; its command stream has no
  command-line reader.
- Recording the frame stalled the game for several seconds; Monado then
  reported the session lost, and xrizer panicked on `unwrap()` of
  `ERROR_SESSION_LOST` (`src/input.rs:1376`), an xrizer bug.
- That capture caught a frame with only timestamp work, so DXMT gained
  `DXMT_CAPTURE_FRAME_COUNT` (frames per capture). The 10-frame capture
  `~/Code/alyx-native/runs/dxmt-capture-20261026-d/hlvr_F3000-10frames.gputrace`
  (6.3 GB) holds about five game frames: 249 encoders, 5,836 draws, 20
  dispatches. Xcode would only replay it in Lite mode (no per-shader cost or
  limiters).
- **The long compute encoder is Alyx's own MSAA resolve.** The first wide
  compute bar is one dispatch of `cs_af359480…` (the shader the Instruments
  samples found), once per game frame: `dispatchThreadgroups {700, 357, 1}` ×
  `{8, 8, 1}`, 5600 × 2856 threads, both eyes at full size. It reads a
  2D-multisample texture and writes two 2D textures. Source 2's D3D11 renderer
  resolves its MSAA scene in a compute shader; the ARM64 Vulkan build uses its
  render-pass resolve fallback instead (the native launchers' "custom resolve"
  fallback), which Apple GPUs do in tile memory at almost no cost. So part of
  the native gap is the renderer, not DXMT.
- The shader is 256 instructions, mostly ALU. 16 million threads of it should
  take well under the 12–18 ms measured, and DXMT translates `ld_ms` to
  Metal's native multisample read. Unproven suspects: lossless compression
  disabled on the MSAA texture by its Metal usage flags (shader write or
  pixel-format views), which roughly doubles the bandwidth of a full-screen
  pass, and low occupancy. It was resolving **8× MSAA** at full size (see the
  correction above), eight samples per pixel rather than two.
- Render passes start with several `waitForFence` calls before their vertex
  stage (`beforeStages:Vertex|Object|Mesh`), which stops the GPU overlapping one
  pass's vertex work with the previous pass's fragment work. Unmeasured.
- Frame rate with the boundary on is not yet measured cleanly: one 14-minute
  run had 12.0 frames/s against 12.6–13.2 without it, but it also had Metal's
  capture layer loaded and ran four times longer as the machine heated.
If they are all close, go straight to sections 2 and 3.

### DXMT candidates (from reading `src/d3d11/d3d11_context_impl.cpp`, `src/dxmt/dxmt_context.cpp`)

Apple GPUs are tile-based, so load and store bandwidth per render pass is the
usual large cost of translated D3D11:

- **Every render pass loads and stores every attachment** (`load_action =
  Load`, `store_action = Store` at pass start, ~line 4508). DXMT already turns
  a preceding clear into `LoadActionClear`, a following depth clear into
  `StoreActionDontCare`, and merges a following `ResolveSubresource` into the
  pass. Everything else costs a full load and store.
- **`DiscardView`, `DiscardView1` and `DiscardResource` are not implemented**
  (`ERR_ONCE("Not implemented")`, with a FIXME that says to use them for
  `DontCare`). Engines tuned for mobile and for D3D11.1 call these to mark depth
  and MSAA buffers dead after a pass. Check how often Alyx calls them; the
  `ERR_ONCE` message appears in its DXMT log.
- **MSAA** buffers that are resolved and never read again are still stored
  unless a discard or a following clear marks them dead. At Alyx's 4x and 8x
  MSAA levels, that is several times the colour bandwidth.
- DXMT's frame statistics (`clear_pass_count`, `clear_pass_optimized`, shown by
  its swapchain HUD) are a cheap first look at render passes per frame.

These are generic D3D11 improvements, so they also help flat games. They are
upstreamable to 3Shain/dxmt rather than bridge-specific.

## 2. Render fewer pixels, at fixed visual quality

- **MetalFX spatial upscaling on the swapchain path.** DXMT already offers
  MetalFX spatial upscaling for desktop swapchains. The same thing for XR
  swapchains (the game renders at, say, 70% and the image is upscaled into
  the runtime image before release) works for every Windows XR path: wineopenxr
  for OpenXR games, xrizer, and the driver's resolve sets for SteamVR. It
  belongs in the native half of wineopenxr or in Monado, where it is native
  Metal code. This is what OpenXR Toolkit (mbucchia, open source, now archived)
  did on Windows with FSR and NIS.
- **Quad views foveated rendering for D3D11 OpenXR titles.** mbucchia's
  open-source Quad-Views-Foveated API layer gives `XR_VARJO_quad_views` to
  games that support it (several Unreal titles, DCS and others), composing four
  views into two **in D3D11 inside the game's process**. Under Wine that runs on
  DXMT unchanged. The runtime needs only `XR_EXT_eye_gaze_interaction`, which
  the PS VR2 eye-gaze work already targets. Few titles support it, but the
  saving is large for those that do, and the whole stack is established open
  source.
- **SteamVR per-app resolution** already works; the driver should report
  Monado's recommended size so SteamVR's 100% matches the runtime's. The
  2026-10-14 runs showed SteamVR and Monado recommending very different sizes.

## 3. Hold a steady cadence instead of jittering

Alyx's frames are spread across refresh multiples (through SteamVR at 120 Hz:
1: 21%, 2: 19%, 3: 32%, 4: 19%, 5+: 9%). Each change of period shows as judder
even with timewarp. A locked divisor (40 Hz at 120, 45 Hz at 90) with Monado's
reprojection usually feels better than an average of 41 frames/s.

- **SteamVR path:** the driver ignores `PostPresent`'s `Throttling_t`. Set
  `Prop_Hmd_SupportsAppThrottling_Bool` and honour `nFramesToThrottle` and
  `nAdditionalFramesToPredict`. That enables SteamVR's per-app "Throttling"
  setting, the standard way to lock a game to half or third rate on Windows.
- **OpenXR and xrizer path:** a Monado app-pacer option for a fixed minimum
  period per client (the proxy already had a minimum-frame-period policy) does
  the same for games that go direct.
- **90 Hz plus a fixed divisor** is worth a matched trial against 120 Hz.
  SteamVR's larger 90 Hz render target made Home worse at 90 Hz, so pair it with
  the resolution fix above.
- Later: depth layers (`XR_KHR_composition_layer_depth`) let Monado reproject
  positionally, which matters more once games run at a half-rate divisor.

## 4. OpenVR: skip SteamVR's compositor (the VeRantyx-style capture), built on what exists

Expect gains in latency and image quality, not frame rate (see Summary).

The pieces already exist:

- The driver **already allocates the game's eye textures**: SteamVR calls
  `CreateSwapTextureSet` with the game's pid, and `vrclient` copies each Submit
  into them. Allocating them as OpenXR swapchain images, as the compositor's
  resolve sets already are, means the game's frames land in Monado's images
  with no added copy.
- The OpenVR shim already hooks `WaitGetPoses` and `Submit` by vtable, so it
  sees each frame's texture, bounds and render pose. It only needs to pass a
  small per-frame record (set, index, pose, bounds, frame id) to the driver,
  for example through shared memory.
- `SubmitLayer` already builds generic multi-layer `xrEndFrame` submissions.

Then the driver submits the game's set as the projection layer, posed with
the game's own render pose, and Monado's timewarp does the reprojection. It
uses the compositor's resolve set only when SteamVR must draw something:
dashboard, overlays, chaperone, fades, or a game that has stopped submitting.
The switch condition is the risky part. The chaperone is a safety feature, so
fall back to the compositor whenever in doubt.

Cheap experiment first: log the owning pid of each texture set reaching
`SubmitLayer` across Home, a game, dashboard open and closed, and SteamVR
settings. The header says app sets are "passed back to SubmitLayer", and ALVR's
driver composites a variable number of layers, so SteamVR may pass app layers
straight through in some cases. So far this driver has only seen compositor
sets, but that was not tested across those cases.

VeRantyx's version (read 2026-10-26 from the `also/VeRantyx` fork, last
commit 2026-10-18, `VRDriver/src/openvr_capture_proxy.cpp`) is a replacement
`openvr_api.dll` that hooks `Submit` by vtable, as this repository's shim does.
It copies each eye into a D3D11 staging texture, maps it, and copies the pixels
into shared memory for the HEVC encoder that streams to the Vision Pro. Its
comments say it exists because SteamVR could not open the game's shared
textures across Wine processes; DXMT's cross-process sharing fixed that here.
A CPU readback cannot be the design for a zero-copy Metal path. Every fork
found stops at 2026-10-18; the upstream repository had v0.2.1 (23 October) and
later per-draw CPU work, possibly with a GPU-side capture, which cannot be read
now.

The same tree's VXRuntime (`docs/VXRT.md`, `VXRuntime/src/d3d11_shared.m`)
replaces Wine and DXVK with its own Windows API and D3D11 → Metal translator,
still under Rosetta. Its baseline was Wine + DXVK + MoltenVK, CPU-bound at about
30 frames/s, not Wine + DXMT. Its headline 60 frames/s is on an M1 Max (32 GPU
cores); on an M5 Air it reports 27 frames/s median at ALVR defaults, with a
capture tool on. Here Alyx reached 41.5 frames/s on the M5 through SteamVR at
1252×1276 per eye. Neither comparison is matched. Its shared textures are
IOSurfaces; `ReleaseSync` returns at once, and the other process waits **on
the CPU** until a Metal completion handler marks the producer's work done. DXMT's
keyed mutex here already waits GPU-side on an `MTLSharedEvent`, and the
2026-10-14 trace found no synchronisation defect in the steady state.

Two ideas from the capture proxy are worth knowing about: it matches each
Submit to the render pose from `WaitGetPoses`, and it answers `WaitGetPoses`
itself when SteamVR takes focus away from a game whose Submits it never
received.

Alternative already working: for games, xrizer gives the single-compositor path
with established open source and no SteamVR at all. Keep SteamVR for Home, the
dashboard and games that need it. The bypass is worth building when SteamVR is
needed **and** latency matters.

## 5. CPU: Rosetta, and the ARM64 CrossOver preview

CodeWeavers' CrossOver Preview (31 July 2026) runs Wine natively on ARM64 macOS
with FEX for x86 and x86-64 code; release is planned for CrossOver 27 in early
2027, and Rosetta 2 is expected to be mostly gone in macOS 28. For this stack:

- Games and SteamVR stay x86-64 (under FEX instead of Rosetta). Wine's Unix
  side, the wineserver, `winemetal.so` and the native OpenXR client become
  native ARM64. DXMT built as ARM64EC would make the whole D3D11 layer native
  inside an emulated game, the way Windows on Arm runs x64 games.
- The earlier blocker for virtual-display mode (too much x86-64 frame path, and
  ARM64 Wine with FEX needing an entitlement) may be solved by CodeWeavers'
  signed build. Verify before relying on it.
- Costs: the five CrossOver patches and DXMT's cross-process work need
  rebasing; the preview has no D3DMetal; FEX may run game code slower than
  Rosetta. Worth a spike once the preview's FOSS source is published, but not
  before items 1 to 3, since the measured bottleneck is the GPU.

## 6. Monado-side savings that apply to Windows games

Monado's service GPU time was about 172 ms/s during Home at 120 Hz, about
1.4 ms a frame, so a sixth of the 8.3 ms budget goes to the runtime.
`XRT_MACOS_COMPOSITOR_COMMON_LAYERS` and `XRT_MACOS_COMPOSITOR_QUAD_BOUNDS`
(18–23% and 8–17% savings on composition, opt-in, visual acceptance pending)
apply when the SteamVR dashboard or overlays add layers. Promoting them after
visual checks returns budget to the game. Single-projection frames already take
the fast path.

## Suggested order

1. Matched A/B/C GPU comparison (section 1). It decides how much DXMT work is
   worth doing.
2. Driver throttling support and a Monado fixed-divisor option (section 3):
   small, and helps every title now.
3. DXMT discard and store-action work, guided by the trace (section 1).
4. MetalFX spatial upscaling on XR swapchains (section 2).
5. The `SubmitLayer` pid experiment, then the SteamVR bypass if latency
   justifies it (section 4).
6. ARM64 CrossOver spike when its source is available (section 5).

## Sources outside this repository

- [DXMT](https://github.com/3Shain/dxmt) (MetalFX swapchain upscaling, render-pass code read locally in `~/Code/dxmt`)
- [ALVR `OvrDirectModeComponent.cpp`](https://github.com/alvr-org/ALVR/blob/master/alvr/server_openvr/cpp/platform/win32/OvrDirectModeComponent.cpp): layers are collected per `SubmitLayer` and composited by the encoder
- [CodeWeavers: CrossOver Preview, the right to bear ARM64 on Mac](https://www.codeweavers.com/blog/mjohnson/2026/7/31/crossover-preview-the-right-to-bear-arm64-on-mac); [TUAW on its limitations](https://www.tuaw.com/2026/08/02/crossover-goes-native-on-apple-silicon/)
- [FEX](https://github.com/FEX-Emu/FEX)
- mbucchia's Quad-Views-Foveated and OpenXR Toolkit (from prior knowledge; check their current repository status before relying on them)

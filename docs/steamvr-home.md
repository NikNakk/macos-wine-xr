# SteamVR Home under Wine

Goal: run Windows SteamVR (and so SteamVR Home, the dashboard and in-VR game
launching) under Wine, with a bridge-owned SteamVR driver supplying PS VR2
poses from Monado and receiving composited frames through
`IVRVirtualDisplay`. OpenComposite and xrizer cannot provide this: they
implement only the application side of OpenVR.

## Spike 1: null driver under GPTK/D3DMetal, 2026-10-06

Setup: SteamVR 2.17.10 (build 25330290), installed through Windows Steam in
the CrossOver 26.3 + GPTK 4.0b2 Alyx prefix. The run used an APFS clone,
`.build/in-process-openxr-study/build-in-process/prefix-steamvr`, launched by
`build-in-process/steamvr-spike/run.zsh`. `steamvr.vrsettings` forced the
`null` driver (1280x720 window, 1280x1440 per eye, 90 Hz) and disabled the
other drivers. `openvrpaths.vrpath` pointed at SteamVR rather than xrizer. No
Monado service was involved.

| Component | Result |
| --- | --- |
| `vrserver` | Starts; loads `driver_null`; HMD active |
| `vrmonitor` | Starts; launches room setup on first run |
| `vrwebhelper` (dashboard CEF) | Starts and connects to `vrserver` |
| `steamvr_room_setup` (Unity OpenVR app) | Starts |
| `vrcompositor` | D3D11 device, shaders and swap chain created on D3DMetal; then `Failed to create shared frame info constant buffer!` -> `VRInitError_Compositor_CreateSharedFrameInfoConstantBuffer`; retries, then exits |
| Steam connection | Steam was slow to start (`BMainLoop appears to have stalled`); `vrserver` had not connected yet when stopped |

Home (`steamtours`) was not reached because it needs a running compositor.

Blocker: the compositor creates a cross-process shared D3D11 **buffer**
(`D3D11_RESOURCE_MISC_SHARED*` on a constant buffer). D3DMetal does not
provide one and is closed source.

Current DXMT (`macos-xr-native-sharing`) shares **textures** across processes
(Mach port bootstrap registration + D3DKMT allocation, optional keyed mutex)
but has no shared path for buffers, so it would fail at the same step. Adding
shared buffers to DXMT on the texture path's pattern is the next step. It
also needs testing of `OpenSharedResource` across processes, which
`IVRVirtualDisplay`/direct-mode drivers rely on.

Logs: `prefix-steamvr/drive_c/Program Files (x86)/Steam/logs/vr*.txt`.

## Spike 2: null driver under patched DXMT, 2026-10-06

Setup: `.build/steamvr-dxmt` in the Monado workspace, built by
`scripts/build-current-dxmt.zsh` with `MACOS_WINE_XR_CURRENT_DXMT_ROOT` set to
that folder. DXMT is `NikNakk/dxmt` branch `steamvr-shared-buffers`
(`a082160`..`5fe1467`, on `macos-xr-native-sharing` `e4e85d2`). The
runtime is a private copy of Wine 11.10. The prefix is an APFS clone of
`wine11-current-dxmt/prefix`, with SteamVR copied in from the GPTK prefix.
`run.zsh` starts Steam, then `vrstartup.exe`, and writes DXMT logs to
`dxmt-logs/`.

DXMT changes:

1. **Shared buffers.** `D3D11_RESOURCE_MISC_SHARED` and `SHARED_NTHANDLE`
   buffers (default usage, no CPU access, no keyed mutex). A new
   `winemetal` unix call (148, `MTLDevice_newSharedBuffer`) allocates
   16 KiB-aligned pages and wraps them in a Mach memory entry (`MAP_MEM_VM_SHARE`).
   It builds the `MTLBuffer` with `newBufferWithBytesNoCopy`. Importers map
   the same entry. The memory entry travels through the texture path's
   bootstrap name and D3DKMT private data (`SharedResourceData` gains
   `desc_buffer`; its size is unchanged). Shared buffers have no
   `DynamicBuffer`, so updates go into the single shared allocation rather
   than renaming it.
2. **Imported shared resources keep their D3DKMT handle.** The import path
   used to destroy the opened handle at once. The shared object then died
   when the creator released its own copy, even though other processes still
   referred to the global handle. Imported textures and buffers now own the
   opened handle for their lifetime, as on Windows. Before this,
   `vrmonitor` got `STATUS_INVALID_PARAMETER` from
   `D3DKMTQueryResourceInfo` for every compositor mirror texture (Wine trace:
   the creator's local handle and the compositor's own reopened handle were
   both freed straight after creation).
3. **`DuplicateOutput` / `DuplicateOutput1`** return a duplication with no
   frames (`AcquireNextFrame` -> `DXGI_ERROR_WAIT_TIMEOUT`, waiting at most
   100 ms) instead of `DXGI_ERROR_UNSUPPORTED`. Desktop capture itself is not
   implemented (it would need ScreenCaptureKit).
4. DXGI "Not implemented" errors name their method.
5. **Keyed-mutex timeouts.** `KeyedMutex::acquire` computed
   `milliseconds * -10000` in 32-bit unsigned arithmetic. That gave a small
   *positive* NT timeout, which Wine treats as an absolute time in the past, so
   every `AcquireSync` with a non-zero timeout (including `INFINITE`) returned
   at once. It now passes a negative 64-bit relative timeout, or no timeout
   for `INFINITE`.

SteamVR settings (`config/steamvr.vrsettings`) in addition to spike 1:

```json
"dashboard": {"showDesktop": false, "enableWindowView": false, "useNewDesktop": false},
"power": {"pauseCompositorOnStandby": false, "turnOffScreensTimeout": 86400.0},
"steamvr": {"forceFadeOnBadTracking": false}
```

`useNewDesktop: false` is required. The new desktop path
(`vrdesktop_graphics_capture.dll`, Windows.Graphics.Capture) makes
`vrdashboard` exit with `Failed to initialize vrdesktop`. When the dashboard
exits, SteamVR treats Home as quitting and kills `steamtours` after 5 s,
then restarts both. The power settings stop the null driver's
motionless HMD from putting the compositor into standby ("move your headset
to wake it").

Results, in order:

| Change | Result |
| --- | --- |
| Shared buffers only | `vrcompositor` initialises; Home (`steamtours`) renders stereo into the null driver's window (753 presents, 0 dropped, about 2 ms compositor GPU time); the dashboard loops on `vrdesktop`; Home is killed with it; VR View is white |
| + standby settings, `DuplicateOutput` stub, `useNewDesktop: false` | Dashboard stays up and connects to Steam; Home stays up |
| + imported resources keep their D3DKMT handle | No `vrmonitor` shared-texture import failures; VR View is still white |

VR View remains white. `vrmonitor` draws it with its own D3D11 renderer
(`overlay_viewer/d3drenderer_shared.cpp`, thousands of `Present` calls) but
creates no DXMT swapchain: all four swapchains in the run belonged to
`vrcompositor` (the `Headset Window` and two hidden `Static` windows) and
`steamtours`. `vrmonitor` also loads Wine's `wined3d`, D3DX9 and D3DX10, so
its presentation path is still unknown. The null driver's headset window
shows the same image, so this does not block Home or game launching.

The user confirmed Home renders in both eyes in the null driver's window.

### Half-Life: Alyx through SteamVR

`APPID=546560 run.zsh all` asks the running Windows Steam to launch Alyx
(`steam.exe -applaunch`), after SteamVR has started. Alyx uses its own,
unmodified `openvr_api.dll` (identical to `openvr_api.dll.monado-original`).
`VR_OVERRIDE` is not set, so the OpenVR path registry selects SteamVR; no
xrizer or OpenComposite is involved. The game folder is the shared
`build-wine-dxmt/games/Alyx`. The prefix's relative `Half-Life Alyx` link was
one directory short (as in the other prefixes); in this prefix it is now an
absolute link.

SteamVR closes Home for the scene-app transition. Alyx connects to
`vrserver` and `vrcompositor`, and its interstitial web UI loads in
`vrwebhelper`. The user saw the Valve intro (which did not work in earlier
OpenComposite/xrizer attempts) and reached the main menu.

`vrcompositor` logged `AcquireSync FAILED with WAIT_TIMEOUT` /
`ReleaseSync FAILED with E_FAIL` on Alyx's submitted textures: 1581 in the
first three minutes after Alyx connected. With the keyed-mutex fix, the same
window has 48, in short bursts during loading. Those may be genuine
contention. Alyx's own DXMT log was not written; Steam probably does not pass
`DXMT_LOG_PATH` through to games.

Not yet tested: opening the dashboard in VR, launching a game from inside Home
or the dashboard (the null driver has no controllers), gameplay and pacing, and
any PS VR2 or Monado connection.

## The mwxr driver: SteamVR on an OpenXR runtime, 2026-10-06

`src/steamvr_driver` builds `driver_mwxr.dll`, a generic SteamVR driver. It
presents any OpenXR runtime's headset and controllers to Windows SteamVR. It is
an ordinary OpenXR client inside `vrserver`. With the in-process wineopenxr
runtime registered in the prefix, its calls go straight to native Monado.
Nothing in it is specific to the PS VR2 or to Monado.

```text
game (OpenVR) -> renders into driver-allocated shared textures (vrclient)
vrcompositor  -> composites the game and overlays into its own resolve textures,
                 which are OpenXR swapchain images (zero-copy)
vrserver: driver_mwxr
  Present:     keyed-mutex sync, release the images,
               xrEndFrame with projection layers posed from SteamVR's render pose
  PostPresent: xrWaitFrame + xrBeginFrame (paces the compositor), VsyncEvent
  pose thread: xrLocateSpace(VIEW) and both grip spaces at the current time,
               xrSyncActions, 500 Hz
  -> wineopenxr (in process) -> native Khronos loader -> Monado client -> service
```

The game's frames are composited twice: once by SteamVR, once by the runtime.
With `IVRDriverDirectModeComponent` (the interface SteamVR's Oculus driver uses
with LibOVR), applications render undistorted eyes into textures the driver
allocates (`CreateSwapTextureSet` for the application's pid). `vrcompositor`
then always composites them, with overlays, fades and its own reprojection,
into "driver direct mode resolve textures" of its own (sets created for
`vrcompositor.exe`), and only those reach `SubmitLayer`. The runtime then does
distortion and timewarp. This pass is built into SteamVR's direct mode; SteamVR
on Oculus headsets has the same double composition. Valve's private compositor
plugin interface (`IVRDriverDirectInternal`) is not public. SteamVR sizes the
resolve textures itself (1292-1440 x 1452-1468 per eye so far, below Home's
1549 x 1742 render), so this pass also limits resolution.

**Zero-copy.** With DXMT's `IDXMTNativeDevice3::CreateSharedTextureHandle`, each
of the compositor's resolve sets is an OpenXR swapchain whose three images are
published to `vrcompositor`. The compositor renders straight into the runtime's
images and the driver makes no copy. The first image is acquired at creation
(the compositor renders into it first); later ones are acquired in
`GetNextSwapTextureSetIndex` (the runtime decides the order) and released in
`Present` after the keyed-mutex wait. Applications' sets, which never reach
`SubmitLayer`, stay plain shared textures. A compositor set falls back to one
copy per eye into a separate swapchain when its format is not a runtime
swapchain format, it is multisampled, the swapchain does not have exactly three
images, or the device cannot export them. Building without `DXMT_SOURCE_DIR`
always copies. The driver setting `zeroCopy` (default true) allows A/B
comparison. Checked against simulated Monado: the compositor's sets were
zero-copy, Home's were shared textures, with no copies or `xrEndFrame` errors.

### What the driver presents

- **HMD.** Resolution, FOV, eye-to-head poses and IPD come from the primary
  stereo views, located in VIEW space right after `xrBeginSession` (Monado
  locates views only in a begun session). The reference space is STAGE when
  offered, otherwise LOCAL. Distortion is identity, and its inverse is left to
  SteamVR (returning an identity inverse fails `BatchedComputeDistortion`). The
  display frequency follows the shortest `predictedDisplayPeriod` seen, because
  late frames report multiples of the refresh period.
- **Controllers.** One action set with bindings suggested for
  `oculus/touch_controller` (fallback `khr/simple_controller`); Monado binds
  PS VR2 Sense controllers to Touch. Both hands are presented as
  `oculus_touch`, with Valve's `{oculus}/input/touch_profile.json`, Quest 2
  render models and the grip pose, so SteamVR's Touch bindings apply. The left
  menu button is the system button. Haptics go to `xrApplyHapticFeedback`. There
  is no skeletal input yet.
- **Play area.** A standing universe at the reference-space origin, as
  `Prop_DriverProvidedChaperoneJson_String` (key `jsonid`). It uses the STAGE
  bounds when available, otherwise 2 x 2 m, so SteamVR does not require room
  setup.
- **Time.** `XR_KHR_win32_convert_performance_counter_time` when the runtime
  has it (the in-process runtime maps it to native
  `XR_KHR_convert_timespec_time`, as Proton does). Otherwise the time is
  estimated from the frame loop, running ahead by the runtime's display lead.

### MinGW and SteamVR's calling convention

For a C++ member function that returns a struct by value, MSVC (SteamVR)
passes `this` first and the hidden result pointer second; MinGW GCC 16 passes
the result pointer first. `ITrackedDeviceServerDriver::GetPose` and
`IVRDisplayComponent::ComputeDistortion` return structs. Implementing them
directly made SteamVR read garbage distortion and fail with
`VRInitError_Compositor_CreateDistortionSurfaces`. `openvr_abi.h` declares
those two interfaces with an explicit result parameter, which has MSVC's layout
under either compiler. Any further OpenVR interface method that returns a
struct by value needs the same treatment.

### Building

```sh
# In-process runtime with Win32 time conversion (separate from build-in-process/gate):
MWXR_IN_PROCESS_BUILD=$PWD/build-in-process/gate-steamvr \
DXMT_SOURCE_DIR=/path/to/dxmt MWXR_WINE_SDK=/path/to/wine-8.16-sdk \
MWXR_WINE_SOURCE=/path/to/proton-wine MWXR_WINE_RUNTIME=/path/to/wine-11.10 \
MWXR_NATIVE_LOADER=$PWD/build-in-process/openxr-loader-x64/src/loader/libopenxr_loader.dylib \
OPENXR_SOURCE_DIR=/path/to/OpenXR-SDK scripts/build-in-process-gate.zsh

# Driver (the Khronos loader and MinGW runtimes are linked statically):
cmake -S src/steamvr_driver -B build-in-process/steamvr-driver -G Ninja \
  -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DDXMT_SOURCE_DIR=/path/to/dxmt   # optional: zero-copy (IDXMTNativeDevice3)
cmake --build build-in-process/steamvr-driver
```

The SteamVR side needs DXMT with both the in-process import interface and the
SteamVR fixes (`NikNakk/dxmt` branch `steamvr-in-process`). It is installed
into a Wine 11.10 root by `scripts/build-current-dxmt.zsh`, whose prefix holds
Windows Steam and SteamVR.

### Running

```sh
export MWXR_STEAMVR_ROOT=/path/to/steamvr-dxmt          # wine-11.10/, bin/, prefix/
export MWXR_NATIVE_RUNTIME_JSON=/path/to/monado-x64/openxr_monado-dev.json
MWXR_MONADO=hardware scripts/run-steamvr-mwxr.zsh          # installed Monado service, PS VR2
MONADO_SIM_BUILD=/path/to/sim-service MWXR_MONADO=simulated scripts/run-steamvr-mwxr.zsh
SIMULATED_LEFT=wmr SIMULATED_RIGHT=wmr MONADO_SIM_BUILD=... MWXR_MONADO=simulated scripts/run-steamvr-mwxr.zsh
APPID=546560 MWXR_MONADO=hardware scripts/run-steamvr-mwxr.zsh   # then launch Half-Life: Alyx
```

The script registers the driver folder as an `external_drivers` entry in
`openvrpaths.vrpath`. It sets `forcedDriver: mwxr`, disables the null driver and
SteamVR safe mode, and applies the dashboard and power settings from spike 2.
It starts Steam, then `vrstartup.exe` with the in-process runtime's
environment, which `vrserver` inherits. The x86_64 Monado client must match the
service's git tag. In simulated mode it starts an isolated simulated-only
service under its own launchd label, and removes it when `vrserver` exits.

Operational notes:

- **Stop SteamVR by closing its status window** (or
  `wine taskkill /im vrmonitor.exe`), not `wineserver -k`. A crash early in a
  run sets `blocked_by_safe_mode`, and SteamVR re-applies it from a saved crash
  record at the next start ("Not loading driver mwxr because it was blocked by
  a previous safe mode event"). One clean run clears it.
- Shutdown can hang at "Exiting" while SteamVR repeatedly kills a `steamtours`
  process that no longer exists. Kill `vrserver`, `vrcompositor` and
  `vrmonitor`.
- Monado's macOS compositor presents only on the PS VR2 panel, so the
  simulated service shows nothing on the desktop. Simulated runs are checked
  through the driver's logs.
- `vrstartup.exe` exits with status 3 even on success.

### Diagnostics

Every 5 s the driver logs to `vrserver.txt` (prefix `mwxr:`):

- the head pose;
- each submitted layer's texture region, pose and FOV;
- SteamVR's prediction horizon (`flHmdPosePredictionTimeInSecondsFromNow`) and
  the angle between the render pose and the current head pose;
- each hand's action values.

The startup line names the runtime, the system, the reference space and the
time source.

### Results

Simulated Monado v25.1.0-2146: SteamVR loads the driver, the compositor starts
on it, and Home renders into driver textures, submitted without `xrEndFrame`
errors. The time estimate, with the extension hidden, gave valid poses and
frames.

PS VR2 (installed service v25.1.0-2146, `XRT_MACOS_CLIENT_COMPOSITOR=1`):

- Home renders in stereo, distorted by Monado, at 1428-1440 x 1456-1468 per
  eye (SteamVR's choice from Monado's recommended 2800 x 2856).
- STAGE is used. The head is at 1.77-1.80 m while worn, after Monado's
  automatic floor calibration (`XRT_FLOOR_EYE_HEIGHT_M=1.75`).
- The render pose is within 0.06-0.13 deg of the head when still, and 1-3 deg
  when turning. SteamVR predicts 41-45 ms ahead (about 9 ms against simulated
  Monado).
- Both hands bind Touch. SteamVR shows both controllers as tracking, and
  trigger/squeeze values reach the driver.
- Before the `jsonid` fix the play area was rejected ("Could not find
  universe"). The view had a constant black lower region bounded by a straight
  edge, and faded beyond about 30 deg of head turn. The likely cause is that,
  with no standing universe, apps placed the eyes at floor height, so the
  floor was seen edge-on. This is unconfirmed: the fixed build has not yet
  been run on the headset.

### Open issues

- Controllers were not drawn in Home and Home did not react to them, although
  their values reached the driver. With simulated WMR controllers
  (`SIMULATED_LEFT=wmr SIMULATED_RIGHT=wmr`, bound through the driver's
  `khr/simple_controller` fallback), SteamVR presents both as `oculus_touch`,
  builds the Quest 2 render-model templates, and loads Home's
  `bindings_touch.json`; Home loads the model components and processes the
  actions, with no input errors. The binding chain therefore works. The
  likely cause on the headset was the missing universe: with the eyes at
  floor level, the hands were below Home's floor. Recheck on the PS VR2 after
  the `jsonid` fix.
- SteamVR's 41-45 ms prediction on the PS VR2 is long, and comes from the
  vsync timing the driver reports. Worth tuning with real pacing data.
- "WaitForAcquire timed out" appears occasionally during hitches.
- Game Mode: frames now pass through `vrserver`, which macOS may throttle like
  `monado-service`. Not yet measured.
- No skeletal input, battery or proximity.

### Virtual-display mode: SteamVR as the only compositor, 2026-10-06

Goal: one compositor for the SteamVR path. SteamVR's compositor produces the
panel-ready image; Monado only owns the device and display, the timing and the
final Metal present. Direct mode cannot provide this: its resolve textures are
undistorted by contract. `IVRVirtualDisplay` can, because SteamVR's compositor
then distorts with the driver's `ComputeDistortion` and hands over "the final
backbuffer for display".

Stage 1 (this section) proves the SteamVR side. Presenting the backbuffer
through Monado's macOS presenter is stage 2.

- **Distortion from the runtime.** Monado's new experimental
  `XR_MNDX_display_distortion` (worktree `.build/monado-display-distortion`,
  branch `claude/display-distortion-mndx`, `f34bb674b`; off by default,
  `-DXRT_FEATURE_OPENXR_MNDX_DISPLAY_DISTORTION=ON`) exposes the display
  size, each view's viewport and distortion FOV, and batched per-channel
  distortion evaluation through `xrt_device_compute_distortion`. It is the
  same mapping as SteamVR's `ComputeDistortion`; the PS VR2 function
  (`psvr2_compute_distortion_asymmetric(eEye, fU, fV)`) already has that
  shape. IPC clients use the existing device call, so the service is
  unchanged. The in-process runtime passes it through
  (`mndx_display_distortion.h`, one extra unix entry).
- **Driver.** `driver_mwxr.displayMode` = `virtual` (default `direct`)
  presents `IVRVirtualDisplay` instead of the direct-mode component when the
  runtime has the extension. The window bounds are the display, the eye
  viewports and FOVs come from the extension, and `ComputeDistortion`
  evaluates it exactly. `WaitForPresent` currently paces with an empty OpenXR
  frame (the headset shows nothing yet), and `GetTimeSinceLastVsync` derives
  from it. `virtualDisplayDumpDir` writes presents 300 and 1200 as PPM.
- **DXMT fix.** The backbuffers are `MISC_SHARED_KEYEDMUTEX`, and the
  compositor acquires and releases them on different threads. Wine's server
  ties keyed-mutex ownership to the thread and rejected the releases
  (diagnostic: acquired on thread 3472, released on 4148,
  `STATUS_INVALID_PARAMETER`), so every later `AcquireSync` failed (about 15
  a second). DXMT now runs each mutex's D3DKMT acquire and release on one
  owner thread (`0249f5b`).

Against simulated Monado (new x86_64 client from the worktree, tag still
v25.1.0-2146): SteamVR's compositor started in virtual-display mode with three
backbuffers at the display size (1280 x 720), `R8G8B8A8_UNORM` (28),
render-target and shader-resource bindings, `MISC_SHARED_KEYEDMUTEX`. With the
DXMT fix there were no keyed-mutex failures. The dumps are the full display,
side-by-side eyes, undistorted because the simulated HMD's distortion is
identity. PS VR2 distortion parity (UV conventions, chromatic aberration, mesh
resolution) needs the headset: its calibration comes from the device.

Findings for stage 2:

- Sync is the backbuffer's keyed mutex, which DXMT backs with an
  `MTLSharedEvent`, so the presenter can wait for it GPU-side.
- The format is RGBA8. The 8-bit `CAMetalLayer` formats are BGRA, so the
  final copy into the drawable must swizzle: a small draw rather than a blit.
- Monado's presenter already ends every frame with a blit from an
  IOSurface-backed image into the drawable (`comp_window_macos.m`), so a
  precomposited frame costs no more than today's final step.

Version note: the client must match the service's git tag. The worktree
build was configured before the commit, so it still reports 2146. A fresh
configure reports a new tag and needs `IPC_IGNORE_VERSION=1` until the
service is rebuilt (the IPC protocol is unchanged).

### Presenting the virtual display (stages 2 and 3), 2026-10-06

The driver now submits SteamVR's backbuffer, and Monado presents it without
compositing. The frame path in virtual mode:

1. SteamVR's compositor renders and distorts into its backbuffer.
2. `IVRVirtualDisplay::Present`: the driver takes the backbuffer's keyed
   mutex (key 0), copies it into a display-sized OpenXR swapchain image
   (`SAMPLED | TRANSFER_SRC`, same format) and releases both. It then ends the
   frame with one projection layer carrying `XrCompositionLayerDisplayImageMNDX`.
   `WaitForPresent` waits for and begins the next frame.
3. Monado (`claude/display-distortion-mndx`, `ad9902882` and `9a30427fd`)
   sees a frame whose only layer is a display image. It skips distortion,
   timewarp and its own target image. It passes the swapchain image's
   `MTLTexture` to the macOS presenter (`comp_target::present_external`),
   which draws it into the `CAMetalLayer` drawable. The draw swaps RGBA to
   BGRA, and scales when the sizes differ. A GPU-reuse claim keeps the image
   from the application until the presenter's command buffer completes.
   Targets without `present_external` blit the image instead.

The in-process runtime passes the marker struct through `xrEndFrame`, and now
allows `TRANSFER_SRC` swapchain usage.

GPU work per frame is one copy in D3D11 and one draw into the drawable,
instead of a resolve, Monado's distortion pass and a blit. The copy could go
later, by presenting SteamVR's backbuffer directly (DXMT exporting its Metal
texture and the keyed mutex's shared event), but that needs new DXMT and
runtime plumbing for about one copy.

Simulated HMD on the attached PS VR2 panel. The service was built from the
worktree without the PS VR2 USB driver; the x86_64 client's tag matched.
`XRT_MACOS_CLIENT_COMPOSITOR` was on, so the presenter ran inside vrserver:

- `Presenting an externally composited 1280x720 image (format 70, drawn)`.
  Format 70 is RGBA8. The simulated display is 1280 x 720, scaled to the
  4000 x 2040 drawable.
- The presenter's Vulkan wait was 0.000 ms: Monado rendered nothing.
- Once warmed up, it held 120 Hz: completion cadence 8.34 to 8.45 ms average,
  0 to 4 late per 240 frames. The first two windows, while SteamVR started,
  had 15 and 73 late.
- No keyed-mutex or `xrEndFrame` failures.

The run script now passes `XRT_*_LOG` variables (for example
`XRT_COMPOSITOR_LOG=info`) to the simulated service.

### Virtual-display pacing on the PS VR2, 2026-10-06

The first PS VR2 run of virtual mode (Monado `5f948c72c`, bridge `652017b`)
presented at a steady 120 Hz, and the image was correctly distorted. It felt
less smooth than direct mode. Client-side traces (`PSVR2_TIMING_TRACE=1`)
showed SteamVR completing only 37.7 frames a second, mostly every third
refresh, so each image was shown three times. Direct mode hides that: Monado
re-warps every repeat with a fresh pose. In virtual mode nothing does, by
design.

The cause was the timing the driver gave SteamVR:

- `GetTimeSinceLastVsync` was based on `predictedDisplayTime - period`, but
  Monado's app pacer predicts several refreshes ahead (about 51 ms here). The
  "last vsync" was therefore about 33 ms in the future, and SteamVR waited for
  it.
- SteamVR's slow frames then raised Monado's estimate of the app's frame time
  (about 15 ms of "draw"), which pushed the prediction further ahead.
- Virtual mode also kept the 90 Hz default display frequency; only direct
  mode adopted the runtime's period.

The fix (driver):

- **Last vsync:** report the latest vsync that has already happened (display
  time minus whole refresh periods).
- **Vsync to photons:** set `SecondsFromVsyncToPhotons` to the remaining whole
  periods, so SteamVR's estimate equals the runtime's display time.
- **Vsync counter:** derive it from the vsync time.
- **Display frequency:** start from the extension's nominal refresh rate.

On the PS VR2, the result:

- SteamVR ran at 116 to 120 fps after start-up (109 fps over the whole 95 s
  run).
- 98.9% of frames advanced the display time by one refresh.
- The app lead fell from 51 ms to 32 ms.

That run was launched as a direct-mode comparison. A concurrent simulated
launch rewrote `displayMode` to `virtual` before vrserver started, so it ran
in virtual mode against the PS VR2 service. A clean direct-mode comparison is
still to do.

In that run, `SecondsFromVsyncToPhotons` changed nearly every frame,
alternating between 4 and 5 refreshes (25 / 33 ms), and reaching 9 during
start-up. Each change shifts SteamVR's pose prediction by a refresh, which
shows as the world (not the compositor-drawn controllers) jumping.

Taking the median of 60 frames still switched every few seconds. The driver
now checks every 60 frames, and changes the value, to that median, only when
none of those frames agreed with the current one. That change is not yet
tested on hardware.

Later PS VR2 runs, 2026-10-06:

- Home renders properly in virtual mode, at normal height.
- SteamVR's frame-timing graph showed 4 to 5 ms at 120 Hz. Home itself
  dropped 699 of 6,760 frames (10%, 550 during start-up). That is why the
  world felt less smooth than the controllers, which the compositor draws.
- A fade to SteamVR's grid at a fixed head angle (independent of turn
  speed) happens in both display modes. It is not a frozen frame (Home
  and the tutorial animate) and not the play-area size (4 x 4 m via
  `playAreaSize` made no difference). As a next check, the driver no longer
  claims `DriverProvidedChaperoneVisibility`, so SteamVR manages the boundary
  itself.
- SteamVR waited for the headset to be worn. The driver now reports a
  proximity sensor that reads as worn. The tutorial's desktop window always
  says "Put on your headset", so that text is not itself an error.

### Home: the HMD render pose is invalid under Wine, 2026-10-06

`tools/openvr_probe` (run with `scripts/run-openvr-probe.zsh` while SteamVR is
up) showed why Home misbehaves while the Unity tutorial works:

- `WaitGetPoses` returns the HMD with `bPoseIsValid = false` and
  `TrackingResult_Uninitialized` on every frame the application keeps up,
  while the pose values are live. It is valid on frames where the
  application is late, when the client computes poses itself. Home
  respects the flag and keeps its last good view. SteamVR's compositor then
  re-projects Home's frames until they no longer cover the view, which is
  the fade at a fixed head angle, the vanishing cabin after recentring, and
  probably the dead controls. Unity's tutorial ignores the flag.
- The same happens with SteamVR's own null driver, so it is not
  driver_mwxr.
- Ruled out:
  - **Tracking space:** standing, seated and raw are all invalid; the
    chaperone calibration state is OK.
  - **HMD activity:** the HMD reports user interaction.
  - **Display timing:** the vsync counter base, and the vsync timing in
    both modes.
  - **Prediction time:** client queries are valid from -1 day to +1 day;
    SteamVR clamps.
  - **Compositor clock:** its frame time counts from SteamVR's start.
  - **`forceSystemLayerUseAppPoses = false`:** tried and reverted.
  - **Focus, frame drops and reprojection:** none were seen.
- Client queries (`GetDeviceToAbsoluteTrackingPose`) are valid in every
  space. The controllers' render poses are valid. The compositor's
  vsync signal thread runs.

The fault is in how SteamVR's compositor produces the HMD's render pose
when running under Wine, inside a closed binary. Options are a client-side
workaround for apps that respect the flag (Source 2: Home, probably Alyx),
or reverse engineering vrcompositor.

### Home: frames stop after the first, 2026-10-06 (continued)

The pose flag above turned out not to be what stops Home. Measured inside
Home with `tools/openvr_shim` (installed beside Home by the launcher; it
logs to the run's `openvr-shim.log`), with the HMD pose marked valid
(`MWXR_OPENVR_SHIM_FIX_POSE=1`) or not:

- Home called WaitGetPoses at up to about 30 per second, but submitted
  only about 6 frames per eye in 5 s, with gaps of 2 s and more. In other
  5 s windows it made almost no calls at all (gaps of 6 to 13 s).
- Submit itself takes 4 ms on average (16 ms at most), without errors;
  one shared texture is used for both eyes.
- Marking the HMD pose valid changed nothing.

What the user sees: one real frame, then a frozen scene. SteamVR
re-projects that frame, which explains the fade and the unresponsive
controls. The frame-timing graph shows every frame reprojected.

State of the frozen process:
- `sample`: every thread is waiting in Windows waits. None is in Metal,
  and DXMT's encode and finish threads are idle.
- winedbg: the D3D11 render thread (`rendersystemdx11`) waits with no
  timeout for work.
- Home's developer console (port 29009, `-vconport`) logs "Submitting
  first frame to the compositor", Panorama event backlogs of about 900,
  and then nothing new.
- Tracing (`WINEDEBUG='-all,steamtours.exe:+seh,steamtours.exe:+thread,...'`)
  showed no fault exceptions. The main thread stayed alive, and
  repeatedly enumerated all threads (`NtGetNextThread`, about 900 times).

`Steam/dumps` holds many Home assert dumps from earlier in the day. They
are Steam networking asserts: lock-wait warnings and GetAdaptersAddresses.
The two latest sessions wrote none, so the dumps do not explain the
freeze. Still open: what Home's main loop waits for after its first
frame.

Last run of the day, with Steam's overlay kept out of SteamVR and Home
(`MWXR_WINEDLLOVERRIDES='gameoverlayrenderer64='`, confirmed absent from the
process), and Home's console recorded live from start-up:

1. 3.3 s: Home spawns the player.
2. 3.8 s: "Submitting first frame to the compositor".
3. 4.0 to 6.6 s: UI updates and Panorama backlogs.
4. After that, nothing from the game loop; only background network pings
   follow (7.1 and 10.2 s).

While frozen, Home's process (`0x1248`) had no thread `0x124c`, normally
its main thread under Wine. The overlay is therefore not the cause. Next:
trace Home's main thread (waits and exit) from launch to the freeze, or
try other titles (Half-Life: Alyx, Unity games) first.

### CrossOver 26.3 Wine with msync, 2026-10-07

The CrossOver rig (`.build/steamvr-crossover`) runs the same launcher with
CrossOver 26.3's FOSS Wine (Wine 11.0, x86_64 only) and the SteamVR-patched
DXMT. `MWXR_WINE_TREE` and `MWXR_WINE_WRAPPER` select it. The rig's
`run-mwxr.zsh`, `run-openvr-probe.zsh` and `stop-steamvr.zsh` set these, along
with `WINEMSYNC=1`.

Setting up the Wine tree:
- Copy the CrossOver dependency libraries (`deps-x86_64/lib`) into the tree.
- Link `lib/libvulkan.1.dylib` to `libMoltenVK.dylib`; D3DKMT needs it.
- Copy DXMT's `d3d10core`, `d3d11`, `dxgi` and `winemetal` into the tree and
  into the prefix's `system32`.
- Run `wine wineboot` with `WINEDLLOVERRIDES="mscoree=;mshtml="`, so it does
  not stop at the Mono dialog.

**Keyed mutexes under msync.** Stock CrossOver wineserver hangs the first
cross-process `D3DKMTAcquireKeyedMutex`: vrcompositor waits forever for
the driver's backbuffer. msync waits in-process on internal syncs, but a
keyed mutex's wait sync is only ever signalled by the server.
`patches/crossover/0001-server-keyed-mutex-waits-on-server-syncs.patch` creates
that sync with `create_server_internal_sync`, as `debugger.c` already does.
Rebuild with the macOS 26.5 SDK, then copy `server/wineserver` into the tree:
`SDKROOT=.../MacOSX26.5.sdk make server/wineserver`.
`tools/keyedmutex_test` checks the fix: 2,000 cross-process round trips, no
timeouts, where the stock server hangs on the first one.

**Measurements.** Simulated service, virtual display mode, idle SteamVR with
Home, and `tools/openvr_probe` for 14 s. CPU is per process, over 8 s.

| Wine | Compositor | Probe fps | vrserver | wineserver | Service |
| --- | --- | --- | --- | --- | --- |
| 11.10 | in vrserver | 6–53, mean 23 | 70% | 39% | – |
| CrossOver, msync off | in vrserver | mean 24 | 67% | 43% | – |
| CrossOver, msync on | in vrserver | 25–88, mean 59 | 73% | 30% (earlier run) | – |
| 11.10 | in service | mostly 90, dips to 28 | 36% | 52% | 47% |
| CrossOver, msync on | in service | 90 throughout | 35% | 33% | 49% |

Correction: the two "in service" rows ran in direct mode, not virtual. With
the compositor in the service, Monado's IPC client did not copy the display
description, so XR_MNDX_display_distortion was refused ("View 0 is rotated").
Monado commit `3a3afbf2a` fixes this; those rows need repeating.

"In vrserver" means `XRT_MACOS_CLIENT_COMPOSITOR=1`, which this machine sets
globally with `launchctl setenv`. The simulated service paces at 90 Hz.

Where vrserver's time goes:
- SteamVR's own null driver uses about 4% of vrserver. The rest is the
  mwxr driver and the Monado client it loads.
- The driver's pose rate (`poseRateHz` 500, 250, 120) changes vrserver
  only from 78% to 73%.
- With the compositor in vrserver, a Time Profiler trace shows:
  - most of the time on GCD threads submitting Metal command buffers and
    acquiring `CAMetalLayer` drawables;
  - the drawable pool keeps allocating new IOSurfaces;
  - all of this runs under Rosetta, because vrserver is x86_64.
- Metal System Trace: about 830 command buffers per second. About 700 of
  these come from DXMT; each swapchain release commits a DXMT signal, the
  bridge's wait and Monado's empty command buffer.
- vrserver's main thread opens and queries other processes about 1,300
  times a second. That is SteamVR's own process polling, and each call is a
  wineserver round trip.

Conclusions:
- msync more than doubles the frame rate.
- Moving the compositor back into the native service halves vrserver's
  load and makes pacing steady.
- For SteamVR, prefer `XRT_MACOS_CLIENT_COMPOSITOR=0` unless Game Mode
  throttling is shown to matter. To try it on the PS VR2:
  `XRT_MACOS_CLIENT_COMPOSITOR=0 MWXR_MONADO=isolated ...`.

Home under CrossOver: `wineserver -d1` shows Home's first thread alive and
spinning. It makes about 3,500 round trips a second on a Steam IPC pipe:
writes of 5 and 13 bytes, then a 4-byte read. Steam's side meanwhile polls
with `FSCTL_PIPE_PEEK`. This is a lead for the stopped game loop: Home may be
waiting on a Steam call that never completes.

A one-off vrserver crash (execute fault in `kernel32`, no driver frames)
put SteamVR into safe mode. The launcher clears `blocked_by_safe_mode`, but
vrserver blocks the driver once more from its saved crash timestamp; the
next start loads it again.

### Home main-thread fault captured: Workshop dependency recursion, 2026-10-07

The termination investigation now has a concrete fault, superseding the
unqualified missing-main-thread and incomplete-Steam-IPC hypotheses above.
This was a simulated-only run; no PS VR2 hardware was used.

Source: macos-wine-xr worktree `03c7a73f83a1c08ed005ee1b9452890f23b4f85f`.
Wine 11.10, the existing SteamVR/DXMT prefix and mwxr driver, with matching
Monado simulated service and x86_64 client tags `v25.1.0-2151-g6b828a9ec`.
The decisive run used `XRT_MACOS_CLIENT_COMPOSITOR=0`. Home's `client.dll`
SHA-256 was `c331ca4fc37723dd496d06930049a5f847433aa169b70943a4511b080fa16f96`.

An automated Windows debugger attached before Home finished startup. It
armed `ntdll!RtlExitUserThread`, `ntdll!NtTerminateThread` and
`kernelbase!TerminateThread`, recorded debugger thread-exit events, and sampled
the original application thread. Normal helper-thread exits were captured
with zero status and their caller stacks, demonstrating that the termination
capture worked. No termination call for Home's original thread was recorded
before the fault.

In the decisive run Home was Windows PID `0x0fe0`, original TID `0x0fe4`:

- At debugger elapsed 51.335 s, the main thread was inside `steamclient64.dll`
  beneath repeated `client.dll+0x4f1a1a` frames.
- At 53.614 s, that thread raised first-chance `0xc00000fd`
  (`STATUS_STACK_OVERFLOW`), at `kernelbase.dll+0x681d9`.
- Wine then logged a Rosetta synchronous-exception error. The native failure
  and a subsequent suspension attempt prevented a complete thread-exit trace;
  this run does not establish how the final thread disappearance is reported.
- Reading the retained main-stack memory found **8,008** occurrences of the
  recursive return address. Frames were 128 bytes apart, with the same object
  pointer and alternating saved item IDs **3149046643** and **2289310332**.

Disassembly identifies `client.dll+0x4f14f0` as
`CWorkshopContentManager::GetItemStateAndDownloadIfRequested`, using its
referenced assertion strings (`steamtours_workshop.cpp`). At `+0x4f1a15` it
calls itself while traversing a cached list of dependent Workshop items;
`+0x4f1a1a` is the return address. The saved item arguments were decoded from
the function's actual prologue, rather than inferred from Steam IPC traffic.
This is strong evidence of a Workshop dependency cycle in Home's cached
traversal data, causing stack exhaustion. It explains why the previous Steam
IPC loop was seen: each recursive call queries item state through Steam.
The follow-up below confirms the same cycle in published metadata. The
recursion does not implicate the Wine XR proxy or mwxr frame transport.

The OpenVR shim logged sparse pose calls and zero submits in its emitted
windows during this run. The previously observed single visible frame was not
independently confirmed here; do not use this run as visual rendering proof.
An initial high-overhead relay run and a manual-launch repeat did not reach
the same fault and are not validation of continuous rendering.

Evidence in the Monado workspace:

- `.build/steamvr-home-termination/clean-trace.log`
- `.build/steamvr-home-termination/workshop-stack-memory.log`
- `.build/steamvr-home-termination/client-recursion.asm`
- `.build/steamvr-home-termination/trace_threads.cpp` (temporary diagnostic)
- `.build/steamvr-dxmt/run-mwxr-20261007-071337/`

### Workshop metadata and exclusion test, 2026-10-07

The public Steam Workshop pages independently confirm reciprocal requirements:

- [Aperture Focus Glass Assets (3149046643)](https://steamcommunity.com/sharedfiles/filedetails/?id=3149046643)
  requires item 2289310332.
- [Aperture Focus Glass Environment (2289310332)](https://steamcommunity.com/sharedfiles/filedetails/?id=2289310332)
  requires item 3149046643.

Saved HTML is `item-3149046643.html` and `item-2289310332.html` in the
termination evidence directory. Steam's `workshop_log.txt` reported zero
installed, needed, or subscribed items for AppID 250820; there was no local
`steamapps/workshop` directory. Home therefore encounters this cycle while
querying browsable Workshop metadata, even without subscribing to these items.
Removing local downloads or subscriptions is not an applicable workaround in
this prefix.

A repeat with the same simulated runtime excluded only these two IDs in
Home's process memory. The temporary debugger validates the recorded function
prologue, routes matching published IDs to an unavailable-state result, and
executes the original function for all other IDs. This is a diagnostic patch
specific to the recorded DLL, not a supported Home setting or a durable fix.

Run: `.build/steamvr-dxmt/run-mwxr-20261007-073405/`;
trace: `.build/steamvr-home-termination/exclusion-trace.log`.
Home was PID `0x0410`, original TID `0x0414`. The debugger attached at elapsed
43.652 s, sampled the original main thread alive through 155.748 s, and
restored the original function on successful detach after its 120-second
observation period. No stack-overflow exception occurred. The OpenVR shim
logged continuous left/right submissions through this period, with no Submit
errors. Across the complete run, including continued operation after debugger
detach, emitted windows recorded **77,334 submissions per eye**, zero Submit
errors. Some windows exceeded 100 submissions/s; this does not establish
physical presentation rate or pacing quality. Pose logs still reported invalid
HMD poses in this simulated setup. No headset rendering was assessed.

Excluding the cycle restored sustained submission, strongly connecting Home's
failure to its unguarded Workshop dependency traversal. Restoring the function
on detach did not immediately retrigger the failure in this already-initialized
process; this is not a clean-start baseline with the cycle enabled.

All test processes were stopped. SteamVR settings and Valve's OpenVR DLL were
restored; `client.dll` retained the SHA-256 above. No Workshop subscriptions,
downloaded content, or Home binaries were changed persistently. A durable
workaround or upstream cycle fix remains to be implemented.

### Isolated Steam UGC probe, 2026-10-07

`tools/workshop_probe/main.cpp` now retrieves the same items independently of
Home, OpenVR, the Wine XR proxy and Monado. It dynamically loads Home's Steam
API DLL under Wine 11.10, uses AppID 250820 and Home-matching UGC013/Utils009
interfaces, and queries state, details and children. Traversal tracks visited
nodes and the active path, with request/graph limits, so a cycle is reported
rather than recursively overflowing. It does not subscribe or download.
Build and repeat instructions are in `tools/workshop_probe/README.md`.

Uncached queries returned success (`EResult=1`, `cached=0`) in 279 and 362 ms.
Both items returned state flags 0, one child each, and the exact reciprocal
IDs found on Home's stack. Cache-allowed queries returned the same data with
`cached=1`, in 9 and 3 ms. A second fresh run verified the final probe.
All successful probes completed normally and reported the two-node cycle.
Thus neither a stuck Steam call nor a cache-only fabricated edge is needed to
reproduce the dependency graph; Home adds the unbounded recursive traversal.
This does not test file downloads or other Steam client implementations.

Logs: `.build/steamvr-workshop-probe/fresh.log`, `cache-allowed.log` and
`fresh-repeat.log`. No SteamVR processes or simulated runtime were started for
these probes. The Steam client was stopped after testing.

### Native Windows baseline and remaining attribution, 2026-10-07

The user reports Home boots successfully on their Windows XPS13, with PS VR2
tracking and a null display because the laptop lacks the required DisplayPort
connection. This is a useful application baseline; binary hashes and the UGC
probe results on that machine have not yet been compared with Wine.

The user subsequently reports reproducing the crash on native Windows. This
supersedes the assumption that Windows consistently avoids it, and supports a
cross-platform application/content problem. The Windows exception code,
recursive stack and binary hashes have not been supplied here, so an identical
fault mechanism remains to be confirmed from that evidence.

The user attempted to pre-download both resources as a workaround, but the XPS
went to sleep before download completion or Home recovery could be confirmed.
Record this as an attempted, unvalidated workaround, not a successful run.
After waking, run the probe and inspect both `STATE` lines: installed is bit
0x4; needs-update, downloading and download-pending are 0x8, 0x10 and 0x20.
Subscription alone (0x1) does not establish completed installation. Once both
items are installed and current, restart Home and record whether the crash
persists. Download completion is useful evidence, but the published cycle
remains and preinstallation is not yet proven to prevent recursive traversal.

The tests above establish a published cycle, a recursive main-thread overflow
in the Wine setup, and recovery when the two IDs are excluded. Native Windows
probe and build/run instructions are included with the probe.

### Preinstallation experiment under Wine, 2026-10-07

The two resources were downloaded directly through `ISteamUGC::DownloadItem`,
without subscribing. Completion callbacks returned `EResult=1` for both.
`GetItemState` returned **0x4** for each, and `GetItemInstallInfo` confirmed:

- 3149046643: 102,539,125 bytes in `steamapps/workshop/content/250820/3149046643`.
- 2289310332: 509,098,326 bytes in `steamapps/workshop/content/250820/2289310332`.

The prefix initially had no Workshop directory. Steam loaded AppID 250820's
Workshop path as an empty string and attempted staging at `/downloads/250820`,
returning I/O failure (`EResult=37`). Creating the standard
`steamapps/workshop` directory and restarting this prefix's Steam client made
it load the correct Windows path. Requests also need Steam to finish logging
in; requests issued earlier returned false. The temporary download helper
waits for login and success callbacks before accessing installation info.

Home was then tested with these resources installed and **no exclusion**.
An initial launch stopped at OpenXR initialization because the simulated
service was tag 2151 while its x86_64 client had been rebuilt to tag 2152.
That launch is not evidence about Home. Rebuilding the simulated service from
its existing source checkout produced matching client/service tags
`v25.1.0-2152-g3a3afbf2a` for the meaningful repeat; Home/API DLL hashes were
unchanged from the previous probe.

Meaningful run: `.build/steamvr-dxmt/run-mwxr-20261007-101104/`.
Home PID `0x0bcc` (3020), original TID `0x0bd0`, started slowly and initially
waited in SteamVR client calls. It eventually submitted **146 frames per eye**
in emitted log windows, with no Submit errors. Once Workshop metadata was
processed, the main stack again contained repeated `client.dll+0x4f1a1a`.
A retained-stack read found **1,096 recursive frames**, 128 bytes apart, with
the same object pointer and alternating IDs **3149046643 / 2289310332**.
Both preinstalled resources therefore still participate in the recursive cycle.
The process was stopped after capturing this evidence, before a new
stack-overflow exception was observed: do not describe this run as a captured
second overflow. Preinstallation did not eliminate the recursion or establish
sustained healthy rendering. This was simulated-only, without headset validation.

Evidence: `.build/steamvr-preinstall-test/download-final.log`,
`home-trace-matched.log`, `home-trace-rendering.log`,
`preinstalled-stack-memory.log`, `binary-hashes.json`, and temporary helper
sources in that directory. The main thread was observed descending into the
cycle during the second, rendering-phase trace. Diagnostic tools were stopped,
SteamVR settings and Valve's OpenVR DLL were restored. Downloaded resources
are retained in the test prefix; no account subscriptions were added.

### General cycle guard and draft report, 2026-10-07

`tools/workshop_guard` adds a standalone, opt-in Windows x64 debugger helper.
It checks the entire recorded Home DLL hash and entry prologue, then tracks
(manager pointer, published item ID) on each thread's active call path.
Revisiting a pair on the same path returns unavailable state 0 / result 0;
shared acyclic dependencies and other threads are permitted. A depth limit of
128 provides a further bound. The implementation has no item-ID exclusion list.
The helper stays attached; it intercepts function entry and a substituted return
gate. Orderly detach restores the original entry and outstanding return slots.
Home files and Workshop metadata are not edited. This is a debugger workaround
for one verified binary, with runtime overhead and native Windows validation
still outstanding, rather than a production Home replacement.

The real Windows debugger fixture passes a shared dependency graph, a cycle,
a too-deep graph, and 32 completed traversals on four threads: 393 entries,
34 blocked calls, 359 normal returns, no helper failure. An active-call detach
fixture restores the pending return address and completes normally after
detach. The tests exposed and corrected a DLL-registration timing race:
startup probing now reaches a stopped debug event after module registration.
Repeatable fixture commands are in the helper README.

Live Home accepted the verified entry and ordinary Workshop calls returned.
The first run started before Steam finished login and did not reach the cycle.
The online repeat (Home PID 2968) returned 117 ordinary calls and submitted
frames, but did not encounter a cycle rejection during the 600-second guard
period. Fresh and cache-allowed standalone queries for the cycle items each
timed out after 30 seconds, unlike the successful earlier probes. A further
90-second attachment also returned normally and restored/detached. Thus the
live run checks attach/ordinary-call compatibility and restoration; it does
**not** validate rejection of the actual Home cycle. Frame submission was slow
in this simulated setup, and no headset presentation was assessed.

Logs: `.build/steamvr-cycle-guard/guard-online.log`, `guard-final.log`,
`home-online-shim.log`, `metadata-online.log`, `metadata-cached.log`, and
`final-tests/`. The simulated launcher run is
`.build/steamvr-dxmt/run-mwxr-20261007-102458/`. All diagnostic processes were
stopped and the original SteamVR settings / Valve OpenVR DLL restored.

A report for Valve and a separate proposed author message are prepared in
`docs/bug-reports/steamvr-home-workshop-cycle.md`. Neither has been submitted.
The report distinguishes captured Wine evidence from the user-reported Windows
reproduction, and keeps the general guard's live acceptance gate explicit.

### CrossOver guard restart, 2026-10-07

At the user's request, all remaining Wine Windows processes and wineservers
were closed and a process inventory confirmed none remained. Steam, SteamVR
and the guard were then restarted in the existing
`.build/steamvr-crossover/prefix`, using its patched CrossOver 26.3 FOSS Wine
and `WINEMSYNC=1`, with the matching simulated service/client and
`XRT_MACOS_CLIENT_COMPOSITOR=0`. The guard attached to Home PID 1156, verified
the same recorded client DLL hash, and logged `ARMED` before Workshop calls.
Early Home submission windows advanced (356, then 756 frames per eye), with
no Submit errors. No dependency traversal or cycle rejection had occurred at
this checkpoint. Steam's connection log had no successful logon in this run,
and its startup log reported a main-loop stall; cached credentials alone do
not establish a live session. UI inspection was unavailable because macOS was
locked. SteamVR/Home and the guard are intentionally left running for the user.

Logs: `.build/steamvr-cycle-guard/crossover/guard.log`, `launcher.log`, and
`.build/steamvr-crossover/run-mwxr-20261007-110244/openvr-shim.log`.
A remote recheck at 11:08 found Home, SteamVR and the guard still running.
Home had submitted 24,196 frames per eye across emitted windows, without
Submit errors; the latest windows ran about 47–65 frames/s. The guard had
returned all 32 intercepted calls normally, with zero rejections. Steam itself
had exited: `steam.log` ended with `fatal stalled cross-thread pipe` and
`Fatal assert; application exiting` after the main-loop stall. No successful
logon had occurred. Thus this is sustained simulated submission without a live
Steam session, not validation of the published Workshop cycle under CrossOver.

Original settings are backed up as
`.build/steamvr-cycle-guard/crossover/settings-before.json`; the running
launcher restores Valve's OpenVR DLL when it exits.

### Steam-only cold-start isolation, 2026-10-07

Home and SteamVR were stopped before starting Steam alone in the CrossOver
prefix, with the same launcher arguments. Steam again logged
`CSteamEngine::BMainLoop appears to have stalled > 15 seconds without event
signalled`, without completing sign-in. A separate cold startup with
`WINEMSYNC=0` also stalled. A third startup temporarily replaced the custom
`steamwebhelper.exe` wrapper (which adds `--disable-gpu --single-process`)
with its installed original helper; this also stalled. No Home/guard process
was running in these three tests, so they reproduce the startup warning
independently of the Home guard. They do not identify the stalled thread/IPC
endpoint or prove every test would reach the same later fatal assertion.

The earlier long CrossOver guard log also contains msync node-pool warnings.
The local Wine server source falls back to `malloc` when that pool is empty;
the warning alone is not evidence of allocation failure. The msync-disabled
startup did not resolve the Steam hang. The account configuration still has a
cached account and `RememberPassword=1`; this does not establish a valid live
session. Exact underlying startup cause remains unresolved.

Logs are in `.build/steamvr-cycle-guard/crossover-steam-only/`: `steam.log`,
`steam-msync-off.log`, and `steam-original-helper.log`. Failed tests were
stopped, and the original helper wrapper, SteamVR settings and Valve OpenVR DLL
were verified restored. Home/guard were not relaunched because the required
Steam sign-in gate was not met. This supersedes the earlier left-running state.

### Display-image wrapper comparison, 2026-10-07

The user identified the previously working command as
`XRT_MACOS_CLIENT_COMPOSITOR=0 .build/steamvr-crossover/run-mwxr-display-image.zsh`.
That wrapper selects isolated hardware service / virtual-display mode and
inherits the installed service's environment. The earlier guard run selected
simulated service / direct mode with a smaller environment. The underlying
Steam launch command and CrossOver Wine wrapper are identical.

The display-image wrapper was retried with `MONADO_SERVICE_BUILD` overridden
to the simulated-only build and a temporary service template preserving the
installed environment while enabling simulation. This avoids a hardware run
while the user is away, so it does not reproduce the full prior PS VR2 setup.
Home PID 948 loaded the supported DLL and the guard armed; all 32 ordinary
calls returned with zero rejections. Latest simulated submission windows ran
about 120 frames/s, without Submit errors. Steam still logged the same main-loop
startup stall and had not completed sign-in at the checkpoint. The changed
launch path therefore has not resolved the sign-in problem in simulation.
The wrapper/guard are left running. Logs: `.build/steamvr-cycle-guard/crossover-display-wrapper/`
and `.build/steamvr-crossover/run-mwxr-20261007-121237/`.

### Display-image run stopped at user request, 2026-10-07

At the final checkpoint Home was still submitting roughly 64–68 frames/s,
with no Submit errors. Steam had meanwhile exited with the same fatal stalled
cross-thread pipe assertion. The guard's later log contains repeated msync
node-pool warnings; its last ordinary-call status was 32 calls, 32 returns,
zero blocked. An exit record was present after shutdown. Sustained Home
submission and these ordinary calls do not validate a live cycle rejection.

Home/SteamVR were stopped at the user's request. The launcher and guard
sessions ended; process inspection found no Wine/SteamVR/guard or simulated
Monado service remaining. Valve's OpenVR DLL and the original SteamVR settings
were verified restored. The wrapper retry is no longer running. The user will
inspect the Steam issue later; the bug-report draft remains unsubmitted.

### Virtual mode on the PS VR2 with the compositor in the service, 2026-10-07

Two Monado client bugs meant virtual mode had only ever worked with the
compositor in vrserver (`XRT_MACOS_CLIENT_COMPOSITOR=1`):
- Without the hosted compositor, the IPC client never copied the display
  layout, so XR_MNDX_display_distortion was refused ("View 0 is rotated")
  and the driver fell back to direct mode (`3a3afbf2a`).
- Its placeholder distortion then replaced the IPC call with an identity,
  so SteamVR drew with no lens correction. The image was undistorted and
  felt locked to the eyes (`2f93a1e43`).

The driver now logs the display layout and distortion samples at start-up
(`80921a1`). For the PS VR2, the left eye's centre row maps 0.5 to 0.608,
and its corner maps to (−1.7, −1.7). Identity values mean the client is not
asking the service.

The service and client must be built from the same commit. Isolated mode
leaves `IPC_IGNORE_VERSION` unset, so a mismatch shows as "xrCreateInstance
failed: -51".

On the PS VR2 with CrossOver and msync, and the compositor in the service,
virtual mode looks right and tracks the head. Half-Life: Alyx was more
playable in virtual mode than in direct mode at the same commit, though
still CPU-limited at 120 Hz.

### msync leaked wait registrations, 2026-10-07

After long sessions, the CrossOver wineserver printed "msync: warn: node
memory pool exhausted" many times. Under msync, the server keeps a list of
the threads waiting on each object (from a pool of 524,288 entries). Two
paths left entries on those lists:
- The client gave up on a multi-object wait while the server was still
  registering it (an object became signalled), without sending the removal.
- The server's own early exit had an off-by-one (`i > 1`, not `i > 0`), so the
  first object kept its entry.

Entries on objects that are never signalled, such as a worker's shutdown
event, then stay for good. Every unregister walks those lists, so the
wineserver gets slower the longer the prefix runs. Home's high CPU in a Steam
session that had been running for hours, and the drop after a restart, fit
this.

`patches/crossover/0002-msync-remove-abandoned-wait-registrations.patch`
fixes both: the client now sends the removal when it gives up, and the
server's early exit unregisters every object it registered. Rebuild
`server/wineserver` and `dlls/ntdll/ntdll.so` and install both. In
`tools/keyedmutex_test/msync_wait_leak.c`, stock CrossOver logged the warning
12.9 million times in 60 s; the patched build logged none, used a fifth of the
memory, and satisfied 68% more waits. The keyed-mutex test still passes.

### Home's hands and the invisible dashboard, 2026-10-07

Home hides its hands and pointer while SteamVR's dashboard is open. At start-up,
SteamVR opens the dashboard with Steam's interface as its main panel
(`valve.steam.gamepadui.main`, "InitialOverlayToAutoShow"). Home's console then
shows `Received event: 502, Dashboard Showing: 1` (`VREvent_DashboardActivated`)
with no later 503. Under Wine the dashboard never draws: the view dims, and no
panel appears, not even SteamVR's own bar. So the hands stay hidden behind a
dashboard you can't see. The PS button (`/input/system/click`) toggles it,
and macOS also opens Apple Arcade on that button.

Fixed on the way: the headset now reports `Prop_ExpectedControllerType_String`
`oculus_touch`. Before that, Home started in gamepad mode when the controllers
attached late, logging the fallback hundreds of times a second.

Steam's web helper in the CrossOver prefix is a wrapper that adds
`--disable-gpu --single-process`. With Valve's helper restored
(`steamvr-crossover/steamwebhelper-gpu.zsh on`), Steam does not start. ANGLE
cannot create window surfaces: DXMT refuses a swap chain for another process's
window ("cross-process swapchain not supported yet", `d3d11_swapchain.cpp`),
and Chromium's GPU process presents for the browser process. The wrapper is
back (`… off`). Cross-process swap chains would need remote-layer hosting
in DXMT. That would make Steam's windows faster, but it is not shown to be
what keeps the dashboard dark. SteamVR's web helper runs with a GPU process
and its bar is also missing, so overlay textures crossing processes are the
next suspect.

Logging added for this: `home-console.log` in each run (Home's VConsole,
`tools/vconsole_capture`), and the driver's skeleton update results.

### The compositor draws no overlays, 2026-10-08

`tools/openvr_probe/overlay_probe.exe` creates overlays four ways:
- raw pixels (red);
- a shared D3D11 texture (green);
- a BMP file (blue);
- raw pixels placed in the room rather than on the headset (magenta).

In a simulated, virtual-mode CrossOver run, with frames dumped every 900
presents (`driver_mwxr.virtualDisplayDumpEvery`), every call succeeded and
SteamVR reported every overlay visible. No dumped frame contained any of
them. The dashboard counts as open from start-up and dims the scene, but
draws nothing.

So the fault is not only Steam's GPU-less web helper, the shared-texture
handoff, or the head pose:
- Setting `forceSystemLayerUseAppPoses` false removed the dimming (the
  compositor stopped logging "Driver requests system layer use app poses"),
  but overlays still did not appear.
- The room-placed overlay, which needs no HMD pose, was also missing.

Leads still open:
- Even raw and file overlays are probably uploaded by SteamVR's overlay
  manager and shared with the compositor, so a sharing route other than
  games' may arrive empty under DXMT.
- DXMT logged two unknown interface queries: `ID3D11On12Device`, and
  `50c7f8e9-a63f-4035-9801-b96e733347de` on the DXGI factory.
- The compositor logged `VirtualDisplay CopyPixel AcquireSync FAILED`.

The same probe on the Wine 11.10 rig (driver_mwxr, virtual mode) gave the
same result. With SteamVR's null driver on that rig (its desktop window,
watched by eye) every call again succeeded, and the dashboard reported open
after `ShowDashboard`, but nothing appeared in the window. The null driver
uses none of our code, so the fault is in SteamVR's compositor under Wine
and DXMT, not in driver_mwxr, CrossOver or msync.

Steam's web helper also logs `Failed to create VR overlay for browser
valve.steam.gamepadui.mainmenu` (`BOpenVRInitialized` false). That affects
only the dashboard's Steam pages; the probe's overlays fail without it.

DXMT shared-resource logging (DXMT `95b92f5`: `DXMT_LOG_LEVEL=debug`,
`DXMT_LOG_PID=1`), in two simulated runs on the 11.10 rig, both of which fell
back to direct mode:
- No share, import or keyed mutex call failed in any process.
- The compositor creates a texture for each raw and file overlay (64x64,
  with initial data, plus 300x300 and 16x16 ones from the dashboard) and
  takes a shared handle for each, but never imports any of them.
- The probe's client creates an sRGB copy of the green texture and shares
  it, and no process ever opens it.
- At start-up, the compositor's render device does import the textures its
  loader created (the skybox and others) and the driver's six eye images.
  So importing works; overlays are simply never fetched for drawing.
- The compositor creates two shared 1308x1472 "systemlayer" textures that no
  process opens.

So the compositor never gets as far as drawing an overlay, under any
driver. The renderer is not the cause, so the D3DMetal comparison is no
longer needed. The invalid HMD render pose (see "the HMD render pose is
invalid under Wine") remains a candidate; see below.

### Where overlays drop out in vrcompositor, 2026-10-08 (CrossOver)

Simulated, direct mode, probe overlays shown. vrcompositor 2.17.10
(v1789498647), loaded at its preferred base 0x140000000. Breakpoints were
counted with `tools/winedbg_count.py`, which never interrupts the debuggee:
quitting winedbg while attached kills the process, and an earlier attempt
that did so killed the compositor.

- Thread stacks (`winedbg`, `bt all`): all 27 threads are in ordinary waits;
  nothing is stuck.
- `CGraphicsDevice::DrawOverlays` (function 0x140047380) runs every frame,
  but its list of overlays to draw (`m_vecSortedOverlays`, at +0x48 of its
  parameter block) was empty in 300 of 300 calls.
- That list is built in 0x1400def00 from the scene graph's render list (the
  loop at 0x1400e15c0, items of 0x218 bytes from +0x80/+0x88 of a per-frame
  structure). The loop body never ran: the render list is empty. Each item
  would pass through 0x1400e3f00, which can reject it.
- In SteamVR 2.17 overlays, standalone ones included, are drawn as nodes of
  a scene graph that the dashboard web UI (`systemui`, in vrwebhelper)
  sends as `update_scene_graph` mailbox messages to
  `vrcompositor_systemlayer` (systemui.js: `owning_overlay_key:
  VRHTML.VROverlay.ThisOverlayKey()`).
- With a log line temporarily added to systemui.js (since removed), the web
  UI sent many updates, 9 to 17 KB each, all with key `system.systemui`.
  The compositor's handler (0x1400e9df0) was entered once per send (5 of 5),
  found the owning overlay, and found a `scene_graph`. So the scene graph
  arrives and is accepted, and the render list built from it is still
  empty.
- Separately, vrwebhelper logs `Got SetOverlayTexture failure
  (VROverlayError_InvalidTexture)` about 20 times per session, so its own
  pages (the dashboard panels) would have no image even once drawn.

The render pose is invalid in both display modes. `tools/openvr_probe` on
CrossOver, simulated: every `WaitGetPoses` HMD pose was invalid (90 of 90
per second) in direct mode, in seated, standing and raw spaces alike, and
equally in virtual mode (worktree builds `build-sim` and `build-x64`, with
`IPC_IGNORE_VERSION=1` for their two-commit difference). Home looked right
in virtual mode only because it was late on most frames there, and late
frames get client-computed poses, which are valid.

So one cause may explain both problems: the compositor's own HMD pose is
uninitialised under Wine, Home therefore needs the shim's pose fix, and a
scene graph whose panels are placed relative to the head or the dashboard
(re)latch yields no render items.

### Root cause: SteamVR thinks the Windows session is locked, 2026-10-08

Fixed by `patches/crossover/0003-wtsapi32-session-info-ex-and-remote-session.patch`
(Wine's `wtsapi32`). Traced with breakpoints on CrossOver, simulated,
direct mode:

1. The compositor's own pose query (vtable +0x50, in 0x14011542d onwards)
   returns the head as `TrackingResult_Running_OK`, valid.
2. Before it publishes the poses for clients (shared memory, offset
   0xad8c; vrclient's `WaitGetPoses` copies them from there), 0x14011b5b0
   sets the head pose to `TrackingResult_Uninitialized`, not valid, with
   display state 3, when the display is locked
   (`steamvr.allowDisplayLockedMode` is false by default).
3. "Display locked" is a flag set when the compositor's window is created
   and on `WM_WTSSESSION_CHANGE`, from 0x140203470. That asks
   `WTSQuerySessionInformationA` for `WTSIsRemoteSession` and then
   `WTSSessionInfoEx` (`SessionFlags`, `WTS_SESSIONSTATE_UNLOCK`), and
   counts the session as locked when a query fails.
4. Wine implements neither class (`FIXME("Unimplemented class")`), and its
   ANSI wrapper would have converted their results as strings.

In locked mode the compositor also draws no overlays and no dashboard,
which explains the empty scene graph render list above.

The patch adds `WTSINFOEX` and `WTS_SESSIONSTATE_*` to `wtsapi32.h`, and
implements both classes: a local session (`WTSIsRemoteSession` FALSE) that
is active and unlocked. Only `wtsapi32.dll` changes; it is installed in
`steamvr-crossover/wine-crossover` (the original kept as
`wtsapi32.dll.pre-sessioninfoex`). Results, simulated:
- `tools/openvr_probe`, direct mode: 0 invalid head poses per second (90
  before).
- Overlay probe, virtual mode with frame dumps: the red, blue and magenta
  overlays are drawn, and so is SteamVR's dashboard bar. Green (the shared
  D3D11 texture) is head-locked behind the magenta one, so these frames do
  not show whether it works.

Still open: vrwebhelper's `Got SetOverlayTexture failure
(VROverlayError_InvalidTexture)`, which would leave its dashboard pages
blank, and whether `MWXR_OPENVR_SHIM_FIX_POSE` is still needed for Home
(it should not be). The Wine 11.10 rig does not have the fix.

### vrwebhelper's InvalidTexture failures, 2026-10-08 (CrossOver)

vrwebhelper (SteamVR's CEF host for the dashboard pages) hands frames to
SteamVR two ways, chosen per frame by CEF:
- `OnAcceleratedPaint` (0x140008c20): CEF's GPU process shares a BGRA
  texture by NT handle (1860x2048, misc 0x802); vrwebhelper opens it with
  `OpenSharedResource1` and calls `IVROverlay_029::SetOverlayTexture` with
  `TextureType_DirectX`. This works: DXMT imports it, and the probe's
  `--cef` mode reproduces it successfully at the same size and flags,
  through `IVROverlay_028` and `_029` and as `VRApplication_WebHelper`.
- `OnPaint` (0x14000b220, software): vrwebhelper uploads the pixels to an
  OpenGL texture and passes `TextureType_OpenGL`. Every failure comes from
  here: `vrclient_vrwebhelper_main.txt` logs `wglGetCurrentContext()
  returned NULL` once per failure, in bursts of about 115 per second while
  a page animates.

The OpenGL path cannot work in this setup:
- vrclient adds `-forceOnPaint=gpu` to vrwebhelper's command line
  (0x18013d85c; overridable with the string setting `steamvr.forceCEFMode`,
  `cpu` or `gpu`). In `gpu` mode vrwebhelper creates no GL context at all.
- In `cpu` mode it asks SDL2 for a hidden window and a 4.1 core context,
  which failed (`Could not create GL context: Invalid parameter`).
  `tools/openvr_probe/sdl_gl_probe.c` reproduces this. Two causes:
  `winemac.drv` rejects `WGL_CONTEXT_OPENGL_NO_ERROR_ARB` (SDL passes it, 0,
  because Wine advertises the extension); fixed by
  `patches/crossover/0004-winemac-accept-wgl-context-opengl-no-error.patch`,
  installed (original `winemac.so.pre-noerror`). And it requires the
  forward-compatible flag for core contexts; CrossOver's own
  `CX_FWD_COMPAT_GL_CTX=1` adds it. With both, all four probe requests
  succeed (4.1 Metal).
- Even with a context, vrclient then needs `WGL_NV_DX_interop` to give the
  GL texture to the compositor (`Failed to load wglDXOpenDeviceNV!`), which
  Wine on macOS does not provide. In `cpu` mode every page then fails.

So the pages must never fall back to `OnPaint`. Why CEF paints some pages
in software in `gpu` mode is not yet known. A candidate: CEF's GPU process
logs DXMT's `CreateSwapChain: cross-process swapchain not supported yet`
(and ANGLE's `eglCreateWindowSurface failed with error EGL_BAD_ALLOC`),
which may make Chromium drop GPU compositing for some or all browsers.
vrwebhelper also hosts desktop (windowed) pages, `settings_desktop` and
`pairing`, which would need such swapchains.

No record of a vrwebhelper crash was found: no dumps, and the logs SteamVR
keeps (current and previous) end normally.

Confirmed cause of the fallback (DXMT `7d4a400`). The swapchain error now
names the process and window: vrwebhelper's GPU process creates swapchains
for two 800x600 `Chrome_WidgetWin_0` windows of its browser process (the
desktop pages), about 13 s before the first software paint. With
`DXMT_CROSS_PROCESS_SWAPCHAIN=offscreen` (opt-in; such a swapchain presents
to a layer in no window, so the desktop pages stay blank), two 4-minute
simulated runs on CrossOver had 0 failures, against 18 in the same run
without it (and 2 to about 1,200 in earlier sessions). Installed in the
CrossOver rig: d3d11, dxgi and d3d10core from this build, and
`winemetal.so` (original `winemetal.so.pre-crossproc`).

A real fix presents those frames in the other process's window. GDI cannot
do it: `tools/keyedmutex_test/gdi_xproc_test.c` shows another process's
drawing never reaches a window in CrossOver's Wine, and winemac.drv has no
OpenGL for such windows either. So the window's own process must present.

DXMT `b2ac201` and `a7d3be9` do it with `CALayerHost`
(`DXMT_CROSS_PROCESS_SWAPCHAIN=host`): the swapchain renders into a layer
attached to a `CAContext`, sized to the window's client area, and publishes
the context id as the window property `DXMT_REMOTE_LAYER`, signalling
`Local\DXMT_REMOTE_LAYER_<pid>`. A thread started when d3d11.dll loads in
the window's process shows it with a `CALayerHost` and removes it when the
property goes; nothing is done per frame. `CAContext` and `CALayerHost` are
private QuartzCore interfaces, checked at runtime as Chromium does.
`tools/keyedmutex_test/d3d11_xproc_swapchain.c` (a parent's window, a
child's swapchain) showed the child's red, green and blue frames filling the
parent's window, confirmed by eye. This build changes winemetal.dll too.

Steam with its GPU web helper on (`steamwebhelper-gpu.zsh on`, Steam
started by hand: the launcher passes `-cef-disable-gpu`), 2026-10-08: the
first attempt stayed black, because Steam's browser process loads dxgi.dll
but not d3d11.dll, so nothing hosted the layers (`process 300 is not hosting
remote layers`). With the host thread started from either DLL (DXMT
`eb8af5e`), Steam's window draws. Chromium recreated the swapchain six times
during start-up, each time re-hosted cleanly. The CrossOver rig's
`run-mwxr.zsh` also exports the mode, because the launcher starts SteamVR
through `run-in-process-openxr.zsh`, not the wrapper.

Steam's 700x440 sign-in window (`SP DesktopLoginWindow`, frameless) then
stayed black with the GPU web helper on, though it drew with the helper off.
It does have a swapchain, hosted like the others; listing the prefix's
windows (`tools/keyedmutex_test/list_windows.c`) showed why it was hidden.
Under the swapchain's `Chrome_WidgetWin_1` Chromium makes a
`Chrome_RenderWidgetHostHWND` ("Chrome Legacy Window") covering the whole
page, after the swapchain, and Wine's view for it sat above the hosted layer.
Giving the hosting view a high `zPosition` (DXMT `85a8cd5`) fixed it: the
sign-in window draws (user, 2026-10-08). Remote layer sizing follows the
window's client area (DXMT `b58d7f9`), and `DXMT_REMOTE_LAYER_DEBUG=1` logs
where each layer is hosted.

During one control run, vrserver crashed (`Unhandled page fault on execute
access to 00006FFFFF9FFB90`, thread 03f4), not reproduced since. Wine's
crash dialog waited unseen, and SteamVR's safe mode then blocked
`driver_mwxr` (`blocked_by_safe_mode`), shown as "some SteamVR add-ons have
been blocked". The prefix now has `HKCU\Software\Wine\WineDbg`
`ShowCrashDialog=0`, so crash backtraces go to the run's `vrstartup.log`.

### OpenXR games launched from Steam go direct to Monado, 2026-10-08 (CrossOver)

Hyperbolica (app 1256230, Unity 2021.1 with the OpenXR plugin 1.2.8) uses
OpenXR, not OpenVR. Launched from the SteamVR dashboard it came up on the
desktop: its OpenXR loader reads the prefix's ActiveRuntime (wineopenxr, set
by `run-in-process-openxr.zsh`), but Steam had been started without the XR
environment, so the game failed with `XR_ERROR_RUNTIME_UNAVAILABLE`.

Decision: OpenXR games go direct to Monado rather than through SteamVR's
OpenXR runtime (no double composite, no SteamVR prediction). SteamVR's driver
session stays connected behind the game; Monado's multi-client focus makes the
game active and hides the driver session (`shouldRender 0`), and focus returns
to SteamVR when the game exits. Wine clients composite in the service
(`XRT_MACOS_CLIENT_COMPOSITOR=0`, set by the launcher; client compositing made
Wine runs slower).

Launcher changes (`8cff8f7`, `ce8fc59`, `b38fdcd`, `90dcd70`, `8f04a1d`):
Steam starts after the service with the XR environment; a Steam the launcher
did not start (recorded by PID in `prefix/.mwxr-steam-xr-env`) is restarted;
the isolated service uses fixed names; the service path is made absolute.

Steam's VR launch option for Hyperbolica is `Hyperbolica.exe startXR`; a plain
`-applaunch` starts it flat, so use `APP_ARGS=startXR`.

Runs:
- 08:50, simulated, DXMT with share logging: session reached READY, then the
  game hung with two threads in DXMT d3d11 (one called from wineopenxr);
  Steam's crash reporter fired, Steam exited and SteamVR followed.
- 12:22, simulated, DXMT `a7d3be9`, Steam overlay off for the game
  (`OverlayAppEnable 0` in localconfig.vdf): FOCUSED and rendering in the
  headset. Steam took an assert dump of the game 1.6 s after focus (no
  exception; the game carried on).
- 14:02, PS VR2 (isolated `monado-display-distortion/build-hw` at `5f456920d`,
  direct display mode, DXMT `eb8af5e`): plays in the headset, including a
  relaunch. Problem: text on the game's buttons (labels missing, plain rounded
  rectangles; dialogue text is fine). Same in flat mode, with the original
  DXMT and with GPTK's D3DMetal, so not graphics. Cause: no Arial in the
  prefix's C:\windows\Fonts (Wine registers macOS fonts by Z: path only).
  Unity's default UI font is Arial, looked up there; the dialogue uses fonts
  bundled with the game. Fixed by linking macOS's copies of the Windows core
  fonts into the prefix, which the launcher now does. Which of the overlay setting
  and the DXMT update fixed the 08:50 hang is not known.

### Next steps

- Run the general guard against a reproducible live cycle, on Windows or with
  responsive Steam metadata under Wine. Check sustained submits after a logged
  rejection, then assess frame pacing. The debugger fixtures do not replace this
  application acceptance gate.
- PS VR2 run of the CrossOver rig, with the compositor in the service and in
  vrserver.
- Compare the Workshop recursion under CrossOver if it remains after resolving
  the dependency/cache issue.
- PS VR2 run of virtual mode. It needs Monado built from
  `claude/display-distortion-mndx` with
  `-DXRT_FEATURE_OPENXR_MNDX_DISPLAY_DISTORTION=ON`, for both the service and
  the x86_64 client. `MWXR_MONADO=isolated` runs such a service
  (`MONADO_SERVICE_BUILD`) under its own launchd label and socket, with the
  installed LaunchAgent's environment. The agent is left alone, but must not
  be running, because the service claims the headset's USB.
  `MWXR_DISPLAY_MODE=virtual` selects the mode. Checks:
  - the image is correctly distorted;
  - a dump matches the panel;
  - latency and pacing compared with direct mode and the xrizer path.
- PS VR2 check of the zero-copy path.
- Raise SteamVR's resolve resolution towards the runtime's recommendation, if
  SteamVR allows it (its supersampling settings), to reduce the loss in the
  extra composite.
- Measure latency and Game Mode behaviour against the direct
  OpenComposite/xrizer path before choosing defaults per game.

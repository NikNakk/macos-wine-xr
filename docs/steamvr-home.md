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

- Controllers are not drawn in Home, and Home does not react to them, although
  their values reach the driver. This may have the same cause as the missing
  universe; recheck after the `jsonid` fix.
- SteamVR's 41-45 ms prediction on the PS VR2 is long, and comes from the
  vsync timing the driver reports. Worth tuning with real pacing data.
- "WaitForAcquire timed out" appears occasionally during hitches.
- Game Mode: frames now pass through `vrserver`, which macOS may throttle like
  `monado-service`. Not yet measured.
- No skeletal input, battery or proximity.

### Next steps

- PS VR2 check of the zero-copy path.
- Raise SteamVR's resolve resolution towards the runtime's recommendation, if
  SteamVR allows it (its supersampling settings), to reduce the loss in the
  extra composite.
- Simulated controllers, to debug input without hardware.
- Measure latency and Game Mode behaviour against the direct
  OpenComposite/xrizer path before choosing defaults per game.

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

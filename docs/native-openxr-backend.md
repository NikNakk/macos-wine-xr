# Generic native OpenXR backend and reverse sharing

This path loads the **native Khronos loader**, not a runtime library directly.
`MWXR_OPENXR_LOADER` selects that loader; `XR_RUNTIME_JSON` selects its native
runtime. The backend requires `XR_KHR_metal_enable` and uses the runtime's
reported Metal device and its own queue. There is no Monado dependency in this
backend, protocol, host, or Win64 thunk.

## Build and run

Use Khronos OpenXR headers (tested with SDK 1.1.63), current DXMT from
`NikNakk/dxmt:macos-xr-native-sharing`, MinGW, CMake and Ninja:

```sh
OPENXR_INCLUDE_DIR=/path/to/OpenXR-SDK/include \
DXMT_SOURCE_DIR=/path/to/dxmt scripts/build-generic-openxr.zsh

XR_RUNTIME_JSON=/path/to/native/runtime.json \
MWXR_OPENXR_LOADER=/path/to/libopenxr_loader.dylib \
  build-native-openxr/native_openxr_backend_probe
```

The probe reports runtime/system, stereo recommendations, native Metal formats,
image count, IOSurface backing, successful shared-handle reopening and strategy.
`native_openxr_blit_probe` tests the copy and producer-event path.

For the independent sharing test:

```sh
build-native-openxr/dxmt_reverse_import_probe /path/to/wine \
  build-win-openxr/dxmt_reverse_import_probe.exe
```

Add the loader path as the final argument to test actual runtime swapchains.
The proof covers 2D, array-of-two and three images, descriptor rejection, broker
revocation and timeline synchronization. A native compute shader checks the
rendered pattern. Only a four-byte mismatch counter is read by the CPU; no
image data is read back or copied by the CPU.

For RPC without graphics, start the host with `MWXR_RPC_TOKEN` (64 lowercase
hexadecimal characters), then run `openxr_rpc_smoke.exe` with the DLL path and
matching `MWXR_RPC_PORT`/`MWXR_RPC_TOKEN` in Wine.

For an application using the Khronos Windows loader:

```sh
XR_RUNTIME_JSON=/path/to/native/runtime.json \
MWXR_OPENXR_LOADER=/path/to/libopenxr_loader.dylib \
MWXR_WINE=/path/to/current-dxmt/wine \
MWXR_PRIVATE_WINEPREFIX=/path/to/dedicated/prefix \
MWXR_TIMING_TRACE=/tmp/generic-openxr.csv \
  scripts/run-generic-openxr.zsh /path/to/hello_xr.exe \
  --graphics D3D11 --space Local --blendmode Opaque --verbose
```

The runner changes `ActiveRuntime` only in the explicitly supplied private
prefix. Wine's loader may ignore `XR_RUNTIME_JSON` in an elevated context.
The native host receives the native manifest; the Wine process receives the
thin DLL's manifest. Changing native runtimes does not change the DLL.
The runner does not register or replace a Monado LaunchAgent.

## Graphics ownership and lifetime

The runtime creates ordinary `XrSwapchainImageMetalKHR` textures. Each image
has a bridge ID and plain metadata; native pointers never cross RPC.
All images must produce shared handles which reopen with matching device,
format, dimensions, array size, mips and samples before direct sharing is
selected. IOSurface backing is an observation, not a requirement.

Direct images are imported through `IDXMTNativeDevice` and exposed as normal
`ID3D11Texture2D` wrappers. DXMT neither allocates replacement storage nor copies
these images. For an unshareable runtime swapchain, the host creates shareable
application-facing Metal staging images which DXMT wraps in the same way.
After the application's imported D3D11 fence signals, the host issues one blit
command buffer into the acquired native runtime image, then releases it.
Array slices use separate encoder copy calls in that one command buffer.
Thus the copy count is one per image, with all slices copied, and two per frame
when an application submits separate left/right eye swapchains.

Capabilities are random, scoped Mach broker endpoints. They carry a version
and object kind and validate the requesting effective UID. Closing the broker
destroys its receive right and prevents future imports; existing imports retain
their own resources. This avoids trying to revoke raw Metal ports registered
with `bootstrap_register2`, which did not revoke successfully in our test.
The host retains brokers until swapchain/session destruction and releases
staging textures, native swapchains and event rights on disconnect. Per-request
autorelease pools bound transient Foundation/Metal objects during long sessions.
The original DXMT-to-Monado proxy path remains available.

## Protocol and supported subset

`protocol/openxr.json` generates fixed-width, little-endian codecs. The
32-byte packet header includes magic, version, operation, sequence, result,
exact payload length and schema fingerprint. A mismatch closes the connection.
Authentication reuses the nonce/HMAC-SHA256 scheme, using CommonCrypto on
macOS and BCrypt on Windows. The listener binds to loopback. TCP_NODELAY avoids
Nagle delays in the synchronous frame calls. Requests are serialized.

The host owns instances, sessions, reference/action spaces, paths, action
sets/actions, native events, views and frame lifecycle. The thin Win64 runtime
implements official loader negotiation and `xrGetInstanceProcAddr`; it stores
remote IDs and D3D11 wrappers, rather than a Windows compositor/state tracker.

The initial subset supports OpenXR 1.0, `XR_KHR_D3D11_enable`, one instance,
one HMD system, one session per connection, primary stereo, one projection
layer (or zero layers), at most four views, sixteen swapchains and sixteen
images each, single-sample/single-mip 2D or 2D-array color images. Color format
translation includes RGBA8/BGRA8 unorm/sRGB and RGBA16 float where the native
runtime lists it. Depth submission, protected images, cubes, quad layers,
unrecognized `next` chains and other extensions fail explicitly or are omitted
from advertised capabilities. The host currently permits one outstanding
acquire per swapchain. The native swapchain library can queue acquisitions.

Action setup, suggested bindings, attachment/synchronization, Boolean/float/
vector/pose state, action spaces, current profile and vibration are marshaled.
`hello_xr` exercises creation and polling; controller interaction/haptics have
not been validated on hardware. This is an experimental subset, not OpenXR
conformance or full game compatibility.

## Evidence: 2026-10-01, Apple M5

Starting source heads:

- bridge `e1bcb3c92ebda7209b1053a44a86efa527c6ca69`;
- DXMT `a0082244fc3596e92fc81d6a06e0f65ad13d1ee3`;
- Monado `f1a6b7f98ba3cf33b73e0f3b969ab2db969607d5`.

The tested bridge/DXMT additions are the commits introducing this document.
The native Monado runtime was built from the starting head with the simulated
HMD enabled and hardware drivers disabled. No PS VR2 hardware run was performed.
Meta XR Simulator was 207.0.0, reporting Meta Quest 3.

| Test | Simulated Monado | Meta XR Simulator |
|---|---|---|
| Native stereo recommendations | 896x1007 | 1680x1760 |
| Representative images | 3, array 2 | 3, array 2 |
| IOSurface / reopened shared handle | 0 / 1 | 0 / 0 |
| Selected path | shared-metal-zero-copy | gpu-blit |
| Native blit and DXMT GPU pattern proofs | passed 2D/array/3 images | passed 2D/array/3 images |
| Win64 RPC properties/system/views | passed | passed |
| Same Win64 DLL, Khronos hello_xr, opaque | exit 0, 677 frames | exit 0, 759 frames |

Both `hello_xr` runs used Metal validation. The traced Monado path submitted
no bridge blits. Meta copied each 1680x1760 eye image: GPU copy median 0.117 ms,
p95 0.232 ms; producer-event wait median 0.526 ms, p95 1.187 ms. Median native
end-frame intervals were 16.632 ms (simulated Monado) and 13.857 ms (Meta).
These short simulator runs include transport, scheduling and runtime behavior;
they are not headset latency or a pacing comparison with the transitional path.

Meta's original default alpha-blend run submitted 30 frames, then its passthrough
code asserted that an IOSurface texture must use shared/managed storage. The
opaque run above passed. The bridge does not suppress validation or special-case
the runtime name. Use opaque composition for the verified validation-enabled
regression command.

### Alpha-blend isolation, 2026-10-02

The user first confirmed that native `psvr2-openxr-test` ran against Meta XR
Simulator with `--blendmode AlphaBlend`, without `--passthrough`, and did not
crash. That run's duration and validation settings were not recorded. Controlled
repeats on Apple M5 / Meta XR Simulator 207.0.0 isolated Metal validation:

| Client / mode | Metal validation | Result |
|---|---|---|
| Native diagnostic / Opaque | `MTL_DEBUG_LAYER=1` | Survived 6 seconds; SIGINT, exit 0 |
| Native diagnostic / AlphaBlend | `MTL_DEBUG_LAYER=1` | Aborted with the same IOSurface storage-mode assertion |
| Native diagnostic / AlphaBlend | `MTL_DEBUG_LAYER` unset | Survived 8 seconds; SIGINT, exit 0 |
| Bridged hello_xr / AlphaBlend, corrected event check | `MTL_DEBUG_LAYER` unset | 348 submitted frames; exit 0 |

Native source: Monado `7fd7f2835693d447d46da933e9a54c9f71ddfae9` plus local
blend-mode selection changes. Bridge source: `df2106148cfff8b13d05773bbff3e2b359d909d2`
plus the shared-event check correction below. No headset hardware was used.

Both clients use two separate 1680x1760 swapchains, one image layer per eye,
one sample/mip/face, array size 1, BGRA8 sRGB (Metal 81; DXGI 91), LOCAL space,
and one stereo projection layer. The native app requests COLOR_ATTACHMENT
usage and premultiplied source alpha. hello_xr additionally requests SAMPLED
usage and UNPREMULTIPLIED_ALPHA. The thunk/host preserve those usage flags,
layer flags, blend mode, poses, FOVs, rectangles, and array indices. Those
client differences are not required to reproduce the validation assertion:
the native client fails without Wine, DXMT, RPC, shared imports, or bridge blits.
This establishes a native Meta reproduction; it does not identify the exact
internal texture descriptor field. LLDB could not launch the app under the
host's debugserver permissions, so no native backtrace was obtained.

A separate bridge failure appeared without validation: the blit guard rejected
`event.device == nil`. Apple's SDK documents that MTLSharedEvent's device may
be nil because shared events span devices. Removing that invalid device check
preserves texture device/descriptor checks and the producer timeline wait.
Before correction, bridged alpha blending failed at xrReleaseSwapchainImage
with XR_ERROR_FEATURE_UNSUPPORTED; afterward it passed the 348-frame run.
The existing three-image, two-slice GPU pattern probe and unsignaled-event timeout
checks passed on both Meta and simulated Monado without validation. The four
bridge CTest suites also passed with macOS bootstrap access.

Evidence logs are in the local `.build/reverse-sharing/` directory of the
Monado workspace: `native-meta-{Opaque,AlphaBlend}-validation.log`,
`native-meta-AlphaBlend-no-validation.log`,
`meta-alpha-compare-no-validation-{host,wine}.log`, and
`{meta,monado}-blit-no-validation.log`.

To reproduce natively, use the diagnostic built from the source above:

```sh
MTL_DEBUG_LAYER=1 \
XR_RUNTIME_JSON=/Applications/MetaXRSimulator.app/Contents/Resources/MetaXRSimulator/meta_openxr_simulator.json \
PSVR2_OPENXR_LOADER=/path/to/libopenxr_loader.dylib \
  /path/to/psvr2-openxr-test --blendmode AlphaBlend
```

Opaque remains the validation-enabled regression baseline. Disabling validation
allows the short alpha runs to complete but does not repair Meta's invalid
IOSurface texture descriptor. The next runtime-side step is to inspect/report
that native reproduction to Meta; no bridge workaround is justified by these
results.


### Wine 11.10 and Underture, 2026-10-02

The current-DXMT builder now runs applications with **Wine 11.10**. The existing
Wine 8.16 SDK is retained only for linking DXMT. Set
`MACOS_WINE_XR_WINE11_SOURCE` to an installed Wine 11.10 tree containing
`bin/wine`; the builder clones it privately and installs current DXMT there.
Source the generated `env.zsh` before using `run-generic-openxr.zsh`. The
launcher respects that private prefix and changes to the executable's directory.

The tested local stack is `.build/wine11-current-dxmt` in the Monado workspace.
Its prefix is an APFS copy of the previously working `build-wine-dxmt/prefix`;
its engine is copied from the older Wine 11.10 installation. Original engines,
prefixes and game files were preserved. A fresh minimal prefix is not covered
by this game validation.

Current DXMT also needed desktop presentation support for stock Wine 11.10,
which hides the macdrv Metal-view exports. The companion DXMT fork now matches
the requested HWND to Wine's `WineWindow` on the AppKit thread and attaches a
Metal view to that exact window. It retains the existing exported-hook path.
The two-window desktop probe passed create, present, resize and destruction
with the foreground window deliberately different from the first swapchain.
This proves more than the earlier XR-only test, which did not create a desktop
DXGI swapchain.

Underture uses Unity 2018.3.1f1, bundled Mono and OpenComposite
`a27e7e6a64bdcd1eff6b7fba1ea2ea34bcf1273d` (1.0.1539). Wine 8.16 repeatedly
faulted during MonoManager reload, including with OpenVR disabled. Wine 11.10
completes reload and initializes the game's OpenVR rendering. Meta then
rejected its legacy inverted texture bounds: the submitted OpenXR rectangle
was `0,1760 1680x-1760`, yielding `XR_ERROR_LAYER_INVALID`. This is separate
from Meta's native alpha-blend Metal-validation assertion.

The private copy at `.build/wine11-current-dxmt/games/Underture` has:

```ini
initUsingVulkan=false
invertUsingShaders=true
```

OpenComposite's shader inversion preserves the flip while submitting positive
OpenXR rectangle extents. This copy submitted **562 frames** to Meta without
`xrEndFrame` rejection during a bounded run. The bridge still uses its single
Metal blit for Meta; the shader inversion is an additional OpenComposite GPU
pass. No CPU pixel transfer was introduced. Native host error logging records
the first two rejected frames' rectangles, pose, FOV and blend mode per client.

With the same Wine 11.10/current-DXMT stack, opaque `hello_xr` passed with Metal
validation enabled: **328 frames** against simulated Monado using shared Metal
images, and **520 frames** against Meta using the Metal blit path. The public
launcher was checked separately. The source build/provision script completed.

Evidence is local in `.build/reverse-sharing/`: `wine11-window-probe.log`,
`wine11-{monado,meta}-hello-{run,host,wine}.log`, and
`underture-wine11-shader-flip-{run,host,wine,player}.log`. Game runs were bounded
and terminated by the harness. Accepted frame submission does not establish
physical headset output, controller/gameplay compatibility or pacing. PS VR2
audio routing from the older launcher has not been ported or validated; Unity
still logs audio/codec warnings. The user confirmed visible cubes from `hello_xr` in Meta, but Underture shows
its normal desktop menu/game view while Meta remains blank. Accepted frame
counts therefore do not establish working VR rendering for Underture.

Native capability/auth/serialization/framing tests pass. Native and MinGW
builds pass, including the legacy metadata probe and transitional proxy.
Clean Monado service plus shared-memory/socket-security/thread-shutdown tests
pass after removing dormant references to deleted channel framing fields.
The authoritative transitional protocol retains all 136 command IDs and all
18 old schemas, with only the generic external semaphore command appended.
Old local September build headers are incompatible and must not be used.
The old shared-fence metadata runtime probe cannot complete on this Wine
8.16 build (`D3D11Fence: Invalid device handle`); the new imported native event
route passes. No claim is made that the transitional headset runner or physical
controller/pacing regressions passed in this session.

### Underture black-eye investigation, 2026-10-02

A missing core output chain was found in the generic Win64 runtime:
OpenComposite requests `XrSpaceVelocity` through `xrLocateSpace`, which the
thunk incorrectly rejected with `XR_ERROR_FEATURE_UNSUPPORTED`. The thunk and
native host now forward the velocity flags and both vectors. RPC protocol
version 2 requires rebuilding the host and DLL together; the handshake rejects
older layouts. Other unsupported output chains remain explicit errors.

The optional `openxr_rpc_smoke.exe <runtime.dll> --space-velocity` regression
passed with Meta (location flags 15, velocity flags 3) and simulated Monado
(location flags 7, velocity flags 0). It checks valid pose output, finite valid
velocities, preserved chain pointers, rejection of an additional unsupported
chain, and the unchanged unchained call. Zero velocity-valid flags from Monado
are accepted. The Monado probe itself exited successfully, but its native
Metal-validation teardown asserted while a command buffer retained a Metal
object; this is not recorded as a clean validation shutdown.

The pose fix removes OpenComposite's unsupported head-pose error, but does
**not** fix Underture's black VR output. Temporary diagnostics sampled a 4x4
grid in Unity's original D3D11 eye textures before OpenComposite, as well as
native staging images after producer completion: all sampled RGBA values were
zero. The source textures are 1680x1760, single-sample,
`DXGI_FORMAT_R8G8B8A8_TYPELESS`. The same source result occurs against simulated
Monado, so the remaining issue is not isolated to Meta's Metal blit path.
OpenVR reports a valid connected headset, `TrackingResult_Running_OK`, and
`CanRenderScene=true`; raw FOVs, projection matrices and eye-to-head transforms
are finite and consistent with the runtime's stereo views.

The shared-image GPU regression now clears a typeless local D3D11 source and
copies it into each imported image/slice. Destinations start white so a missing
copy cannot pass. Native GPU verification found zero mismatches for all three
images in both 2D and two-slice array cases. This proves this copy path for a
known pattern, not Unity's full rendering path.

Changing shader inversion, forcing OpenVR, disabling MSAA, and requesting
Unity's direct graphics mode did not change the sampled black source images.
Temporary sampling DLLs and altered game assets/configuration were restored;
no CPU readback was added to the production transport. Logs are local under
`.build/reverse-sharing/`, including `underture-source-after-velocity-fix.log`,
`underture-monado-source.log`, `underture-projection-msvc-source.log`,
`velocity-{meta,monado}-smoke-{run,host,wine}.log`, and
`wine11-typeless-copy-probe.log`.

The comparison recovered the original Win64 client build tag
`e5358184e0afc2b9a2ce089ba442cf896d3b12c3`. A native service rebuilt from that
exact commit, with PS VR2 disabled, matches all 110 original IPC command IDs
and the shared-memory layout. The installed clean service cannot be paired
with that original DLL: its command IDs differ. The private comparison uses
its own socket directory, TCP port, copied Wine prefix and the preserved Wine
11.10/DXMT v0.80 stack; it does not replace the installed LaunchAgent.

Against simulated Monado, that original stack initialized OpenVR and imported
both IOSurface eye swapchains, but the source eye samples remained black.
Those diagnostic runs disabled GPU-only semaphore synchronization, and one
terminated stream logged an invalid command at shutdown. They do not establish
a working old-stack baseline, or contradict the user's earlier PS VR2 result.
The typeless sampler positive control read the expected `255,128,0,255` from
a GPU-cleared source, and the new bridge's actual startup FOV/eye-transform
queries were valid. Meta eventually reached `XR_SESSION_STATE_FOCUSED` and
OpenVR released input focus; eye samples stayed black. Meta also intermittently
crashed during OpenComposite's bootstrap-session replacement, a separate
remaining stability issue.

A local headset comparison build and launcher are prepared at
`.build/legacy-underture-compare/run-underture.zsh` in the Monado workspace.
It uses a separate game/prefix copy and the matching old client/service, with
camera streams disabled. The build is checked, but physical PS VR2 execution
is left to the user. Evidence includes
`underture-original-full-submit-{run,source}.log`,
`underture-sampler-control-source.log`, `underture-focus-{source,host}.log`, and
`underture-startup-source.log` under `.build/reverse-sharing/`.

## Timing trace

`MWXR_TIMING_TRACE` enables native host CSV records. Times ending in `_ns` use
native `CLOCK_MONOTONIC`; GPU start/end use Metal's seconds-based clock.

```
frame,frame_number,end_ns,interval_ns,predicted_display_time,view_count
release,swapchain_id,image_index,fence_value,fence_wait_ns,copy_gpu_ns,release_ns,result,copy_submit_ns,copy_complete_ns,gpu_start_seconds,gpu_end_seconds
```

Direct sharing reports zero copy fields. Fence wait remains measured separately.
Do not compare native predicted OpenXR time directly to Wine QPC ticks.

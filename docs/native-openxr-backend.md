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

Meta's default alpha-blend run submitted 30 frames, then its passthrough code
asserted that an IOSurface texture must use shared/managed storage. The opaque
run above passed. The bridge does not suppress validation or special-case the
runtime name. Use opaque composition for the verified regression command;
alpha-blend passthrough needs investigation in Meta's runtime.

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

## Timing trace

`MWXR_TIMING_TRACE` enables native host CSV records. Times ending in `_ns` use
native `CLOCK_MONOTONIC`; GPU start/end use Metal's seconds-based clock.

```
frame,frame_number,end_ns,interval_ns,predicted_display_time,view_count
release,swapchain_id,image_index,fence_value,fence_wait_ns,copy_gpu_ns,release_ns,result,copy_submit_ns,copy_complete_ns,gpu_start_seconds,gpu_end_seconds
```

Direct sharing reports zero copy fields. Fence wait remains measured separately.
Do not compare native predicted OpenXR time directly to Wine QPC ticks.

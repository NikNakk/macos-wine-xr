# DXMT Metal foveation prototype

2026-11-08: the target-specific D3D11 OpenXR stereo proof now passes. This is
not yet an EXD integration or a real-eye-tracking validation of the Wine path.

## Target-specific OpenXR integration

The [second DXMT patch](../patches/dxmt/0002-target-specific-metal-rate-map.patch)
adds `IDXMTNativeContext2::SetRenderTargetRasterizationRateMap(texture, map)`.
Bindings belong to the resource and change in command-stream order; each
render pass snapshots its own map. Binding another eye or switching to an
unrelated RT does not change the first eye's map. Device, resource layout and
map logical dimensions are validated before accepting a binding. The initial
contract is one non-array, single-sample/mip colour target without depth.

wineopenxr now forwards the standard FB profile/configuration/update APIs
and META eye-tracked profile structures. The experimental Windows companion
`XR_MWXR_foveation_d3d11` supplies `xrApplyFoveationD3D11MWXR`. An application
must request it alongside `XR_FB_foveation` and
`XR_FB_swapchain_update_state`; FB foveation without this rendering companion
is rejected explicitly. Level profiles additionally use
`XR_FB_foveation_configuration`, and tracked profiles use
`XR_META_foveation_eye_tracked`. The companion is advertised only when the
native Metal transport and base FB APIs are present. Session creation rejects
graphics backends without the optional DXMT interface.

For each eye, create a foveation-capable swapchain, enumerate its images,
update the FB profile, acquire/wait the image, then apply its map **once before
recording draws**. Both eyes may be acquired before either is drawn. The apply
call obtains Monado's existing `xrGetFoveationMetalStateMNDX` state and binds
the map to that particular imported image. No raw Metal pointer is exposed by
the public companion function. Image release submits the producer work and
removes the DXMT target binding; Monado retains its own selection for the
released image's compositor reconstruction. Release before apply and duplicate
apply are rejected. Failed apply blocks release until a successful retry.

The private stereo proof passed two complete instance/session lifecycles:
900 pixel checks across eye images and unrelated control targets, including
changing profile offsets, disable/re-enable and image reuse. Compositor traces
record `foveated_views=2`. Tests also cover extension gating, map-size mismatch,
missing-eye-device rejection without changing the fixed profile, and ordinary
unfoveated 2D/array swapchains. The DXMT backend compiles with old headers too.

The simulator has no eye device. Profile changes exercise moving maps, but
do **not** validate real gaze sampling, tracking loss, final display pixels,
or performance. The META route reuses Monado's existing implementation; an
attended hardware check remains necessary. No installed runtime, EXD files or
LaunchAgent settings were changed.

See [the second-stage results and reproduction](results/dxmt-foveation-openxr-20261108/README.md).

## Implemented

The [private DXMT patch](../patches/dxmt/0001-immediate-context-metal-rate-map.patch)
adds `IDXMTNativeContext1::SetRasterizationRateMap(UINT64)` to the immediate
D3D11 context. It accepts a borrowed native Metal rate map from the same
device, retains it synchronously, and queues the change in command order.
Zero disables it. Invalid live objects are rejected without changing the
previous binding. This first interface accepts single-layer maps only.

The winemetal render-pass ABI now carries the map to
`MTLRenderPassDescriptor.rasterizationRateMap`. Each DXMT render encoder holds
its own retained snapshot, including when a newer map is bound before the
older pass is encoded. Passes with different maps cannot merge. With no map,
ordinary passes do not issue an extra native retain call. Existing COM
interfaces and Unix-call indices remain unchanged; the new Unix operation
is appended at index 155. The PE and Unix components must be rebuilt together.

This is an explicit rendering interface. The caller must supply compatible
attachments and logical viewports/scissors, and arrange reconstruction before
ordinary texture sampling. It does not infer scene targets or rewrite shaders.
Deferred command lists, layered rendering, MSAA, depth and postprocessing have
not been validated with this prototype.

## Pixel proof

`tests/windows/dxmt_foveation_probe.cpp` renders a D3D11 interpolated UV
gradient into native Metal textures imported through DXMT. A native helper
constructs two different rate maps and computes expected sample values using
Metal's screen/physical coordinate conversions.

All four draws/map changes are recorded before the first `Signal` (which
flushes in DXMT). The caller releases its map references before drawing, so
the queued commands and encoder snapshots must keep the maps alive. The
test also attempts to bind a texture as a map and checks `E_INVALIDARG`.

| Case | Logical dimensions | Rasterized dimensions | Pixel checks |
| --- | --- | --- | --- |
| Foveal region towards left | 512×512 | 288×288 | 25 passed |
| Foveal region towards right | 512×512 | 272×288 | 25 passed |
| Map disabled | 512×512 | 512×512 | 25 passed |
| Map enabled again | 512×512 | 288×288 | 25 passed |

All 100 checks pass with Metal API validation enabled on the M5. The negative
control deliberately omits rate-map binding while expecting compacted output:
it fails on its first pixel, as intended. These are correctness checks, not
performance measurements. Textures retain full logical allocations for this
proof; the smaller dimensions describe the active rasterized region.

Logs, source hashes and the build revision are in
[results/dxmt-foveation-20261108](results/dxmt-foveation-20261108/README.md).

## Reproduce

Base: the existing clean `~/Code/dxmt-d3d10fix` revision
`4c3a2f5ea661bd258c14cf8ccb6924791cc75a77`. Work in a private source copy;
leave the installed DXMT/Wine runtimes untouched. Copy the populated
`include/native/directx` and `external/nvapi` submodules too. Apply the patch
from the source copy with `patch -p1 < /absolute/path/to/the/patch`, applying
`0001` and then `0002`.

Configure its own build:

```sh
meson setup build-in-process/dxmt-foveation-build build-in-process/dxmt-foveation-source \
  --cross-file build-in-process/dxmt-foveation-source/build-win64.txt \
  -Dnative_llvm_path="$HOME/Code/dxmt-toolchains/llvm-darwin" \
  -Dwine_install_path="$HOME/Code/wine-crossover/runtime" \
  -Denable_tests=true --buildtype=release
ninja -C build-in-process/dxmt-foveation-build -j 4
```

This machine currently lacks Xcode's downloadable Metal compiler component.
For the recorded build, unchanged `.air`/`.metallib` helpers were reused from
the existing base build, while C/C++/Objective-C code was compiled afresh.
If the component is still absent, run this after Meson setup and before Ninja:

```sh
python3 tools/reuse_dxmt_shader_cache.py \
  --source build-in-process/dxmt-foveation-source \
  --build build-in-process/dxmt-foveation-build \
  --cache-source "$HOME/Code/dxmt-d3d10fix" \
  --cache-build "$HOME/Code/dxmt-d3d10fix/build"
```

Clone a Wine runtime to `build-in-process/wine-foveation`, then overlay the
built `d3d11.dll`, `d3d10core.dll`, `dxgi.dll`, `winemetal.dll` into its
`lib/wine/x86_64-windows` and `winemetal.so` into `lib/wine/x86_64-unix`.
Do not overlay the installed runtime or an active prefix. Build/run the probe:

```sh
DXMT_SOURCE_DIR="$PWD/build-in-process/dxmt-foveation-source" \
MWXR_WINE_SDK="$HOME/Code/wine-crossover/runtime" \
MWXR_WINE_SOURCE="$HOME/Code/wine-crossover/sources/wine" \
  scripts/build-dxmt-foveation-probe.zsh
MWXR_IN_PROCESS_WINE="$PWD/build-in-process/wine-foveation" \
  scripts/run-dxmt-foveation-probe.zsh
MWXR_IN_PROCESS_WINE="$PWD/build-in-process/wine-foveation" \
  scripts/run-dxmt-foveation-probe.zsh --targeted
# Expected failure: the verifier must detect missing rate-map rendering.
MWXR_IN_PROCESS_WINE="$PWD/build-in-process/wine-foveation" \
  scripts/run-dxmt-foveation-probe.zsh --negative-control
```

## Remaining integration work

1. Validate the new Wine path on an attended headset with real gaze and
   tracking loss, retaining the existing Monado gaze/policy implementation.
2. Capture EXD's actual scene colour/depth, resolve and postprocessing passes.
   Choose where the compacted representation can remain in use and where
   reconstruction is required. Resolve screen-space texture reads and
   position semantics before enabling it in gameplay.
3. Measure GPU execution time and image quality at physical-headset render
   dimensions. The simulator's 60 fps cap cannot establish the speedup.

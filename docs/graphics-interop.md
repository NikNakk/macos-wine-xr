# Renderer-neutral graphics interop: DXMT and D3DMetal

Date: 2026-10-03. Branch `claude/graphics-interop-backends`, based on the
in-process Wine OpenXR work (`codex/in-process-wine-openxr`). The proxy and
native-host paths are unchanged fallbacks. Monado and DXMT are unchanged.

## Layering

```text
Windows OpenXR app (D3D11)
  -> wineopenxr PE half: graphics.cpp (OpenXR semantics only)
       -> GraphicsInterop (src/in_process/graphics_interop.h)
            -> DxmtInterop      graphics_interop_dxmt.cpp
            -> D3DMetalInterop  graphics_interop_d3dmetal.cpp
       -> unix call: mw_graphics_native_call (graphics_interop_native.m, Metal only)
  -> wineopenxr native half: graphics_native.m (Khronos Metal binding, backend-neutral)
  -> native Khronos loader -> Monado / Meta / any XR_KHR_metal_enable runtime
```

The native runtime sees only `XR_KHR_metal_enable`: an `MTLCommandQueue` in
`XrGraphicsBindingMetalKHR`, ordinary Metal swapchains and
`xrReleaseSwapchainImage`. Nothing runtime-specific or backend-specific
crosses into it. The Windows app sees ordinary `XrSwapchainImageD3D11KHR`
textures.

## Phase 1: the DXMT path as found

Before this branch, `src/in_process/graphics.cpp` called DXMT directly:

| Step | Where | What happens |
| --- | --- | --- |
| Requirements | `xrGetD3D11GraphicsRequirementsKHR` | Native Metal requirements give the runtime's `MTLDevice`; the DXGI adapter 0 LUID is returned. Generic DXGI, not DXMT-specific. |
| Session | `xrCreateSession` (PE) | `QueryInterface(IDXMTNativeDevice2)`, `GetMetalDevice`. The native half checks `registryID` against the runtime device, creates an `MTLCommandQueue` and `MTLSharedEvent`, and binds the queue. |
| Event import | PE, after native create | `ImportMetalSharedEvent(event) -> ID3D11Fence`. The fence *is* the session event. |
| Swapchain | native `wine_xrCreateSwapchain` | DXGI→Metal format map, shape/usage subset, native create. Runtime images are Metal-owned (Monado: `newSharedTextureWithDescriptor:`, Private storage) or IOSurface-backed (Monado's service path, Shared storage). |
| Images | `xrEnumerateSwapchainImages` (PE) | Enumerate `XrSwapchainImageMetalKHR`, then `ImportMetalTexture(MTLTexture, desc) -> ID3D11Texture2D`. DXMT retains the exact object; no handle, reopen, replacement storage or blit. |
| Acquire/wait | generated thunks | Pass-through; the runtime owns the gate. |
| Release | PE then native | `ID3D11DeviceContext4::Signal(fence, ++value)` and `Flush`. The native half encodes `encodeWaitForEvent:value:` on the bound queue, then calls native release. Monado commits an empty command buffer on that queue and waits for it, so producer writes are ordered before the compositor. |
| Process boundary | none | Direct objects in one process. The broker-name/XPC handle passing exists only in the proxy/native-host paths. |
| Zero copy | `zero-copy image=` log | Every runtime image, or a loud failure. |

Only the session device, event import, image import and release signal were
DXMT-specific. Those four operations are the interface.

## The interface

`GraphicsInterop` (`src/in_process/graphics_interop.h`):

- `capabilities()`: `MW_GFX_CAP_*` flags for D3D11, D3D12, Metal-texture
  import, IOSurface import/export, shared texture, shared event, keyed
  mutex, zero-copy import/export and copy fallback.
- `metal_device()`: the borrowed `MTLDevice` behind the D3D device.
- `extra_swapchain_usage()`: OpenXR usage flags the backend needs on native
  images (D3DMetal: `MUTABLE_FORMAT`). The app's swapchain info is unchanged.
- `bind_completion_event(MTLSharedEvent)` and `sync()`:
  `SharedEventGpu` or `FenceCpuWait`.
- `import_image(MTLTexture, D3D11_TEXTURE2D_DESC, GraphicsImage*)` returns the
  app-visible texture, a `zero_copy` flag, IOSurface backing, and, only for an
  explicit copy fallback, a native `copy_source`.
- `signal_completion(value)`: order and submit all recorded D3D work.
  Only `FenceCpuWait` may block.
- `release_image`, `flush`.

Selection is `MWXR_GRAPHICS_BACKEND`: `auto` (default), `dxmt`, `d3dmetal`, or
`d3dmetal-copy`. `auto` tries DXMT's private interface, then D3DMetal
identification. Only `d3dmetal-copy` can produce a copy.

Each session logs one line with the backend and sync mechanism. Each swapchain
logs one summary line: backend, API, size, format, `native=MTLTexture`,
IOSurface count, sync, `zero-copy=yes|no`, and `fallback=explicit-gpu-blit`
when applicable. Nothing is logged per frame.

**DXMT** implements the interface with exactly the previous calls. Caps:
D3D11, Metal-texture import, IOSurface import, shared event and zero-copy
import. DXMT's broker-name export serves the transitional host path and is
not offered here.

## Phase 2: D3DMetal findings

### Test runtime

Initially no Game Porting Toolkit was installed. The user then supplied
`Game_Porting_Toolkit_4.0_beta_2.dmg`. Its nested "Evaluation environment for
Windows games 4.0 beta 2" image holds `redist/lib`:

- `external/D3DMetal.framework` 4.0b2 and `external/libd3dshared.dylib`, both
  x86_64.
- PE `d3d10/11/12`, `dxgi`, `nvapi64` and `nvngx-on-metalfx` DLLs.
- Matching unix `.so` links to `libd3dshared.dylib`.

That tree was extracted outside the repository. No Apple binary is vendored.

The Wine is CrossOver 26.3.0 (based on Wine 11.0), built from CodeWeavers'
FOSS tarball (`crossover-sources-26.3.0.tar.gz`, SHA-256
`ac99c8ca4b3848f3e81784135f023df266b61c2345726ea55a50b3e030dd6872`). It is
x86_64 only, with the optional libraries disabled; see "Building the GPTK
runtime". `scripts/install-gptk-runtime.zsh` then applies the GPTK files to a
copy of that install.

### How D3DMetal is loaded under Wine

- [Silo](https://github.com/mikaelhug/Silo) (LGPL-2.1+): GPTK's PE
  `d3d11.dll`/`dxgi.dll`/`d3d12.dll` go into the Wine runtime's
  `lib/wine/x86_64-windows`. Their unix `.so` files are symlinks to
  `lib/external/libd3dshared.dylib`, which loads
  `lib/external/D3DMetal.framework`. The overlay must be in the runtime's own
  tree; `WINEDLLPATH` alone keeps wined3d. Silo states D3DMetal is validated
  against CrossOver-patched Wine and builds that Wine from CodeWeavers'
  published source.
- CrossOver Wine (LGPL, mirror `PhoenicisOrg/winecx`): `winemac.drv/d3dmetal.c`
  exports macdrv hooks D3DMetal calls (window data, Metal views, registry and
  user32 functions through `WINAPI` pointers). `ntdll/loader.c` exports
  `__wine_unix_call` for "D3DMetal PE DLLs". `setupapi` matches GPU LUIDs with
  D3DMetal's DXGI.
- [d3dmetal-native](https://github.com/utmapp/d3dmetal-native) (MIT): hosts
  the framework without Wine through its `GFXT` host interface. D3DMetal is
  x86_64-only. Its COM vtables use the Windows ABI. The bundled v3.0 **stubs
  out every resource and fence sharing entry point**: `CreateSharedHandle`
  "fakes with passthrough", `OpenSharedHandle` returns `E_NOTIMPL`, and the
  `OpenSharedResource` family does nothing. d3dmetal-native re-implements
  sharing by swizzling `MTLDevice`/`MTLHeap` allocation selectors and
  substituting its own allocation while a thread is armed. It also documents
  two hazards. D3DMetal's create-time zero-fill of Shared-storage textures goes
  through `-[texture heap]`. GPTK 4.0's `D3DMDevice::UseInternalHeaps`
  suballocates from pools. Its D3D11 cross-process fences publish completion
  from the CPU.

Consequences for this bridge, confirmed with GPTK 4.0b2:

1. In CrossOver 26.3 + GPTK 4.0b2 the app's `ID3D11Device*` is a **PE object**
   (`d3dmetal device PE object, framework loaded`), not a native D3DMetal
   vtable. Identification by loaded framework is therefore required.
2. D3DMetal's Metal allocations for `CreateTexture2D` and `CreateFence` **run
   on the calling thread** and go through `-[MTLDevice
   newTextureWithDescriptor:]` and `-[MTLDevice newEvent]`, so thread-local
   arming works. GPTK 4.0b2 exports `D3DMDevice::UseInternalHeaps`. With it
   cleared during arming, no heap suballocation was seen.
3. For a DEFAULT render-target/shader-resource texture, D3DMetal requests
   **Shared** storage and usage `ShaderRead|RenderTarget|PixelFormatView`
   (`0x15`), whatever the format.
4. D3DMetal backs `ID3D11Fence` with a plain **`MTLEvent`** (`newEvent`), not an
   `MTLSharedEvent`.

### Central question

> Can a native OpenXR runtime-created IOSurface-backed texture be exposed
> through D3DMetal as an `ID3D11Texture2D` referring to the same underlying
> allocation, with GPU-side synchronization and no texture copy?

**There is no supported API.** D3DMetal exposes no import of an existing
`MTLTexture`, `IOSurface` or `MTLSharedEvent`, and its D3D sharing entry points
are stubs (v3.0, per d3dmetal-native). Allocation interposition works instead:
arm the thread, call `CreateTexture2D`, and D3DMetal's own
`newTextureWithDescriptor:` returns the runtime texture.

- **Runtime-owned Private `MTLTexture`** (Monado's in-process direct
  swapchains, `newSharedTextureWithDescriptor:`): **yes, zero copy with GPU
  synchronization, demonstrated** with GPTK 4.0b2 under Metal validation. D3D11
  renders into the runtime's object, native Metal reads the pixels, native
  writes are visible to D3D11, and the D3D11 fence signals the session
  `MTLSharedEvent` that the runtime queue waits on. Two rules make this work.
  A Private runtime texture may serve D3DMetal's Shared request; D3DMetal uses
  these textures only on the GPU, and validation would assert on CPU access.
  And the session `MTLSharedEvent` is handed out from `newEvent`, since an
  `MTLSharedEvent` is an `MTLEvent`.
- **IOSurface-backed (Shared, heapless) texture**: **refused, not tested.**
  d3dmetal-native found that D3DMetal 3.0 zero-fills new Shared textures
  through `[[tex heap] newBufferWithLength:...]`, which would dereference nil.
  Whether 4.0b2 still does so is untested; the interposer refuses these with
  `storage-mismatch-or-heapless-shared`. A shadow-heap shim like
  d3dmetal-native's could lift it. Monado's in-process path does not need it.

Diagnostics for runs on other GPTK/Wine versions:

| Condition | Reported as |
| --- | --- |
| Allocation on another thread or through an unhooked selector | `not-reached` (device-capture texture at session start) |
| Requested usage/format/storage differs from the runtime image | `usage-not-subset`, `descriptor-mismatch`, `storage-mismatch...`, with both descriptors logged |
| Pool suballocation | `heap-suballocation` |
| Fence not created through `newSharedEvent`/`newEvent` | `not-reached`, so sync becomes `fence-cpu-wait` |

## Phase 3/4: D3DMetal backend and synchronization

`D3DMetalInterop` (experimental):

- **Discovery**: `IDENTIFY` on the device vtable. `INTERPOSE` hooks
  `newTextureWithDescriptor:`, `newHeapWithDescriptor:` (to watch heap
  placement), `newSharedEvent` and `newEvent` on the process's `MTLDevice`
  classes. One capture-only 1×1 `CreateTexture2D` learns D3DMetal's
  `MTLDevice`; failure is a clear error.
- **Swapchain usage**: `extra_swapchain_usage()` returns
  `XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT`, so the native runtime creates its
  images with `PixelFormatView` (Monado maps the flag). The app's own create
  info is unchanged. This is a standard OpenXR flag; Monado is not changed.
- **Images**: for each runtime image, arm with that `MTLTexture`, then
  `CreateTexture2D(desc)`, then disarm. `substituted` means zero copy, and the
  D3D texture refers to the exact runtime object (+1 retained, released with
  the D3D texture). Any other result is an error, with requested and runtime
  descriptors logged.
- **Copy fallback** (`d3dmetal-copy` only): a capture-only `CreateTexture2D`
  gives a D3DMetal-owned image and its `MTLTexture`. At release, after the
  completion wait, the native half encodes `copyFromTexture:toTexture:` into
  the runtime image on the session queue. The swapchain log says
  `zero-copy=no fallback=explicit-gpu-blit` and the caps include
  `copy-fallback`. **Not exercised**: with GPTK 4.0b2 every Monado image is
  imported without a copy.
- **Sync**: arm the session `MTLSharedEvent`, then
  `ID3D11Device5::CreateFence`. D3DMetal's `newEvent` receives the session
  event, so the D3D11 fence's GPU signals land on it. Release is then the same
  as DXMT: `Signal`, `Flush`, native `encodeWaitForEvent`
  (`shared-event-gpu-wait`). If no substitution happens, the fallback is
  `fence-cpu-wait`: `Signal`, `SetEventOnCompletion`, `Flush`, wait for the
  Win32 event (5 s limit), then set the session event from the CPU. That is a
  CPU stall on the app thread, logged at session creation. It is not used with
  GPTK 4.0b2.

| Point | DXMT | D3DMetal 4.0b2 | D3DMetal fallback (unused) |
| --- | --- | --- | --- |
| `xrAcquireSwapchainImage` | runtime | runtime (index recorded in PE) | same |
| `xrWaitSwapchainImage` | runtime | runtime | runtime |
| `xrReleaseSwapchainImage` | fence = session event, `Signal`+`Flush`; native queue GPU wait | same | CPU wait on D3DMetal fence, CPU event signal |
| Remaining CPU wait | Monado's own empty-buffer completion in release (all backends) | same | plus the producer completion wait |

The Monado release behaviour (commit and wait an empty command buffer on the
bound queue) predates this branch and applies to every backend.

### D3D12

The interface carries `MW_GFX_CAP_D3D12`, but nothing implements it. With
D3DMetal the same interposition would apply to `CreateCommittedResource`, and
d3dmetal-native already handles D3D12 committed/placed resources and fences.
The OpenXR side needs D3D12 binding, queue and resource-state handling,
following Proton's D3D12 path. Not started.

## Phase 5: tests and results (2026-10-03/04, Apple M5, macOS 26.6.2)

All OpenXR runs used an isolated simulated-only ARM64 Monado service and the
matching x86_64 client (`v25.1.0-2074-g1b400a8e7`), with Metal validation
enabled.

| Test | DXMT (Wine 11.10) | D3DMetal 4.0b2 (CrossOver 26.3) |
| --- | --- | --- |
| Pre-refactor `in_process_swapchain.exe` | 12/12 PASS | n/a |
| `graphics_interop_probe.exe` | 17 PASS: Private 2D, Private 2-slice array and IOSurface 2D; zero copy; event not early; native reads D3D11; D3D11 reads native | 12 PASS for Private 2D and array; IOSurface REFUSED as designed; sync `shared-event-gpu-wait` |
| `in_process_swapchain.exe` (now backend-neutral; pixel checks in the runtime's own images) | 12/12 PASS | 12/12 PASS |
| Khronos `hello_xr` D3D11, Opaque, 30 s | FOCUSED, 3,044 frames, exit 0, 2 swapchains `zero-copy=yes` | FOCUSED, 3,146 frames, exit 0, 2 swapchains `zero-copy=yes` |
| Metal validation assertions (client and service) | 0 | 0 |

Independent of the D3D runtime:

- `graphics_interposer_test`, arm64 and x86_64, `MTL_DEBUG_LAYER` 0 and 1:
  PASS.
- ctest (rpc_serialization, rpc_transport, graphics_interposer,
  native_capability, tcp_auth): 5/5 PASS.
- Selection with `MWXR_GRAPHICS_BACKEND=d3dmetal` on DXMT: clean
  `E_NOINTERFACE`. With an invalid value: clean `E_INVALIDARG`.

Not run: PS VR2 hardware, the Meta runtime, real games on D3DMetal, the
`d3dmetal-copy` fallback, and proxy/native-host end-to-end runs (no code there
changed; their native targets build and their ctest suites pass).

How copies are detected: DXMT retains the exact object. D3DMetal substitution
returns the exact armed pointer (asserted by the interposer test, and logged
as `substituted`). The pixel probes then see D3D11 writes in the runtime's own
objects. A bridge copy can only come from `fallback=explicit-gpu-blit`, which
is logged per swapchain. For a Metal GPU capture, set
`MTL_CAPTURE_ENABLED=1`. A zero-copy run contains no bridge blit encoder; the
only blits are the probes' own readbacks.

## Building the GPTK runtime

```zsh
# CrossOver FOSS source, x86_64 Wine (needs bison >= 3: brew install bison).
# Use a macOS 26 SDK on macOS 26: with the 27.0 SDK configure enables pipe2(),
# which macOS 26 lacks, and wineserver startup crashes in init_thread_pipe.
export SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
arch -x86_64 env BISON=/opt/homebrew/opt/bison/bin/bison CC="clang -arch x86_64" \
  CXX="clang++ -arch x86_64" ../sources/wine/configure --prefix=$PWD/../install \
  --enable-archs=x86_64 --disable-tests --without-x --without-freetype --without-gnutls \
  --without-gstreamer --without-sdl --without-cups --without-sane --without-usb \
  --without-v4l2 --without-pcap --without-krb5 --without-gphoto --without-capi \
  --without-netapi --without-opencl --without-ffmpeg
# CrossOver HACK 25909 in win32u/vulkan.c needs SONAME_LIBVULKAN even when no
# x86_64 Vulkan was found; define it in the generated include/config.h:
printf '#ifndef SONAME_LIBVULKAN\n#define SONAME_LIBVULKAN "libvulkan.1.dylib"\n#endif\n' >> include/config.h
arch -x86_64 make -j && arch -x86_64 make install

MWXR_CROSSOVER_INSTALL=/path/to/install MWXR_GPTK_LIB=/path/to/redist/lib \
MWXR_GPTK_RUNTIME=/path/to/runtime-gptk scripts/install-gptk-runtime.zsh
```

Build `wineopenxr` and the probes against the CrossOver SDK. Set
`MWXR_WINE_SDK` to the install, `MWXR_WINE_SOURCE` to `sources/wine` and
`MWXR_WINE_RUNTIME` to the GPTK runtime, with a separate
`MWXR_IN_PROCESS_BUILD`. The runners skip the Wine Mono/Gecko dialogs during
`wineboot`.

## Commands

```zsh
scripts/build-graphics-interop-probe.zsh
scripts/run-graphics-interop-probe.zsh                     # DXMT runtime
MWXR_PROBE_WINE=/path/to/runtime-gptk MWXR_PROBE_PREFIX=/path/to/prefix-gptk \
MWXR_GRAPHICS_BACKEND=d3dmetal scripts/run-graphics-interop-probe.zsh

MWXR_IN_PROCESS_WINE=/path/to/runtime-gptk MWXR_IN_PROCESS_PREFIX=/path/to/prefix-gptk \
MWXR_GRAPHICS_BACKEND=d3dmetal scripts/run-in-process-simulated.zsh app.exe ...
```

## Status summary

1. **Implemented and tested:**
   - The interface and selection.
   - The DXMT backend with no behaviour change.
   - The D3DMetal backend with GPTK 4.0b2 on CrossOver 26.3, zero copy with
     GPU shared-event synchronization: interop probe, runtime-image pixel
     probe and `hello_xr` against simulated Monado.
   - The native interposer (both architectures).
   - The probes, and the reproducible GPTK runtime recipe.
2. **Implemented, not runtime-tested:**
   - The `d3dmetal-copy` fallback and its release blit.
   - The `fence-cpu-wait` sync fallback.
   - Native (non-PE) D3DMetal vtable identification.
3. **Investigated, blocked or unsupported:**
   - D3DMetal has no API to import external textures, IOSurfaces or events.
   - IOSurface-backed (heapless Shared) runtime textures are refused, pending
     a shadow-heap shim; the 4.0b2 behaviour there is untested.
4. **Future:**
   - PS VR2 hardware and real-game runs on D3DMetal; Game Mode / client
     compositor runs.
   - The shadow-heap shim.
   - D3D12.
   - Checking new GPTK releases against the diagnostics table.

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

### Local availability

No Game Porting Toolkit, `D3DMetal.framework` or `libd3dshared.dylib` is
installed on this Mac (searched `/Applications`, `/Library`, `~`, `/opt`,
`/usr/local`). The D3DMetal backend is therefore built but not executed.
Apple binaries are not vendored.

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

Consequences for this bridge:

1. The app's `ID3D11Device*` may be a native D3DMetal object or a PE object
   that thunks through `__wine_unix_call`. Identification accepts either: a
   vtable inside `D3DMetal.framework`, or a PE vtable while the framework is
   loaded and DXMT has declined.
2. Either way, D3DMetal's Metal allocations run on the calling OS thread, as
   Wine unix calls do, unless D3DMetal defers them to a worker. Thread-local
   arming relies on this. It is checked on every call, not assumed.
3. The in-process runtime is currently built and tested against stock Wine
   11.10. A D3DMetal test needs `wineopenxr` loaded into a CrossOver-based GPTK
   Wine. That Wine's unix-call ABI must match the
   `__wine_init_unix_call`/`__wine_unix_call_funcs` split used here.

### Central question

> Can a native OpenXR runtime-created IOSurface-backed texture be exposed
> through D3DMetal as an `ID3D11Texture2D` referring to the same underlying
> allocation, with GPU-side synchronization and no texture copy?

**No supported API can do it.** D3DMetal exposes no import of an existing
`MTLTexture`, `IOSurface` or `MTLSharedEvent`, and its D3D sharing entry points
are stubs in v3.0. The only route is allocation interposition: arm the thread,
call `CreateTexture2D`, and have D3DMetal's own `newTextureWithDescriptor:`
return the runtime's texture. This is implemented and its mechanism is
verified with plain Metal. It is **not demonstrated with D3DMetal**.

For the *IOSurface-backed* case, the answer for D3DMetal ≤ 3.0 is **blocked
even by interposition**. IOSurface textures use Shared storage and have no
`-heap`, and d3dmetal-native found that D3DMetal zero-fills new Shared
textures through `[[tex heap] newBufferWithLength:...]`, which would
dereference nil. The interposer refuses these with
`storage-mismatch-or-heapless-shared`. Monado's in-process direct swapchains
are Private `newSharedTextureWithDescriptor:` textures and are not affected.
A shadow-heap shim, as d3dmetal-native uses, could lift this but is not
implemented.

Remaining unknowns, each reported precisely at runtime:

| Unknown | Reported as |
| --- | --- |
| `CreateTexture2D` allocates on another thread or through an unhooked selector | `not-reached` (probe texture at session start) |
| D3DMetal requests usage/format/storage that differs from the runtime image | `usage-not-subset`, `descriptor-mismatch`, `storage-mismatch...` with both descriptors logged |
| Pool suballocation despite clearing `UseInternalHeaps` | `heap-suballocation` |
| D3DMetal uses a private `MTLEvent` for D3D11 fences | `private-mtlevent`, so sync becomes `fence-cpu-wait` |
| D3DMetal relies on private texture state (residency, aliasing) the runtime texture lacks | not detectable in advance; needs a GPU capture and pixel probe on the target Mac |

## Phase 3/4: D3DMetal backend and synchronization

`D3DMetalInterop` (experimental):

- **Discovery**: `IDENTIFY` on the device vtable. `INTERPOSE` hooks
  `newTextureWithDescriptor:`, `newHeapWithDescriptor:` (to watch heap
  placement), `newSharedEvent` and `newEvent` on the process's `MTLDevice`
  classes. One capture-only 1×1 `CreateTexture2D` learns D3DMetal's
  `MTLDevice`; failure is a clear error.
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
  `copy-fallback`.
- **Sync**: arm the session `MTLSharedEvent`, then
  `ID3D11Device5::CreateFence`. If substituted, the D3D11 fence is the session
  event and release is the same as DXMT: `Signal`, `Flush`, native
  `encodeWaitForEvent`. That is `shared-event-gpu-wait`, although whether
  D3DMetal encodes the signal on the GPU or sets it on completion is internal
  to D3DMetal. Otherwise `fence-cpu-wait`: `Signal`, `SetEventOnCompletion`,
  `Flush`, wait for the Win32 event (5 s limit), then set the session event
  from the CPU. This is a real CPU stall on the app thread, used only when the
  GPU route was refused, and it is logged at session creation.

| Point | DXMT | D3DMetal (substituted fence) | D3DMetal (fallback) |
| --- | --- | --- | --- |
| `xrAcquireSwapchainImage` | runtime | runtime (index recorded in PE) | same |
| `xrWaitSwapchainImage` | runtime | runtime | runtime |
| `xrReleaseSwapchainImage` | fence = session event, `Signal`+`Flush`; native queue GPU wait | same | CPU wait on D3DMetal fence, CPU event signal; native queue wait already satisfied |
| Remaining CPU wait | Monado's own empty-buffer completion in release (all backends) | same | plus the producer completion wait |

The Monado release behaviour (commit and wait an empty command buffer on the
bound queue) predates this branch and applies to every backend.

### D3D12

The interface carries `MW_GFX_CAP_D3D12`, but nothing implements it. With
D3DMetal the same interposition would apply to `CreateCommittedResource`, and
d3dmetal-native already handles D3D12 committed/placed resources and fences.
The OpenXR side needs D3D12 binding, queue and resource-state handling,
following Proton's D3D12 path. Not started.

## Phase 5: tests and results (2026-10-03, Apple M5, macOS 26.6.2)

| Test | Result |
| --- | --- |
| Pre-refactor baseline: `in_process_swapchain.exe`, isolated simulated Monado `v25.1.0-2074-g1b400a8e7` | 12/12 pixel checks PASS |
| After DXMT refactor, same probe | 12/12 PASS, identical log semantics plus summaries |
| `graphics_interposer_test`, arm64 and x86_64, `MTL_DEBUG_LAYER` 0 and 1 | PASS: exact-object substitution (2D, array), refusal of descriptor/usage/storage/IOSurface, capture-only, per-thread isolation, heap report, shared-event substitution, private-event report, CPU signal |
| ctest (rpc_serialization, rpc_transport, graphics_interposer, native_capability, tcp_auth) | 5/5 PASS |
| `graphics_interop_probe.exe` under direct-object DXMT, Wine 11.10, Metal validation | 0 failures. Private 2D, Private array (2 slices) and IOSurface Shared 2D: zero copy, event not early, native reads D3D11 clears, D3D11 reads native writes |
| Same, `MWXR_GRAPHICS_BACKEND=dxmt` / `d3dmetal` / invalid | PASS / clean `E_NOINTERFACE` refusal / clean `E_INVALIDARG` |
| Final build: runtime-image probe, core gate | 12/12 PASS; gate PASS |
| Khronos `hello_xr` D3D11, Opaque, ~30 s, simulated Monado, Metal validation | FOCUSED, 2,713 delivered frames, exit 0, both swapchains `zero-copy=yes`, no validation errors in client or service logs |

Not run: any D3DMetal execution, PS VR2 hardware and the Meta runtime (no
code there changed), and proxy/native-host end-to-end runs (no code there
changed; their native targets build and their ctest suites pass).

How copies are detected: DXMT retains the exact object. D3DMetal substitution
returns the exact armed pointer, which the interposer test proves with plain
Metal. The probe's native readback then sees D3D11 writes in the runtime
object without any bridge blit. For a Metal GPU capture, set
`MTL_CAPTURE_ENABLED=1`. A zero-copy run contains no bridge blit encoder; the
only blits come from the probe's own readback (`verify`) or from
`fallback=explicit-gpu-blit`.

## Commands

The interop probe needs no OpenXR runtime:

```zsh
export DXMT_SOURCE_DIR=... MWXR_WINE_SDK=... MWXR_WINE_SOURCE=...
scripts/build-graphics-interop-probe.zsh
scripts/run-graphics-interop-probe.zsh                     # DXMT runtime
```

On a Mac with GPTK, the Wine runtime must already carry the GPTK overlay
(D3DMetal PE modules and the `libd3dshared` unix modules), for example a
Silo/CrossOver runtime:

```zsh
MWXR_PROBE_WINE=/path/to/gptk-wine MWXR_PROBE_PREFIX=/path/to/new-prefix \
MWXR_GRAPHICS_BACKEND=d3dmetal scripts/run-graphics-interop-probe.zsh
```

Expected outcomes to record: the `d3dmetal device ...` line; the interposer
hook count and heap-pool switch; the bind line, which gives the sync mode;
for each case, `zero-copy=yes` with PASS, or `REFUSED` with the logged
detail. The IOSurface case is expected to be REFUSED (`storage-...`). The
`wineopenxr.so` and probe `.so` may need rebuilding against that Wine's
headers. Then run `in_process_swapchain.exe` and `hello_xr` the same way with
`MWXR_GRAPHICS_BACKEND=d3dmetal`. The runtime-image probe imports its own
event through DXMT and must be adapted for D3DMetal first. Use `d3dmetal-copy`
only as a labelled comparison.

## Status summary

1. **Implemented and tested:** interface and selection; DXMT backend with no
   behaviour change (OpenXR probe, hello_xr, interop probe); the native
   interposer mechanism (plain-Metal test, both architectures); the
   interop probe harness; backend selection errors.
2. **Implemented, not runtime-tested:** the D3DMetal backend (identification,
   device capture, image substitution, fence substitution, CPU-wait sync,
   copy fallback and its release blit). No D3DMetal is available here.
3. **Investigated, blocked:** no D3DMetal API to import external textures,
   IOSurfaces or events, and sharing is stubbed in v3.0. IOSurface-backed,
   heapless Shared runtime textures cannot be substituted safely into
   D3DMetal ≤ 3.0. Zero copy with D3DMetal is not demonstrated.
4. **Future:** run on a GPTK Mac; a shadow-heap shim for Shared/IOSurface
   images; D3D12; a GPTK-Wine build recipe for `wineopenxr`; adapting the
   runtime-image probe to the interop API.

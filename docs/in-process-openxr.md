# In-process Wine OpenXR: Phase 0 study

Date: 2026-10-02. **Design proposal only; implementation and execution have not
started.** Phase 0 stops here. The proxy and native-host implementations remain
available and unchanged.

## Finding

Proton is a suitable structural base: a Windows builtin runtime DLL, generated
OpenXR thunks, and a native unixlib in the same process. Replace its
DXVK/Vulkan-specific adaptation with DXMT/Metal adaptation. This removes the
bridge TCP/RPC/host boundary without moving Windows compatibility code into
Monado.

Two prerequisites precede the vertical slice:

1. This Wine/DXMT installation runs x86_64 under Rosetta. Its native half must
   load an **x86_64 or universal** Khronos loader, Monado client runtime, and
   dependencies. The inspected local Monado runtime dylibs are arm64-only.
   An arm64 dylib cannot be loaded into this process. Build a matching client
   runtime and check its IPC ABI against the arm64 service; do not assume the
   current native-host binaries can be reused in-process.
2. Current `IDXMTNativeDevice` imports capability **names**, not raw Metal
   objects. Direct-object imports and device identification need a small,
   versioned generic DXMT interface extension. No Monado source change has
   been established as necessary. Stop and explain first if the build or
   runtime experiments establish such a requirement.

## Source baseline

Source inspected, rather than inferred from old Proton descriptions:

- Proton default branch `proton_11.0`,
  `5b89db940e0ebe3a137a6009a3589232fe084c09`.
- Its Wine submodule `dc26e61847081a1b5cb0733dc30feba6ee575482`.
- `NikNakk/macos-wine-xr` main,
  `5665797192a79f56903d62df81419bc3da37379e`.
- `NikNakk/dxmt:macos-xr-native-sharing`,
  `e4e85d294126d69b1f2a29090b5a029f8f3b8fdb`.
- Monado client/compositor sources and status documents in the supplied
  workspace. Existing local modifications were preserved.

The new bridge branch is `codex/in-process-wine-openxr`. This study checkout is
under `.build/in-process-openxr-study` in the supplied Monado workspace; it has
the requested bridge repository as its origin. No branch has been published.

## Proton structure, generation and loading

[`wineopenxr/Makefile.in`](https://github.com/ValveSoftware/Proton/blob/5b89db940e0ebe3a137a6009a3589232fe084c09/wineopenxr/Makefile.in)
declares `MODULE = wineopenxr.dll`, `UNIXLIB = wineopenxr.so`, and imports
advapi32, user32, dxgi and winevulkan. It builds `openxr_loader.c`,
`loader_thunks.c`, `openxr.c` and `openxr_thunks.c`.

- **PE half:** `openxr_loader.c` implements loader negotiation, Windows-facing
  object wrappers, graphics requirements, format translation, D3D image
  wrappers, event/layer handle translation and special graphics calls.
  `loader_thunks.c` supplies ordinary Windows entry points and function lookup.
- **Native half:** `openxr.c` supplies special native operations and extension
  substitution. `openxr_thunks.c` supplies native ABI conversion, dispatch and
  `__wine_unix_call_funcs`. `openxr_private.h` provides conversion allocation
  and native dispatch support.
- **Boundary:** `__wine_init_unix_call()` initializes the unixlib;
  `UNIX_CALL(operation, &params)` invokes its indexed entry table. This is the
  modern split, not an older winelib DLL implemented wholly as a `.dll.so`.
  In macOS builds a file called `.so` can still be a Mach-O library.

[`make_openxr`](https://github.com/ValveSoftware/Proton/blob/5b89db940e0ebe3a137a6009a3589232fe084c09/wineopenxr/make_openxr)
is a Python generator descended from Wine's Vulkan generator. Its stale
comments saying “Vulkan xr.xml” do not describe its actual input: OpenXR
registry XML. It pins registry release **1.1.58**, supports core versions
through **1.1**, accepts `--xml`/`-x`, or downloads the pinned Khronos registry.
It emits `wineopenxr.h`, `openxr_thunks.[ch]`, `loader_thunks.[ch]` and
`wineopenxr.json`. `loader_structs.h` is not among those emitted files.

Coverage is registry-driven, filtered by core version, platform guards,
extension dependencies and unsupported-extension lists. Windows, Vulkan,
OpenGL, D3D11 and D3D12 guards are admitted; **Metal is not**. Debug utils,
loader init, perception-anchor interop and HTC foveation are excluded by the
explicit list; this is not universal OpenXR coverage. Separate manual Unix,
manual loader and override tables identify graphics, lifecycle, negotiation
and time-conversion calls. Ordinary actions, spaces and frame calls are
generated. Generation includes structure/chain conversion and handle
unwrapping; it must not be replaced with hand-written RPC codecs.

For our slice, preserve this generator and its boundary, admit native Metal
types, restrict exposed API to OpenXR 1.0 plus D3D11, and retain core output
chains such as `XrSpaceVelocity`. Keep manual adaptation for graphics,
swapchain ownership and event/layer wrapper translation. Generated presence
alone must not advertise unsupported extensions or functions.

The native loader is **linked**, not opened with a wineopenxr-specific
`dlopen()` search. Proton's
[`Makefile.in`](https://github.com/ValveSoftware/Proton/blob/5b89db940e0ebe3a137a6009a3589232fe084c09/Makefile.in)
sets `WINEOPENXR_LDFLAGS = -lopenxr_loader`, with Wine and OpenXR dependencies;
`make/rules-makedep.mk` adapts the Wine build rules for this out-of-tree module.
Native code calls loader `xrCreateInstance` and `xrGetInstanceProcAddr`, then
populates its dispatch table. The native loader selects the native runtime.

Runtime registration is ordinary Windows OpenXR registration. Proton copies
`wineopenxr64.json` into `C:\openxr`; its pinned
[`wine.inf.in`](https://github.com/ValveSoftware/wine/blob/dc26e61847081a1b5cb0733dc30feba6ee575482/loader/wine.inf.in)
sets `HKLM\Software\Khronos\OpenXR\1\ActiveRuntime` to
`C:\openxr\wineopenxr64.json`. That JSON names
`C:\windows\system32\wineopenxr.dll`. The DLL exports
`xrNegotiateLoaderRuntimeInterface`, validates negotiation structures/interface
versions, and returns its `xrGetInstanceProcAddr`. Registration is prefix setup,
not self-registration by each API call. Proton's build explicitly omits 32-bit
wineopenxr for SteamVR; our slice should likewise be Win64 only.

Use the same manifest/registry model in a private Wine prefix. Keep the native
runtime manifest distinct: `XR_RUNTIME_JSON` in the native loader's environment
must identify Monado, not this Windows wrapper. Using registry registration
for the PE loader avoids recursively feeding its JSON to the native loader.
Preserve and restore the private prefix's previous ActiveRuntime when running
comparisons. Prefer linked native loader loading to match Proton; any optional
explicit loader override is a deliberate macOS deployment choice.

## Proton graphics and synchronization

The handwritten
[`openxr_loader.c`](https://github.com/ValveSoftware/Proton/blob/5b89db940e0ebe3a137a6009a3589232fe084c09/wineopenxr/openxr_loader.c)
does the graphics adaptation; ordinary generated calls do not accomplish it.
Native extension substitution maps Windows D3D11/D3D12 enable to
`XR_KHR_vulkan_enable`. Windows QPC conversion maps to native timespec conversion.

For **D3D11**, it queries the application's device for
`IDXGIVkInteropDevice2`; incompatible devices fail. DXVK supplies Vulkan
instance/device/physical-device and submission queue indices. The native
session receives a Vulkan graphics binding for that device/queue. Runtime
swapchain formats translate between Vulkan and DXGI. Runtime-owned Vulkan
images are enumerated and wrapped using `CreateTexture2DFromVkImage`, with the
requested dimensions, layers, samples, mip count and bind usage. The wrapper
does not render into replacement storage and copy it into the XR image.

For **D3D12**, it queries `ID3D12DXVKInteropDevice2` (older interface fallback)
and `ID3D12DeviceExt1`, retains the application's queue, obtains native Vulkan
handles and queue details, and wraps runtime images with
`CreateResourceFromBorrowedHandle`. It allocates acquire/release command buffers
to transition between vkd3d's D3D resource layouts and the runtime's expected
color/depth attachment layouts. Acquired image indices are tracked in order;
failed releases revert the transition.

Synchronization uses the **same Vulkan queue** plus interop submission locks.
D3D11 release drains DXVK rendering commands via `FlushRenderingCommands` and
locks its submission queue across the native call. D3D12 uses Vulkan/command
queue locks and submits its layout-transition command buffers on that queue.
Acquire, release and some frame calls use those locks. This is not a
cross-process shared-handle/fence transport, and it cannot simply be translated
to “call Flush, then release” when DXMT and the native runtime use different
Metal queues.

Proton also has Vulkan-extension discovery/registry coordination with its
Wine graphics stack. Retain the graphics-requirements semantics, but replace
that Vulkan preflight with Metal device requirements and DXMT adapter selection;
do not carry SteamVR/Vulkan registry dependencies into the macOS slice.

## Licence and reuse

The bridge's `LICENSE` is **BSL-1.0**, not LGPL. Proton's root `LICENSE` states
that licences vary and directs top-level content to `LICENSE.proton`
(BSD-3-Clause), while component licences apply separately.

The actual wineopenxr files are mixed:

- `make_openxr` explicitly carries **LGPL-2.1-or-later**, including its Wine
  generator provenance. Preserve that licence and attribution when adapting it.
- Generated headers/thunks carry the Khronos registry notice,
  **Apache-2.0 OR MIT**. MIT is the simpler compatible option, with the notice
  retained. That notice does not erase any separately protected generator
  template code incorporated in output; audit the adapted outputs as well.
- Handwritten wineopenxr files inspected have no separate licence headers or
  directory COPYING file. The repository's stated top-level BSD licence is the
  visible fallback; do not label the entire directory LGPL merely because its
  generator is LGPL, or BSL merely because our repository is BSL. Preserve
  provenance and check inherited material before assigning per-file notices.
- Wine support headers/build code and the current DXMT native-sharing interface
  have their own LGPL obligations.

**Reuse is possible in this repository with explicit per-file licensing and
bundled third-party notices; wholesale BSL relicensing is not.** Keep the
adapted generator and any LGPL-derived material separately identified, preserve
BSD/MIT notices, and ship source/licence material as required for distributed
LGPL components. Source coexistence does not itself require relicensing every
independent bridge file. If an exclusively BSL deliverable were required,
LGPL generator/template implementation would need to remain design reference
or be independently replaced; no such constraint was requested. Confirm the
small vendored file set's provenance before the first implementation commit.
The applicable LGPL terms are in
[Wine's COPYING.LIB](https://github.com/ValveSoftware/wine/blob/dc26e61847081a1b5cb0733dc30feba6ee575482/COPYING.LIB).

## Wine 11.10 and macOS build feasibility

`scripts/build-current-dxmt.zsh` uses Wine **8.16-3shain as the link-time SDK**,
LLVM 15 and MinGW, and installs the resulting builtin DLLs into a private copy
of **Wine 11.10 for execution**. These are not the same Wine build. It invokes
DXMT's Meson build with `wine_builtin_dll=true`, installs PE binaries under
`lib/wine/x86_64-windows`, and `winemetal.so` under `lib/wine/x86_64-unix`.
The existing evidence includes Wine 11.10 hello_xr and imported-fence execution.

DXMT's `src/winemetal/main.c` initializes unix calls in DllMain. Its Meson
rules link the PE half against winecrt0/ntdll and mark it builtin with
`winebuild --builtin`; its native half is compiled as Objective-C and linked
to macOS frameworks and native Wine libraries. The locally installed
`winemetal.so` is verified by `file` as **Mach-O x86_64**. Therefore this
installation already supports the required split.

This proves feasibility, not binary compatibility for an unbuilt new module.
Use Wine 11.10-matching source headers/tools where possible, or explicitly
validate an external build against the existing SDK as DXMT does. Proton's
Linux makedep recipe is a model, not a drop-in macOS build system. Native
Objective-C belongs behind unix calls; never invoke native ObjC methods through
Windows calling-convention function pointers. Keep opaque 64-bit handles and
retain/release rules at the boundary. Initial scope excludes WoW64/32-bit and
ARM64EC/FEX variants.

## Proposed Metal mapping

```text
Windows app / OpenXR loader / D3D11
  -> builtin wineopenxr PE half + generated thunks
  -> __wine_unix_call, same process
  -> macOS native half -> Khronos loader -> Monado client runtime
       -> normal Unix IPC/XPC -> Monado service
       or optional hosted compositor inside this Wine process
```

1. Native extension enumeration requires `XR_KHR_metal_enable`; expose only
   `XR_KHR_D3D11_enable` for the requested graphics slice. Native instance
   creation substitutes Metal for D3D11. Reject unsupported next chains and
   graphics bindings explicitly.
2. Call native Metal graphics requirements before session creation. Match the
   DXMT adapter/device to the runtime's required GPU and obtain DXMT's exact
   native device through a versioned interface. Return an accurate Windows
   adapter LUID/feature level rather than inventing a constant. Bind a native
   Metal queue with explicit producer synchronization.
3. Let the runtime create ordinary Metal swapchains. Enumerate every native
   image and wrap **that same Metal texture object/storage** as D3D11. Validate
   device, pixel format/DXGI format, width, height, array length, usage, sample
   count and mip count. Preserve ownership until both runtime and DXMT work
   have completed. Initially support DEFAULT, single-mip/sample 2D and 2D-array
   color images with render-target/shader-resource bindings, matching current
   host limits. Fail creation/enumeration if exact direct wrapping is impossible.
4. Add direct-object texture/event import and device identity to DXMT using a
   new interface/IID, rather than changing the existing interface's vtable.
   Its existing `ImportSharedTexture` and `ImportSharedEvent` take broker names
   and must remain available to the host path. Implement direct imports through
   DXMT's existing winemetal opaque-object mechanism, reusing descriptor checks,
   resource wrappers and fence machinery.
5. On release, enqueue a DXMT context fence signal at an increasing shared-event
   value after application rendering, and flush actual submission. On the queue
   passed in `XrGraphicsBindingMetalKHR`, commit a command buffer waiting for
   that value **before** native `xrReleaseSwapchainImage`. Monado currently
   commits an empty command buffer to that queue and waits for completion before
   releasing to its compositor: the earlier event wait therefore orders the
   producer. Alternatively share DXMT's exact rendering queue if a supported
   interface provides it. Never block while holding a lock needed by DXMT's
   submission worker. Acquire/wait remains the runtime's ownership gate.

**Direct object passage is valid within this process, conditional on adding
the DXMT API above.** There is no intrinsic need for bridge Mach-port names,
XPC tokens, shared-handle reopening or texture blits, and non-exportable
textures can potentially be wrapped. This is not already supported by the
current public `IDXMTNativeDevice`. Also, it does not eliminate XPC/IOSurface
transport internal to Monado's service-backed client path. No claim is made
that an asynchronous event can be passed through standard OpenXR release:
the proposed queue wait works with Monado's current public Metal behavior,
including its CPU completion wait, without adding an extension to Monado.

Log a distinct direct-object zero-copy selection, image identities and device
identity. A descriptor/import mismatch or need for replacement storage/blit
must be a loud failure against Monado. Do not borrow the native host's staging
fallback for this path. Event `.device == nil` is legitimate for MTLSharedEvent
and must not be rejected; texture device checks remain necessary.

## Removed bridge pieces and remaining risks

For this third path, the following become unnecessary (retained for fallbacks):
TCP listener/connection, HMAC bootstrap/authentication, wire framing/schema
fingerprints/codecs, RPC IDs and host-side state tables, native host process,
bridge bootstrap capability brokers, texture/fence name export and reopening,
proxy republishing into Monado XPC, bridge staging/blit strategy, and
proxy-owned frame-period policy. Generated native ABI thunks, graphics wrappers,
format conversion, device selection and lifetime accounting remain necessary.

- **Game Mode:** runtime in-process does not automatically mean compositor
  in-process. With `XRT_MACOS_CLIENT_COMPOSITOR=1`, Monado's hosted compositor
  should be created in the Wine game's process, which is the intended benefit.
  The service still tracks hardware and hosts CALayerHost. Verify the actual
  favoured PID/coalition, compositor priority and Wine fullscreen policy;
  native Unreal results do not validate Wine. MoltenVK and all hosted-client
  dependencies also need x86_64 builds. Pose IPC remains in the current design.
- **AppKit/run loops:** Wine owns NSApp and its macdrv AppKit thread. Monado's
  hosted frontend explicitly leaves NSApp alone, creates CAContext/CAMetalLayer,
  and flushes transactions; that is promising. Its standalone window frontend
  creates/activates NSApp and should not be invoked blindly inside Wine.
  Exercise creation from a Windows worker thread, layer teardown and session
  recreation. CVDisplayLink/presentation workers are native threads; any
  required main-thread dispatch must integrate with Wine's existing AppKit
  machinery, not start a second application run loop. The optional Metal display
  link implementation has its own NSThread/NSRunLoop; current default pacing is
  CVDisplayLink. Use per-call/worker autorelease pools.
- **XPC/launchd:** a native half in an unsandboxed Wine process should use the
  same bootstrap namespace and verified Unix PID/UID as an ordinary client.
  Cold activation, short-lived NSXPC connections, native callbacks, PID-scoped
  resource ownership and cleanup need actual Wine tests. Wine prefix/registry
  paths are not native filesystem paths. Avoid activation from DllMain.
- **Architecture/ABI:** x86_64 loader/runtime dependencies are the immediate
  gate. Check shared-memory layout and socket protocol against the arm64
  service, and native handles versus Windows calling conventions. If source
  changes to Monado are necessary, stop before making them.
- **Lifetime/concurrency:** DXMT queues, wrappers and runtime images must have
  deterministic destruction order; event waits must not outlive their producer.
  Preserve core chain outputs and translate embedded handles in events and
  projection layers. Pass actions/spaces/events semantically straight through
  the generated adaptation, without a second bridge state tracker.
- **Existing failures:** Meta alpha blend with Metal validation already fails
  in a native app. Underture's black eye images occur before bridge transport,
  including the older comparison stack. Accepted xrEndFrame calls do not prove
  visible game output; neither issue should be attributed to the new path or
  silently worked around by it.

## Proposed small buildable steps and validation

1. Licence/provenance notices; pinned generator; PE/unixlib loader and native
   instance/system queries; x86_64 Monado client against arm64 simulated service.
2. Generic direct-object DXMT interface and independent every-image/slice GPU
   pattern/event-ordering probe, including an unsignaled-event failure case.
3. D3D11 graphics requirements/session/swapchains and strict zero-copy policy.
4. Generated core actions/spaces/events and stereo projection frame lifecycle;
   prefix-local registration and a dedicated runner.
5. Validation and timing support; README status changes only as evidence warrants.

Phase 2 will run D3D11 hello_xr against simulated Monado with Metal validation,
Opaque/AlphaBlend, separate 2D and array swapchains, checking every image/slice.
Exercise session replacement/teardown and all accepted chains. Then run the
existing OpenComposite legacy title without claiming success from frame counts.

Compare all three paths with identical machine, Wine/DXMT revisions, native
runtime/service build, simulated/headset mode, image sizes, workload and warm-up.
Measure PE-entry to PE-return latency distributions for xrLocateSpace and each
xrGetActionState type. Record xrWaitFrame separately as an intentional blocking
call, including predicted display time/period and actual wake time. Capture
frame-interval median/p95, sample counts, and late frames using an explicitly
defined deadline; keep app submission and actual presentation metrics separate.

At least one comparison must leave the bridge pacing hint unused and
`U_PACING_APP_USE_MIN_FRAME_PERIOD` unset, recording inherited settings too.
Enable existing Monado `PSVR2_TIMING_TRACE=1` and set an existing writable
`PSVR2_TIMING_TRACE_DIR`; `monado_psvr2_<pid>_app_pacing.csv` already records
predict/delivered/gpu_done timing and CPU/draw/GPU estimates and actuals. Collect
it from the service PID or Wine PID according to compositor mode. A service
started earlier needs the trace environment at its own launch, not just in the
Wine shell. Collect orderly shutdown logs to retain buffered rows.

Exact commands for the new runner and hardware comparison will be provided
after it exists and has been tested in Phase 2. No hypothetical command is
presented here as executable. PS VR2 hardware runs remain user-owned; no hardware
or performance results are claimed by this study.

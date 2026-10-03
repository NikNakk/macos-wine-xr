# In-process Wine OpenXR

Date: 2026-10-02. Phase 0 study followed by the authorized x86_64 prototype.
**Core and independent DXMT GPU probes pass. Graphics adaptation builds;
Monado swapchain validation and Phase 2 are blocked by XPC test isolation.**
The proxy and native-host implementations remain available and unchanged.

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

The baseline bridge's `LICENSE` was **BSL-1.0**, before the licence decision below. Proton's root `LICENSE` states
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


## Implementation evidence: first core gate

The standalone prototype uses Proton's unchanged generator plus a small
configuration wrapper; it emits OpenXR 1.0 core PE/native thunks, excludes loader
API-layer entry points, and advertises no extensions while graphics is absent.
The PE DLL is marked builtin with winebuild; the native Mach-O unixlib links the
Khronos loader and initializes through __wine_init_unix_call. This is not a
usable D3D11 runtime yet: session/swapchain creation returns unsupported, and
requesting D3D11_enable returns XR_ERROR_EXTENSION_NOT_PRESENT.

2026-10-02 results on this machine:

- Wine 11.10 x86_64, its existing Wine 8.16 link-time SDK, current Proton Wine
  unixlib/list/debug headers: both PE and native modules built.
- Khronos OpenXR-SDK f2448a8797c85814aa892efc1ab8707900fbcc78 (1.1.63): loader
  built x86_64 with bundled jsoncpp, avoiding arm64 system jsoncpp linkage.
- Monado 0f919ce71f7b71c997d7ef22abffbbaadb9cce5f: x86_64 runtime client built
  with universal MoltenVK 1.4.2; no Monado source changes. The supplied workspace
  has unrelated local edits, preserved; this identifies the source revision,
  not a claim of a pristine whole tree.
- ARM64 simulated service using its existing main compositor: Windows probe
  xrCreateInstance=0, xrGetSystem=0 (system=1), xrGetInstanceProperties=0,
  xrDestroyInstance=0. Native module PID=68829 matches Monado's client PID=68829.
- Null-compositor test configuration failed vkCreateInstance with
  VK_ERROR_INCOMPATIBLE_DRIVER: its extension list lacks portability enumeration.
  Switching to the existing main compositor resolved it, without a source patch.

These results establish ordinary cross-architecture Monado control IPC only.
They do not establish Metal image transfer, zero copy, XPC cold activation,
Game Mode, app pacing, visible output or performance. Test registration uses
HKLM\Software\Khronos\OpenXR\1 ActiveRuntime and C:\openxr\wineopenxr64.json
only in build-in-process/prefix-core. The smoke probe directly loads the runtime
DLL; loader negotiation through the Windows Khronos loader still needs a test.

### Reproduce the core gate

Provide explicit checkouts and dependency paths; the scripts never replace the
existing Wine prefix/runtime or build the hardware service. The native build
requires a universal/x86_64 MoltenVK dylib; the tested Khronos v1.4.2 macOS tar
has SHA256 f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e.

```sh
export OPENXR_SOURCE_DIR=/path/to/OpenXR-SDK
export MONADO_SOURCE_DIR=/path/to/macos-monado
export MWXR_MOLTENVK=/path/to/universal/libMoltenVK.dylib
scripts/build-in-process-native.zsh
export MWXR_NATIVE_LOADER="$PWD/build-in-process/native/loader-x64/src/loader/libopenxr_loader.dylib"
export MWXR_NATIVE_RUNTIME_JSON="$PWD/build-in-process/native/monado-x64/openxr_monado-dev.json"
export MWXR_WINE_SDK=/path/to/wine-link-time-sdk
export MWXR_WINE_SOURCE=/path/to/pinned-proton-wine
export MWXR_WINE_RUNTIME=/path/to/wine-11.10
scripts/build-in-process-gate.zsh
# Start a separate ARM64 service with PSVR2/PSSENSE compiled out, SIMULATED_ENABLE=1,
# XRT_COMPOSITOR_NULL=0, and a unique existing XDG_RUNTIME_DIR, then:
export XDG_RUNTIME_DIR=/path/to/isolated-service-directory
scripts/run-in-process-gate.zsh
```

The build gate's Proton Wine headers were dc26e61847081a1b5cb0733dc30feba6ee575482.
The stand-alone Makefile.in is an integration sketch for a Wine-tree build;
only the external build script has been executed here. Build products and raw
logs live under ignored build-in-process/; they are not source artifacts.

### Licence decision

The new bridge distribution adopts LGPL-2.1-or-later. The original BSL licence
and source-level notices remain preserved under LICENSES/ and in legacy files;
third-party source retains its own notices. See LICENSES/README.md for pinned
Proton provenance. The earlier BSL-only assessment above describes the baseline
before this decision. Commercial intent does not determine compatibility.


## Direct-object DXMT prerequisite

The companion DXMT branch codex/in-process-metal-import, commit f8e535b, adds the new
IDXMTNativeDevice2 IID (5a6d2e1b-b10a-4ab7-8ade-d9a64c417283). The original
IDXMTNativeDevice ABI and broker imports are unchanged. The new methods return
DXMT's borrowed Metal device and import a raw texture/event with a retained
reference. Native validation checks texture device identity, descriptor, usage
and framebufferOnly. MTLSharedEvent.device==nil is accepted. The raw-import
route retains exactly the input object and records its GPU resource ID; no
sharing handle, replacement texture or blit is involved.

The independent probe tests three 2D objects and three two-slice arrays,
D3D11 render-target clears into every slice, a fence signal followed by Flush,
and native getBytes from each original shared-storage texture. All nine slice
checks passed with Metal API validation enabled on 2026-10-02. Six unsignaled
fence waits timed out as expected, six descriptor mismatches were rejected,
and passing a texture as an event was rejected. This is a DXMT prerequisite
probe. That initial result preceded the Monado runtime-owned image probe below.
The diagnostic now uses test-only GPU readback so it can inspect Private-storage
runtime images too; no production render copy is implemented.

Build DXMT with the same existing scripts/toolchain configuration, supplying
this companion checkout and an isolated build/install directory. Then:

```sh
export DXMT_SOURCE_DIR=/path/to/dxmt-direct-object-checkout
export DXMT_BUILD_DIR=/path/to/dxmt-direct-object-build
scripts/install-direct-metal-dxmt.zsh
scripts/build-direct-metal-probe.zsh
scripts/run-direct-metal-probe.zsh
```

The helper uses a test-only PE/unixlib DLL, so it creates Metal objects on the
actual DXMT device in the Wine process. The new runtime is copied to
build-in-process/wine-direct; the probe prefix is build-in-process/prefix-metal.
Do not use WINEDLLPATH alone to replace an existing engine DLL: Wine searches
its installed builtin directory first. The dedicated runtime avoids changing
either fallback engine. The recorded first successful run used prefix-core
with only test files replaced; the dedicated runner now keeps the probes apart.


## D3D11/Metal vertical slice and validation

The generated module advertises XR_KHR_D3D11_enable when the native loader
reports XR_KHR_metal_enable. Instance creation substitutes Metal; Windows Metal
entry points stay hidden. The PE side accepts OpenXR 1.0 only, one instance and
one session. Adapter requirements use DXMT's default LUID and validate the
actual Metal device against the native runtime's device at session creation.

PE session/swapchain wrappers, projection and event translation follow Proton's
manual-entry-point model. Actions and spaces use generated core dispatch.
Single-mip/sample, non-cube 2D/2D-array color images are supported, with RGBA/BGRA
8-bit UNORM/sRGB and RGBA16_FLOAT formats filtered against the native runtime.
Only empty frames or one stereo projection layer are accepted. Unsupported
usage flags and extension chains fail explicitly. No copy fallback exists.

Native runtime Metal images are imported through IDXMTNativeDevice2. Release
signals the DXMT shared-event fence, flushes submission, and enqueues an event
wait on the queue bound to the native OpenXR session. Monado's existing release
completion barrier on that queue orders producer writes before release.

Windows and native loaders have separate manifest selection. The Windows loader
uses prefix-local ActiveRuntime; MWXR_NATIVE_RUNTIME_JSON sets the native
libc environment's XR_RUNTIME_JSON without changing Wine's Windows environment.
The native loader remains linked, as in Proton. The application runner unsets
inherited Windows XR_RUNTIME_JSON.

### Endpoint isolation: resolved

The first graphics run failed token validation: a private Unix socket paired
with the fixed hardware Mach endpoint reached two different services. The
approved Monado commit `ffec3b71a` adds XRT_MACOS_METAL_IPC_SERVICE_NAME through
one shared accessor, preserving the original default and ignoring the override
on Linux. Its clients, listeners, external client and optional broker all use
that accessor. No Monado graphics or protocol change was needed.

An independent simulated-only LaunchAgent used
org.freedesktop.monado.inprocess-test.501 and the matching private Metal endpoint.
The registered hardware LaunchAgent was neither replaced nor reconfigured.
After SIGTERM of the private service, Wine caused launchd to restart it: the
job reported `immediate reason = ipc (mach)`, a new PID 88444, and both image
probes passed again. The reusable runner below exercises cold activation with
RunAtLoad=false and removes its own job on exit.

The earlier endpoint proposal in `docs/proposals/monado-xpc-service-name.patch`
is historical; its change is now applied, built and committed in Monado.

### Results on Apple M5, Wine 11.10, 2026-10-02

- Core gate: API 1.1 rejected; singleton limit, unknown-extension rejection,
  instance destruction and recreation passed.
- Khronos hello_xr D3D11, Local space, Opaque: successful bounded run with Metal
  validation. Both eye swapchains imported all four runtime-owned images.
- Runtime-image GPU probe: all four images of an 8x8 2D swapchain and both slices
  of all four images of a two-layer array swapchain passed exact pixel checks.
  It acquires/waits, clears through D3D11, signals/flushed an imported shared
  event, verifies pixels in the original runtime Metal object, and releases
  through the production OpenXR synchronization path. No Metal validation
  error was reported. Array projection rendering in hello_xr remains pending:
  this pinned hello_xr D3D11 plugin asserts imageArrayIndex==0.
- MWXRProbeSwapchainImage is a diagnostic export outside OpenXR GIPA, returning
  borrowed runtime objects while the swapchain is alive. GPU readback blits in
  the test helper inspect Private-storage images; they are absent from the
  production graphics path.
- Independent direct-object probe still covers descriptor mismatch, wrong object
  type and unsignaled-event timeout, as well as 2D/array pixel checks.
- OpenComposite/Underture, private game copy, 20-second bounded run: OpenVR
  initialized and bootstrap/session replacement completed, but no swapchain
  render was observed before termination. This is partial compatibility
  evidence, not working VR output. Original games and fallback paths unchanged.
- AlphaBlend hello_xr exited 1 with "Selected blendmode is not available from
  runtime". The simulated driver configures Opaque only; no existing blend-mode
  override was found. No further Monado change has been applied.

### Pacing hint unused: preliminary measurements

For the opaque hello_xr run, U_PACING_APP_USE_MIN_FRAME_PERIOD was absent from
both client and service environments. The app-pacer CSV contains 190 delivered
frames; exclude the first ten for these statistics. Its predicted period was
16,666,666 ns throughout. Submission/event intervals: median 16.278 ms, p95
21.053 ms. CPU/draw/GPU actual times seen by the pacer: medians
0.031/0.7545/1.0595 ms, p95 0.069/1.864/2.855 ms. None of the remaining 180
GPU-done events occurred after predicted display time. This deadline count is
not a measurement of late physical presentation. The raw evidence is
[hello-opaque-app-pacing.csv](results/in-process-2026-10-02/hello-opaque-app-pacing.csv).

A separate 200-empty-frame control probe used the Windows Khronos loader,
view-to-local xrLocateSpace, and inactive unbound actions. Excluding the first
ten frames, with 20 reads per type per frame:

| Call | Samples | Median (us) | p95 (us) |
| --- | ---: | ---: | ---: |
| xrLocateSpace | 3800 | 20.2 | 58.8 |
| xrGetActionStateBoolean | 3800 | 2.0 | 5.9 |
| xrGetActionStateFloat | 3800 | 1.7 | 4.5 |
| xrGetActionStateVector2f | 3800 | 1.7 | 4.5 |
| xrGetActionStatePose | 3800 | 1.6 | 4.4 |
| xrWaitFrame | 190 | 15653.6 | 21858.3 |

WaitFrame includes intentional pacing wait. Actions were unbound, so these do
not measure active controller input work. p95 uses nearest rank; timings use
QueryPerformanceCounter. The full local CSV is build-in-process/latency-in-process.csv;
[latency-summary.json](results/in-process-2026-10-02/latency-summary.json) records
results and method. Metal validation was enabled. No speedup or pacer-hint
conclusion is claimed before identical-workload proxy/native-host comparisons
and longer hardware runs. The reusable runner explicitly disables the service
hint to override any inherited launchd setting; the original hello run left it
unset.

### Repeat the isolated tests

From this checkout on this machine (paths refer to the supplied Monado workspace):

```zsh
export MONADO_SIM_BUILD=/Users/nickkennedy/Code/monado-2/.build/native-service-check
export MONADO_VULKAN_ICD=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json
export MWXR_NATIVE_RUNTIME_JSON=/Users/nickkennedy/Code/monado-2/.build/in-process-monado-x64/openxr_monado-dev.json
export MWXR_IN_PROCESS_PREFIX="$PWD/build-in-process/prefix-core"
scripts/run-in-process-simulated.zsh "$PWD/build-in-process/gate/in_process_swapchain.exe"
scripts/run-in-process-simulated.zsh "$PWD/build-in-process/gate/in_process_gate.exe"
scripts/run-in-process-simulated.zsh "$PWD/build-in-process/gate/in_process_latency.exe" \
  "Z:${PWD//\//\\}\\build-in-process\\latency-repeat.csv"
scripts/run-in-process-simulated.zsh \
  /Users/nickkennedy/Code/monado-2/build-wine-hello-xr/khr_hello_xr_d3d11.exe \
  --graphics D3D11 --space Local --blendmode Opaque --verbose
```

The runner refuses a service build with PS VR2/PS Sense enabled or unknown. It
creates a unique Mach endpoint/socket, launches on demand, records traces and
tears down only its job. Its logs remain under build-in-process/isolated-UID-PID.
Press a key to stop hello_xr. Build the latency executable with
scripts/build-in-process-latency.zsh and MWXR_WINDOWS_LOADER pointing at the
static Win64 Khronos libopenxr_loader.a used by the hello_xr build. Build the
image probes with scripts/build-direct-metal-probe.zsh.

### PS VR2 commands for the user

The native client and installed hardware service must use matching Monado
revisions, and the headset must be available. Do not use the simulated-service runner for hardware.
The hardware LaunchAgent configuration remains user-owned; the client-side
unset below does not change the service's pacing flags.

```zsh
export MWXR_NATIVE_RUNTIME_JSON=/Users/nickkennedy/Code/monado-2/.build/in-process-native-hardware/monado-x64/openxr_monado-dev.json
export MWXR_IN_PROCESS_PREFIX="$PWD/build-in-process/prefix-core"
unset XRT_MACOS_METAL_IPC_SERVICE_NAME XDG_RUNTIME_DIR U_PACING_APP_USE_MIN_FRAME_PERIOD
export MTL_DEBUG_LAYER=1
scripts/run-in-process-openxr.zsh "$PWD/build-in-process/gate/in_process_swapchain.exe"
scripts/run-in-process-openxr.zsh \
  /Users/nickkennedy/Code/monado-2/build-wine-hello-xr/khr_hello_xr_d3d11.exe \
  --graphics D3D11 --space Local --blendmode Opaque --verbose
# Repeat with client-side compositing, the Game Mode path:
XRT_MACOS_CLIENT_COMPOSITOR=1 scripts/run-in-process-openxr.zsh \
  /Users/nickkennedy/Code/monado-2/build-wine-hello-xr/khr_hello_xr_d3d11.exe \
  --graphics D3D11 --space Local --blendmode Opaque --verbose
scripts/run-in-process-openxr.zsh \
  "$PWD/build-in-process/games/Underture/Underture.exe" -force-d3d11 \
  -logFile "Z:${PWD//\//\\}\\build-in-process\\underture-hardware.log"
```

Record source commits, service environment, image probe output, visible behavior,
Metal validation output, game session transitions, app-pacer and presentation
CSVs. Compare all three paths with the same application, duration, compositor
mode, validation setting and service flags. Simulated metrics above cannot
replace this hardware comparison.

### PS VR2 hardware run, 2026-10-02

The user connected PS VR2 and explicitly authorized agent-run hardware tests.
macOS reported 4000x2040 at 120 Hz. Bridge tree: `4b86477`, DXMT: `f8e535b`;
installed ARM64 Monado service and the matching native x86_64 client:
`7fd7f2835693d447d46da933e9a54c9f71ddfae9` / v25.1.0-2051-g7fd7f2835.
The installed service, LaunchAgent configuration, fallback paths and Monado
sources were unchanged. The current ffec3b71a client was correctly rejected by
the older service; **IPC_IGNORE_VERSION was never enabled**.

The matching client source is the detached worktree
`.build/in-process-monado-hardware-source` in the supplied Monado workspace;
its build is `.build/in-process-native-hardware/monado-x64`. Monado's revision
helper produced -128-NOTFOUND for this worktree, so its existing `GIT_DESC`
CMake input was set to the independently verified git describe string of that
exact clean source revision. This corrects build metadata, not compatibility.
Use its `openxr_monado-dev.json` in MWXR_NATIVE_RUNTIME_JSON when testing the
currently installed service. The newer simulated-client manifest remains
appropriate only for its matching service.

Initial hardware startup failed setting the USB status interface to alt 1, then
claiming that interface. The user power-cycled/reconnected the headset/adapter;
the next startup selected PS VR2 and succeeded. No driver patch was needed.

**GPU result:** all four runtime-owned 2D images and both slices of all four
array images passed exact D3D11-written pixel checks, producer shared-event
synchronization and native release. The
[runtime image evidence](results/psvr2-in-process-2026-10-02/runtime-image-probe.txt)
contains all 12 checks. The diagnostic readback blit is test-only.

**Rendering/API result:** Opaque hello_xr ran for 45 seconds through the service
compositor and exited 0. It reached FOCUSED, selected Monado: PS VR2 HMD, reported
position/orientation tracking, and imported four 2800x2856 images per eye.
A second 30-second run with XRT_MACOS_CLIENT_COMPOSITOR=1 also exited 0, created a
CAContext, and logged a visible hosted front-end on PS VR2. This validates
in-process compositor initialization/presentation plumbing inside Wine.
Client Metal validation was enabled in both runs; no Metal validation assertion
was observed. MoltenVK's rejected high-priority queue request in the client
compositor fell back successfully. Service Metal validation was not enabled by
changing the registered job. Visual stereo/head-motion confirmation was requested
from the user and remains pending; successful submission is not proof of the
picture's quality. OpenComposite was not rerun in this hardware step.

**Pacing result:** U_PACING_APP_USE_MIN_FRAME_PERIOD was unset in clients and
absent from the registered service's explicit/inherited environment. Existing
service presentation/camera settings were preserved. These are sequential
service-vs-client compositor runs of the in-process bridge, not the requested
proxy-vs-native-host-vs-in-process comparison.

| Measurement | Service compositor | Client compositor inside Wine |
| --- | ---: | ---: |
| hello_xr delivered frames | 3213 | 1416 |
| Pacer predicted period | 8.342 ms | 16.667 ms |
| Submission interval median / p95 | 8.635 / 31.088 ms | 16.596 / 19.755 ms |
| Physical presented interval median / p95 | 91.757 / 108.441 ms | 16.683 / 16.684 ms |
| Physical intervals >1.5 predicted periods | 454 / 535 | 39 / 1385 |
| GPU-done after predicted display | 2729 / 3183 | 0 / 1386 |
| Compositor thread priority | 4 throughout | 97 throughout |

Thirty startup samples are excluded. For the service path, physical samples are
restricted to frame IDs with a nonempty submitted layer; for the client path,
they are restricted to the delivered-frame time range. Intervals use the
presented_monotonic_ns callbacks, not requested present timestamps. Callback
cadence does not establish motion-to-photon latency. The service trace shows a
priority clamp consistent with the known throttling problem, but the Game Mode
flag itself was not measured, so no cause is asserted here.

The hosted client's CVDisplayLink initialization and subsequent callbacks
explicitly reported 60 Hz despite the 120 Hz headset mode. Its stable 60 Hz
result **does not validate 120 Hz**. A standalone architecture comparison below
isolates this discrepancy to the translated x64 CoreVideo path on this machine.
No Monado timing fix or new hint was applied. These results do not yet justify
removing the minimum-period hint for other paths/workloads.

The existing 200-empty-frame latency probe also passed on PS VR2 in both
compositor modes. With the first ten frames excluded, median/p95 microseconds:

| Call | Service compositor | Client compositor |
| --- | ---: | ---: |
| xrLocateSpace | 23.4 / 56.1 | 59.0 / 87.7 |
| xrGetActionStateBoolean | 1.7 / 4.2 | 4.8 / 6.3 |
| xrGetActionStateFloat | 1.4 / 2.6 | 4.1 / 4.7 |
| xrGetActionStateVector2f | 1.4 / 2.7 | 4.0 / 4.8 |
| xrGetActionStatePose | 1.4 / 2.6 | 4.0 / 4.7 |
| xrWaitFrame | 7515.8 / 8785.5 | 15314.8 / 17462.0 |

Actions were inactive/unbound; WaitFrame includes the pacing wait. This control
probe does not reproduce the rendered hello_xr workload. Measurements, run
receipts and compressed raw pacing/presentation traces are in
[the hardware result directory](results/psvr2-in-process-2026-10-02).

For this installed service, correct the manifest in the hardware commands above:

```zsh
export MWXR_NATIVE_RUNTIME_JSON=/Users/nickkennedy/Code/monado-2/.build/in-process-native-hardware/monado-x64/openxr_monado-dev.json
```

### Why the hosted x64 compositor paced at 60 Hz

A standalone CoreGraphics/CoreVideo probe, with no Wine, DXMT or Monado loaded,
was compiled from the same C source for ARM64 and x86_64 and run against the
connected PS VR2 on 2026-10-02. Both selected display ID 3, 4000x2040, and read
120.000 Hz from CGDisplayModeGetRefreshRate. Results over 1.2 seconds:

| API/observation | ARM64 native | x64 under Rosetta |
| --- | ---: | ---: |
| CVDisplayLink nominal period | 200201 / 24000000 s | 16666666 / 1000000000 s |
| Nominal frequency | 119.880 Hz | 60.000 Hz |
| Observed callback frequency | 119.789 Hz | 59.982 Hz |

Thus Wine is not required to reproduce the 60 Hz display link. This is an
observed behavior of the translated x64 CoreVideo path on this Mac; it does not
establish a universal Rosetta limitation across macOS versions or displays.
Both nominal timing and actual callback cadence disagree with the real display
mode in x64, so this is more than an inaccurate FPS label or renderer overload.

Monado's macos_display_link_create takes the nominal CVDisplayLink period,
comp_window_macos_init assigns it to the compositor frame_interval_ns, and the
app pacer then predicts 16.667 ms periods. The realtime priority 97 compositor
therefore deliberately runs around 60 Hz. The callback source also ticks at
60 Hz, so changing a frame-period hint alone is not an established solution.
An alternative timing source needs validation before changing Monado.

Diagnostic source: tests/in_process/display_refresh_native.c. Evidence:
[ARM64 output](results/psvr2-in-process-2026-10-02/display-refresh-arm64.txt) and
[x64 output](results/psvr2-in-process-2026-10-02/display-refresh-x64.txt).
Reproduce without starting the hardware service or rendering:

```zsh
clang -arch arm64 tests/in_process/display_refresh_native.c \
  -framework CoreGraphics -framework CoreVideo -o build-in-process/display-refresh-arm64
clang -arch x86_64 tests/in_process/display_refresh_native.c \
  -framework CoreGraphics -framework CoreVideo -o build-in-process/display-refresh-x64
build-in-process/display-refresh-arm64
build-in-process/display-refresh-x64
```

### Underture startup repair, 2026-10-03

The user's black-screen / "Stub" / crash sequence occurred before rendering:
OpenComposite's log reported `xrCreateInstance = -51`. A core-gate reproduction
identified an IPC revision mismatch: the previous x64 native client was
`v25.1.0-2051-g7fd7f2835`, but the updated ARM64 service was
`v25.1.0-2073-g2276cfba9`. Rebuilt only the x64 native client from the current
Monado workspace (including its existing worktree changes), into
`.build/in-process-native-hardware-current/monado-x64`. The client and installed
service's generated IPC protocol headers have the same SHA-256:
`1fc0439adc286fb7f50ba0b9d962f6af862dae22c44e922188136e8566a509dc`.
The core gate then passed creation, HMD discovery, destruction and recreation.
No IPC version bypass or Monado source change was used.

That exposed a second failure: OpenComposite requests
`COLOR_ATTACHMENT | SAMPLED | TRANSFER_DST` for its eye swapchains, but the
prototype's subset rejected `TRANSFER_DST` with `XR_ERROR_FEATURE_UNSUPPORTED`
(-8). Permit that flag and preserve it in the native call. Unsupported
swapchain shapes/usages now log the rejected parameters. Updated the GPU probe
to request the same flags: all nine checks passed (three 2D images and three
2-slice array images) with Metal validation enabled. Image counts are supplied
by the runtime, not fixed by the bridge.

After both fixes, a bounded 40-second Underture run reached OpenVR initialization,
created both 2800x2856 eye swapchains, imported every runtime-owned Metal image,
and reached OpenXR FOCUSED. It remained running until the harness ended it.
No fatal OpenComposite abort or startup crash was reproduced. A non-fatal
unknown tracked-device string property 1001 warning remains. Visual confirmation,
longer gameplay, controller compatibility and matched performance measurements
are still pending; the compositor's CADisplayLink cadence is not an app FPS
measurement.

The zero-copy guarantee applies to the D3D11/Metal runtime boundary. This version
of [OpenComposite's D3D11 compositor](https://gitlab.com/znixian/OpenOVR/-/blob/a27e7e6a64bdcd1eff6b7fba1ea2ea34bcf1273d/OpenOVR/Compositor/dx11compositor.cpp)
copies or shader-draws the original OpenVR textures into the OpenXR swapchains.
Allowing transfer usage inserts no extra bridge copy, but this legacy OpenVR
application does not satisfy an end-to-end no-copy/no-blit requirement. That
would require a separate OpenVR submission adaptation.

Current local launch command (also selects the matching client for InMind):

```sh
bridge=/Users/nickkennedy/Code/monado-2/.build/in-process-openxr-study
export MWXR_NATIVE_RUNTIME_JSON=/Users/nickkennedy/Code/monado-2/.build/in-process-native-hardware-current/monado-x64/openxr_monado-dev.json
export MWXR_IN_PROCESS_PREFIX="$bridge/build-in-process/prefix-core"
export XRT_MACOS_CLIENT_COMPOSITOR=1
export MTL_DEBUG_LAYER=1
unset XR_RUNTIME_JSON XRT_MACOS_METAL_IPC_SERVICE_NAME XDG_RUNTIME_DIR U_PACING_APP_USE_MIN_FRAME_PERIOD IPC_IGNORE_VERSION
cd "$bridge/build-in-process/games/Underture"
"$bridge/scripts/run-in-process-openxr.zsh" "$PWD/Underture.exe" -force-d3d11 -logFile 'C:\openxr\underture.log' > "$bridge/build-in-process/underture-native.log" 2>&1
```

Rebuild the native x64 client whenever the service's source/protocol changes;
the manifest path above identifies a local build, not a permanently compatible
client. Original proxy/native-host prefixes and installed ARM64 service were
not modified. [Captured logs and summary](results/underture-in-process-2026-10-03/summary.json)
record the failures and successful initialization.

### Underture motion feedback, 2026-10-03

The user confirmed the repaired launch displayed the game. Subjectively FPS felt
high, but movement showed high-frequency judder, described as every other frame
jumping slightly backwards. This establishes visible output; it does not establish
smoothness, measured application FPS or controller compatibility.

The earlier agent-run 40-second initialization test used Metal validation and
included startup/session transitions. Eleven 240-interval presentation-worker
completion windows averaged 11.542 ms (86.64 completions/s); 1,120/2,640 intervals
(42.42%) exceeded 1.5 display periods. Window averages ranged from 9.159 to
15.085 ms, with individual maxima up to 28.344 ms. CADisplayLink generally tracked
8.342 ms / 119.88 Hz. These completion logs measure worker execution, not physical
drawable presentation or new application frames. They cannot provide median/p95,
identify alternating source-frame order, or characterize the user's separate run.
Do not label 86.64 as game FPS. The user's Unity log has 3,113 repeated FMOD-system
errors, another confound to investigate rather than an established judder cause.

Current native evidence already distinguishes presentation stalls from regular
source-refresh/parallax under rotational-only timewarp; see Monado's
`doc/macos-psvr2-judder-evidence.md`. The Underture report is consistent with a
pose/image timing or source-transition discontinuity, but is not proof of any
specific cause. A moving-head capture should correlate application frame IDs,
render poses, timewarp targets and physical presented timestamps, and explicitly
check alternation/order. No predictor, pacing or Monado source change is justified
by this feedback alone.

### Corrected Underture trace assessment, 2026-10-03

Normal traces **were captured** for the user's successful run in
`/tmp/monado_psvr2_77379_*.csv` (last modified about 21:42). The service identifies
PID 77379 as `OpenComposite_Underture`. The initial response overlooked these
files and used only presentation-worker console counters from the agent's
separate PID 76564 test. Those counters are not the user's performance result.
Archived the complete client trace set and a reproducible stdlib analyser under
[the user-run results](results/underture-in-process-2026-10-03/user-run/timing-summary.json).

The measurement window spans 59.876 seconds: the focused valid-source interval
with the first two and final one seconds excluded.

| Measurement | Result |
| --- | --- |
| Application frame deliveries | 3,160; 52.77/s |
| Application delivery interval median / p95 | 16.788 / 32.613 ms |
| Physical presentations | 3,746; 62.57/s |
| Physical interval median / p95 / max | 16.683 / 33.366 / 41.709 ms |
| Physical gaps exceeding 1.5 refreshes | 2,679 / 3,745 (71.54%) |
| Repeated source frame across physical presentations | 33.11% |
| Compositor GPU time median / p95 | 0.909 / 2.079 ms |
| GPU completion to physical output median / p95 | 38.421 / 57.535 ms |
| Physical output after compositor pose target median / p95 | 9.891 / 33.799 ms |
| Source predicted-time age at physical output median / p95 | 19.234 / 53.336 ms |
| Compositor or application source ID reversal in physical order | 0 / 0 |

Thus 119.88 Hz display-link callbacks do not establish 120 physical updates or
120 application FPS. This run shows uneven display cadence, repeated application
content, and physical presentation after the pose target. The short compositor
GPU time does not account for the later presentation delay; it does not establish
which presentation stage or process policy causes that delay. There was also a
688 ms application-delivery gap. The source-frame ID sequence rules out simple
old-frame/new-frame order reversal in the recorded physical output, but cannot
prove that every image matches its submitted render pose or identify the user's
subjective alternating backward step. Further analysis can start from these
existing traces; no fresh run is required just to establish the cadence failure.

### Underture lifecycle and scheduler diagnosis, 2026-10-03

Joined all 3,746 physical frames in the existing user-run measurement window.
[The lifecycle analysis](results/underture-in-process-2026-10-03/user-run/lifecycle-summary.json)
and compressed joined CSV are reproducible with `user-run/analyze-lifecycle.py`.
The 7,178 CA callbacks have interval median/p95 8.342/8.377 ms: the major display
cadence loss is later than these regular callbacks.

| Stage | Median / p95 (ms) |
| --- | --- |
| Renderer CPU entry to return | 0.806 / 1.389 |
| Present enqueue to worker start | 2.903 / 7.970 |
| Worker start to Metal commit | 0.129 / 0.283 |
| Metal commit to scheduled callback | 0.130 / 1.824 |
| Metal commit to completion callback | 1.431 / 3.435 |
| Completion callback to physical output | 38.354 / 57.426 |
| Drawable prefetch wait | 16.099 / 30.453 |
| Application semaphore readiness wait | 14.692 / 16.925 |

The app's reported GPU estimate median/p95 15.498/18.240 ms includes completion
and readiness latency; it is not an isolated hardware GPU execution measurement.
Drawable acquisition and after-completion display delay dominate this trace,
but waiting on drawables can also reflect earlier queued presentation work.
The timestamps do not prove which WindowServer/layer/presentation setting caused
the delay. These stage medians overlap and must not be added as a serial budget.

A concrete lifecycle problem is also visible. The first 33 scheduler samples
use time-constraint policy 2 / priority 97. At session recreation, compositor
frame ID resets to 1, and the remaining 7,674 samples use ordinary policy 1 /
priority 31 (all 7,186 steady-window samples). OpenComposite's user-run log at
21:41:03.602 explicitly recreates the session for the application graphics API;
the replacement session starts at 21:41:03.856. Monado creates a fresh multi-system
compositor thread for each replacement hosted compositor, but
`comp_multi_system_macos_trace.h` caches the configured display period in a
process-global static. The unchanged 8.341708 ms period skips applying Mach
policy on the replacement thread. The realtime trace's cached thread ID and
previous CPU/policy samples are also global, so the logged repeated thread ID
cannot establish thread continuity. Archived both original headers as evidence.

This is a specific correctness defect and strong performance candidate, not
proof that fixing it resolves all judder. Compositor/source IDs, compositor pose
target times and application predicted display times all remain monotonic in
physical order. New-source versus reused-source pose-target lateness medians are
10.191 versus 9.324 ms, so the trace does not establish a deterministic alternating
old/new timing split. Destination warp poses, sensor samples, service scheduling
policy and image/pose pixel correspondence were not captured in this client set.

Prepared an **unapplied**
[Monado compositor thread-policy patch](proposals/monado-compositor-thread-policy.patch):
make the scheduler period cache and per-thread realtime diagnostic state thread
local, and collect initial details for every new thread. Trace-file ownership and
row count remain process-global. `git apply --check` passes against the current
workspace. No Monado source modification or policy override was made. Under the
user's explicit Monado-change constraint, stop here for approval before applying.

After approval, build the x64 native client and ARM64 service targets without
replacing the installed service first; check session recreation retains policy
2 / priority 97, then perform the same bounded Underture trace comparison. Retest
physical cadence and pose-target delay before considering a separate presentation
or prediction change. Application rendering/DXMT completion can still limit app
FPS independently of compositor scheduling.

### Approved compositor thread-policy build, 2026-10-03

The user approved applying the proposed Monado patch and compiling the native
service/client and x86-64 client. Applied and committed the scheduling cache and
realtime diagnostic state correction as Monado `1b400a8e7` on the current
`codex/macos-shared-tracking` branch. Only those two headers and a new dedicated
build note were included; unrelated workspace changes remain separate.

Built `monado-service` and `openxr_monado` in
`.build/in-process-native-hardware-current/monado-arm64`, with ARM64 explicit and
PS VR2/PS Sense enabled. Built `openxr_monado` in
`.build/in-process-native-hardware-current/monado-x64`. Both use the current source,
including existing worktree changes; the x64 client picks up the current renderer
fix as well. After committing, regenerated/rebuilt both so all artifacts report
`v25.1.0-2074-g1b400a8e7`. Binary architectures and generated IPC-header hashes
match their intended configurations.

An extracted-source Mach policy probe passes on both ARM64 and translated x64:
three sequential replacement threads each apply a non-default time-constraint
policy once at the same refresh period. The old process-global-cache negative
control fails on its second thread with default policy and period zero. This is
initialization verification, not a new performance or motion-smoothness result.
[Build logs and probe evidence](results/compositor-thread-policy-2026-10-03/summary.json)
are archived; Monado's `doc/macos-compositor-thread-policy.md` describes the change.

No service installation/restart or hardware test was requested in this step.
The installed LaunchAgent still points to the older `build-wine` service. Select
the matching newly built ARM64 service before using either rebuilt client; do not
bypass the IPC revision check. The existing native manifests remain at their
respective `monado-arm64/openxr_monado-dev.json` and
`monado-x64/openxr_monado-dev.json` build paths.

### Desktop mirror launch options, 2026-10-03

The user confirms Underture and InMind run reasonably well and that the separately
corrected scanout-target pose bug noticeably improves motion. They requested
launching without each game's main-display mirror window.

Unity's [2018.3 player command-line reference](https://docs.unity3d.com/kr/2018.3/Manual/CommandLineArguments.html)
documents `-batchmode` for headless operation. In a bounded Underture run,
`-force-d3d11 -batchmode` retains D3D11, initializes OpenVR, creates both eye
swapchains and imports all their runtime-owned Metal images. Those checks did
not establish visible headset output: the user subsequently confirmed that
Underture's `-batchmode` also disables HMD rendering. InMind's older
Unity 5.3 player instead selects `NullGfxDevice` in batch mode, even with
`-force-d3d11`, and logs "VR rendering requires Direct3D11". Do not recommend
batch mode for either game or add `-nographics` to either game's VR launch.

For InMind, `MWXR_DESKTOP_MIRROR=offscreen` enables a small Windows child-process
launcher. It polls only the game's `UnityWndClass` windows and moves them
outside desktop bounds, preserving their visible state and D3D11 initialization.
It forces windowed mode and focuses the game; unrelated dialogs and native
Monado headset windows are not moved. A kill-on-close Windows job and Ctrl+C
handler stop the game with the launcher. The executable is built by
`build-in-process-gate.zsh`; without the opt-in, the original runner path is
unchanged. `MWXR_DESKTOP_MIRROR=window` explicitly selects that default path.

Verified the public runner with InMind: after an acknowledged trace flush,
1,353 compositor rows have valid sources, with 556 distinct application source
frames. macOS reports the InMind window at X=-16000, Y=-16032, outside the
connected displays. A separate off-screen Underture run also submitted 835
source frames. All diagnostic game/helper processes were stopped. These are
initialization/submission checks, not visual or performance comparisons.

Off-screen mode still renders the desktop mirror and can briefly show its window
before the launcher moves it. It is not a zero-cost mirror-disable option. Use
`MWXR_DESKTOP_MIRROR=offscreen` for **both Underture and InMind**, without
`-batchmode`. The user confirms this is required to preserve HMD rendering in
both games; this supersedes the earlier batch-mode recommendation for Underture. The legacy Alyx runner already uses `-nowindow`, but that is a Source 2
argument; no prior Unity no-window solution was found in the bridge scripts/notes.
[Logs and source-frame evidence](results/unity-desktop-mirror-2026-10-03/summary.json)
are archived.

Use the existing matching service/client builds and private prefix:

```sh
bridge=/Users/nickkennedy/Code/monado-2/.build/in-process-openxr-study
export MWXR_NATIVE_RUNTIME_JSON=/Users/nickkennedy/Code/monado-2/.build/in-process-native-hardware-current/monado-x64/openxr_monado-dev.json
export MWXR_IN_PROCESS_PREFIX="$bridge/build-in-process/prefix-core"
export XRT_MACOS_CLIENT_COMPOSITOR=1
unset XR_RUNTIME_JSON XRT_MACOS_METAL_IPC_SERVICE_NAME XDG_RUNTIME_DIR IPC_IGNORE_VERSION

# Underture: preserve headset rendering while moving the mirror off-screen.
cd "$bridge/build-in-process/games/Underture"
MWXR_DESKTOP_MIRROR=offscreen "$bridge/scripts/run-in-process-openxr.zsh" "$PWD/Underture.exe" -force-d3d11 -logFile 'C:\openxr\underture-offscreen.log'

# InMind: preserve graphics while moving the mirror off-screen.
cd "$bridge/build-in-process/games/InMind"
MWXR_DESKTOP_MIRROR=offscreen "$bridge/scripts/run-in-process-openxr.zsh" "$PWD/InMind.exe" -force-d3d11 -logFile 'C:\openxr\inmind-offscreen.log'
```

Choosing `MWXR_DESKTOP_MIRROR=window` for either game restores the previous
desktop-window launch. Keep `-batchmode` absent in both cases. No Monado source or
Wine/DXMT modification was needed.

### Next blocking acceptance requirement

AlphaBlend against the simulated HMD needs a small, opt-in simulated-driver
change. The concrete unapplied candidate is
[monado-simulated-alpha-blend.patch](proposals/monado-simulated-alpha-blend.patch):
SIMULATED_ALPHA_BLEND=1 appends AlphaBlend to the simulated HMD's existing Opaque
mode list; unset keeps current behavior. It changes no hardware driver or
compositor. Under the user's Monado-change constraint, stop for review before
applying this separate change. Alpha blending, array projection, rendered
OpenComposite frames, matched three-path latency/pacing comparison and full-rate/visual hardware
validation remain pending.

### Local commits and provenance

Bridge branch: codex/in-process-wine-openxr. Phase 0: 3bc1815; core gate/licensing:
46a67f6; direct-object probe: 2226aa7; D3D11 graphics adaptation: c3a2f43.
Runtime-owned image verification and the 1.0-only gate: f141dda. DXMT direct import: f8e535b on
codex/in-process-metal-import. Monado endpoint change: ffec3b71a on
macos-upstream-clean, preserving unrelated local documentation/test edits.
No branch has been pushed.

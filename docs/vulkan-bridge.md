# Windows Vulkan to native Metal OpenXR

2026-11-12. A direct Vulkan graphics path is implemented in wineopenxr, alongside
the existing D3D11 path. The private build used for validation is
`build-in-process/vulkan-gate`. The default installed gate and KK sources were
not replaced.

## Path

```text
Windows Vulkan application (or OpenVR through xrizer's Vulkan backend)
  -> Wine Vulkan API -> KosmicKrisp -> imported runtime-owned Metal textures
  -> native Metal OpenXR session -> Monado
```

The bridge introduces no image copy and creates no D3D11 device for a Vulkan
session. OpenVR still needs xrizer's normal eye copy from the game's image into
the OpenXR swapchain. xrizer may create a temporary D3D11 session before the
first Vulkan submission; its real Vulkan session uses this path.

`XR_KHR_vulkan_enable` provides graphics requirements, instance/device extension
strings, physical-device selection, session binding and swapchain images.
`XR_KHR_vulkan_enable2` also provides the Vulkan instance/device creation
helpers and their requirements/device queries. The device helper adds
`VK_EXT_metal_objects` while preserving the application's other extensions,
features and allocation callbacks.

All Vulkan operations execute in PE through Wine's Vulkan API. Wine-wrapped
Vulkan handles never enter native OpenXR. Device selection probes Metal-capable
Vulkan adapters and compares the exported Metal device's registry ID with the
device required by Monado. Session creation checks the selected physical device
and queue family. Runtime Metal images are imported with
`VkImportMetalTextureInfoEXT`, retained until swapchain teardown, and returned
as `XrSwapchainImageVulkanKHR` objects.

## Completion and current limits

**Completion is ordered on the GPU (2026-11-13).** The native session's
`MTLSharedEvent` (the one the D3D11 path signals from DXMT) is imported into a Vulkan
timeline semaphore (`VkImportMetalSharedEventInfoEXT`, which KosmicKrisp supports). On
release the bridge submits an empty batch to the application's queue that signals the
next value, without waiting; the native release command buffer encodes a GPU wait for
that value before Monado's own work, exactly as for D3D11. Everything the application
submitted earlier on that queue (its eye rendering or xrizer's copy) is ordered before
the signal by Vulkan's submission order. `MWXR_VULKAN_SYNC=fence` restores the CPU fence
wait, which is also the fallback if the semaphore cannot be created. Swapchain and
session destruction still wait on the CPU.

The fence wait ran the game's CPU and GPU work back to back. Talos on the
high-resolution simulator (eye 2800×2856, start screen): frame period 34.2 ms with the
fence wait, 18.4 ms with GPU ordering (17.5 ms of GPU work per frame); captured eye
images match. NMS's menu is unchanged pixel for pixel.

The bridge accepts single-sample, one-mip, non-cubemap colour swapchains, with
one or multiple array layers. Current format mappings are RGBA8 UNORM/sRGB,
BGRA8 UNORM/sRGB and RGBA16 FLOAT. Depth, storage-image swapchains, multisampled
runtime swapchains and additional composition-layer types remain outside this
bridge's existing support. Games can render to their own MSAA targets and
resolve before submission.

The Vulkan path does not yet wire KK implicit foveation selection or gaze.
Imported runtime images remain ordinary. Existing D3D11 foveation is kept in
its own path.

## Build and select

Prepare the private Wine/font and xrizer/queue fixes with
`zsh scripts/build-vulkan-test-runtime.zsh`. This uses APFS copies and leaves
the installed Wine and xrizer untouched.

For a headset test on this rig, start macOS Steam and the usual Monado hardware
service, then run `zsh scripts/test-vulkan-game.zsh talos` or
`zsh scripts/test-vulkan-game.zsh nms`. This helper selects the private bridge,
leaves foveation disabled, and prints its timestamped log directory. It does
not start a simulator or replace the default gate.

Use the environment described in [in-process-openxr.md](in-process-openxr.md)
for the Wine SDK, native loader and OpenXR SDK, then:

```sh
MWXR_IN_PROCESS_BUILD="$PWD/build-in-process/vulkan-gate" \
  scripts/build-in-process-gate.zsh
```

The build needs Khronos Vulkan headers. `VULKAN_HEADERS_DIR` is their include
root (containing `vulkan/`), defaulting to `/opt/homebrew/include`. It also
builds `openxr_vulkan_probe.exe` and `openvr_vulkan_probe.exe`. The OpenVR
probe additionally needs an installed game's `openvr_api.dll` next to it and the
usual xrizer provisioning.

The repository launcher accepts:

```sh
export MWXR_IN_PROCESS_BUILD="$PWD/build-in-process/vulkan-gate"
export MWXR_VULKAN=kosmickrisp
export MWXR_KK_ROOT="$PWD/build-in-process/zink-fov"
```

The KK root must contain `lib/libvulkan.1.dylib` and
`icd/kosmickrisp_x86_64.json`. This selects KK without installing or enabling
Zink's OpenGL DLLs. Keep the usual Wine/runtime/prefix variables when invoking
`scripts/run-in-process-openxr.zsh` or `scripts/run-windows-game.zsh`.
The machine's `~/Windows/run-windows-game.zsh` wrapper currently pins the
default gate, so setting the private build requires the repository launcher or
an adjusted wrapper. This implementation does not change that external file.

With a matching Monado service and the private environment selected, run:

```sh
scripts/run-in-process-openxr.zsh \
  "$MWXR_IN_PROCESS_BUILD/openxr_vulkan_probe.exe"
scripts/run-in-process-openxr.zsh \
  "$MWXR_IN_PROCESS_BUILD/openxr_vulkan_probe.exe" --vulkan2
```

## Validation

Both legacy Vulkan and enable2 probes passed on the simulated Monado headset:

- Physical-device matching and Vulkan session creation.
- One-layer and two-layer imported Metal swapchains, including capacity queries.
- Vulkan clear/copy/readback with every RGBA pixel checked after bridge release.
- Stereo projection frame submission through the common bridge path.
- Missing graphics-requirements call, invalid queue family, unsupported MSAA
  swapchain and duplicate release checks.
- Runs with Metal API validation enabled; legacy Vulkan 1.0 and enable2
  Vulkan 1.1 instance creation.

Logs: `build-in-process/game-checks/20261112-224358-vulkan10-final`
(Vulkan 1.0), `20261112-223908-vulkan-final-legacy` (Vulkan 1.1), and
`20261112-223934-vulkan-final-vulkan2` (enable2). KK logs a tolerated missing Metal
ShaderWrite usage bit on these imported render targets; pixel checks pass.

No Man's Sky no longer reproduces `KHR_vulkan_enable not loaded` and gets
further into VR initialisation, but the bounded test did not reach a Vulkan
submission. Talos's Vulkan test still stalled before graphics initialisation.
These launches do not establish game compatibility. The Mac was locked during
the tests, preventing inspection of any game dialogs. See
[the game assessment](vulkan-games-foveation-assessment.md) for earlier evidence.

The existing D3D11 foveation probe also passed against the matching private
foveation-enabled Monado builds, including two instance/session lifecycles,
map updates, disable/re-enable, pixel checks and stereo projection submission.
Log: `build-in-process/game-checks/20261112-224218-vulkan-d3d11-matching-fov`.
The 12 launch-setting unit tests and shell syntax checks passed.

The OpenVR-level probe also exposed an existing xrizer limitation:
`ColorSpace_Linear` Vulkan submissions panic in its
`get_colorspace_corrected_format` function. The probe uses `ColorSpace_Auto`
to exercise the supported route. The bridge itself accepts linear UNORM/FLOAT
formats; this xrizer limitation is independent of its OpenXR Vulkan support.

The OpenVR probe passed eight stereo frame submissions with automatic colour
space under Metal validation. Its log confirms xrizer's transition from the
temporary D3D11 backend to a real Vulkan session and a zero-copy imported
two-layer sRGB runtime swapchain. Log:
`build-in-process/game-checks/20261112-224651-openvr-vulkan-auto`.

## Headset follow-up, 2026-11-12

Talos reached its first Vulkan Submit, but supplied `UINT32_MAX` as the queue
family index. Stock xrizer indexed a one-element array with it. The private
xrizer patch recovers only a single graphics family with one queue, verifies
that queue's identity against the submitted handle, and saves the resolved
family for session and command-pool creation. Other ambiguous/incorrect
identities are not guessed. The OpenVR probe reproducing this invalid index
passed eight stereo frames with the patch. Log:
`build-in-process/game-checks/20261112-234512-openvr-unknown-family-fixed`.

The empty Windows dialogs were traced to a malformed FreeType soname in
`runtime-lsteamclient/lib/wine/x86_64-unix/win32u.so`. The original `runtime`
and `runtime-wow64` already have the corrected name; the selected x64
lsteamclient and foveation copies did not. A private APFS copy at `vulkan-wine`
corrects only that library-name string and passes a text metrics/glyph test.
`test-vulkan-game.zsh` now defaults to this Wine and the private patched xrizer.

NMS's warning text was captured in a disposable prefix: **Graphics Driver
Compatibility Warning** stating that the Vulkan driver for Apple M5 does not
support wide lines, with one **OK** button. No xrizer panic was present at that
point. The warning can be acknowledged; the test did not click it or claim
that NMS renders afterward. Log:
`build-in-process/vulkan-game-inspection/nms-dialog-text/game.log`.
Talos also entered its shipped safe-mode script, which fails on the obsolete
read-only `gfx_iAPI` assignment; the font fix makes its dialogs readable but
does not change that game file.

The queue-recovery unit test passed under Wine, covering the matching sole
queue, a mismatched queue, multiple families, multiple queues, and valid hints.
Result: `build-in-process/vulkan-game-inspection/xrizer-queue-tests.log`.

## Startup crashes fixed and launcher integration, 2026-11-13

Both games now start and run on the simulated headset through this path, to Talos's
VR start screen and NMS's main menu (frames captured from both eyes; neither has yet
been played past that screen).

- **Talos: null query pool.** KosmicKrisp backs every occlusion query with one
  device-wide visibility buffer of 32,768 slots (Metal's 256 KB limit). Talos creates
  four pools of 16,384; the third and fourth failed, Talos ignored the error and
  called `vkGetQueryPoolResults` on a null handle, and Wine's thunk asserted
  (`!status && "vkGetQueryPoolResults"`), ending the process with no crash report.
  The private KosmicKrisp (`patches/kosmickrisp/0002-occlusion-query-overflow.patch`)
  now gives such a pool its own storage: its queries count nothing and report
  "visible", so occlusion culling is simply skipped for them. NMS creates a 65,536
  pool and takes the same fallback. The crash also left Talos's `Temp/run.txt`
  marker, so the next start stopped at an invisible "start in safe mode?" prompt;
  deleting the marker clears that.
- **NMS: wide-lines warning.** NMS waits on a message box when the driver lacks
  `wideLines`. `KK_FAKE_WIDE_LINES=1` (`0003-fake-wide-lines-and-cache-uuid.patch`) claims them;
  lines are still drawn one pixel wide, as Metal has no wide lines.
- **Integration.** `scripts/steam-play-game-settings.zsh` selects this path for
  Talos (552440) and NMS (275850): `MWXR_VULKAN=kosmickrisp`, the private
  KosmicKrisp (`build-in-process/zink-fov`), `vulkan-wine`, `vulkan-xrizer`, the
  combined bridge `build-in-process/vk-fov-gate` (D3D11, Vulkan and KosmicKrisp
  gaze in one build) and, for NMS, `KK_FAKE_WIDE_LINES=1`. `MWXR_VULKAN=off` in a
  game's Launch Options keeps the ordinary setup. `~/Windows/run-windows-game.zsh`
  now keeps an xrizer and bridge chosen by these settings (it overwrote them).
- **Launch Options.** Talos needs `+gfx_strAPI Vulkan` (otherwise it runs D3D11 on
  DXMT); NMS was tested with `-HmdEnable 1`.
- **Gaze.** `MWXR_FOVEATION=gaze|fixed` on a Vulkan game now takes the KosmicKrisp
  route (no DXMT runtime needed): end-frame records the projection swapchain and
  rectangles, and each acquire sends the gaze profile to KosmicKrisp
  ([kk-implicit-foveation.md](kk-implicit-foveation.md)).
- **Capture.** `MWXR_CAPTURE_DIR=dir MWXR_CAPTURE_EVERY=n` writes the released
  swapchain image (each layer) as PNG every n frames, up to six, for Vulkan sessions.

Simulator runs: `build-in-process/kk-fov-tools/vk-game.zsh talos|nms TAG SECONDS`.
Logs and captures are under `build-in-process/game-checks/` (`*-vk-talos6`,
`*-vk-nms3`, `vkcap-*`).

## Into play on the simulator, 2026-11-13

**Talos** starts straight into a level with
`+prj_bStartMainMenu 0 +gam_strLevel Content/Talos/Levels/Cloud_1_01.wld` (after
`+gfx_strAPI Vulkan`); no input is needed.

**No Man's Sky** has no such option. The private simulator runner passes
`QWERTY_SCRIPT` (Monado's timed desktop input; a left click is the focused controller's
trigger) and turns on the qwerty controllers. `QWERTY_SCRIPT="20+left,23-left"
QWERTY_SCRIPT_REPEAT=20` holds the trigger for 3 s every 20 s, which walks a fresh
profile through Play, game mode and into a new game on a planet. The qwerty devices
use the simple-controller profile; NMS's menus still respond to it through xrizer.

**NMS fiber crash (Wine on macOS).** Starting a new game crashed every job thread in
`SwitchToFiber(0x8ff)`. NMS records its current fiber with an inline
`GetCurrentFiber()` (`mov %gs:0x20`, two places in NMS.exe) before switching to a job
fiber, and the job fiber switches back to that value. On macOS `%gs:0x20` is the
system's thread-specific-data slot for the thread's QoS class, not Wine's
`NT_TIB.FiberData`, so NMS read 0x8ff. Writing the fiber there would corrupt macOS's
QoS, so the private Wine's `kernelbase.dll` (`patches/wine/0001-kernelbase-fiber-gs-slot-macos.patch`,
built from `~/Code/wine-crossover/build`'s objects into `build-in-process/wine-fiber-fix`
and installed in `vulkan-wine`; the original is kept as `kernelbase.dll.orig`) records
which fiber last switched to each fiber; a switch to exactly the raw `%gs:0x20` value
(below 0x10000, not a fiber) goes back to that one. Both NMS sites use that pattern.
Logged once: `fiber 0x8FF is the macOS %gs:0x20 value; switching back to ...`. This
affects NMS in flat mode too, and any Windows program that reads the current fiber
inline.

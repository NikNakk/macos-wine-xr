# Talos VR and No Man’s Sky: KK foveation assessment

2026-11-12. Inspection of the installed Windows builds, current bridge and
xrizer sources, plus bounded launches against a private simulated headset.
Follow-up: the direct Vulkan bridge is now implemented and passes synthetic
swapchain/frame tests; see [vulkan-bridge.md](vulkan-bridge.md). The findings
below describe the initial investigation before that implementation.

Neither game has been validated with KK foveation. No production graphics code
was changed for this investigation.

## Finding

The KK work is reusable, but Windows Vulkan OpenVR submissions need a working
presentation bridge first. The current Windows xrizer Vulkan backend uses an
OpenXR Vulkan session; wineopenxr here exposes D3D11 backed by native Metal.
The OpenGL backend already avoids this problem by importing the application's
Metal texture into a D3D11 session. There is no equivalent Vulkan-source mode.

No Man’s Sky reproduced this gap: its call to
`GetVulkanInstanceExtensionsRequired` panics in xrizer with
`KHR_vulkan_enable not loaded`, before a game frame is rendered.
Talos's Vulkan attempts stalled earlier in startup, so that specific failure
has not been reproduced in Talos.

## Shared work needed

1. Provide a Windows Vulkan OpenVR presentation route. One candidate is a
   Vulkan-source D3D11 mode in xrizer, analogous to `d3d11_gl.rs`: export the
   submitted VkImage's Metal texture, import into DXMT, then copy the submitted
   eye region into the ordinary OpenXR swapchain. Handle formats, layers,
   multisampling, inverted bounds and synchronization explicitly. The existing
   Wine `VK_EXT_metal_objects` work supplies part of the export mechanism;
   exportability of the games' images still needs verification.
2. In that route, change xrizer's Vulkan extension and output-device queries
   as well as its Submit dispatch. Merely adding a texture copy is insufficient:
   `GetVulkanInstanceExtensionsRequired`, `GetVulkanDeviceExtensionsRequired`
   and Vulkan `GetOutputDevice` currently depend on OpenXR Vulkan functions.
   An alternative is full Vulkan-to-Metal interop in wineopenxr, which would
   preserve xrizer's existing backend but requires a broader bridge addition.
3. Select the patched KK driver explicitly for Vulkan games. Currently the
   launcher installs the Khronos loader/KK environment under
   `MWXR_OPENGL=zink`. The tests used that switch to select KK; a dedicated
   Vulkan driver selector would avoid tying Vulkan launches to OpenGL setup.
4. Once unfoveated VR frames work, trace image creation, render attachments,
   sampling, storage-image accesses, copies/resolves and Submit bounds. Use
   those results to select the scene targets and establish where to return to
   ordinary coordinates before runtime submission.

Source points: `src/in_process/graphics.cpp` (`xrCreateSession`),
`scripts/run-in-process-openxr.zsh`, and, in `~/Code/xrizer`,
`src/graphics_backends.rs`, `src/graphics_backends/vulkan.rs`,
`src/graphics_backends/d3d11_gl.rs`, `src/compositor.rs` and `src/system.rs`.

## The Talos Principle VR

The installed executable identifies itself as build 443779 (2019-06-28),
includes Vulkan support and requests `IVRCompositor_022` / `IVRSystem_019`.
It also supports D3D11: the first test successfully created a D3D11 VR session
and reported the simulated Monado HMD. The numeric `+gfx_iAPI 2` argument did
not select Vulkan. Later tests used its string selector `gfx_strAPI` with
Vulkan, but stalled before graphics initialisation; quoting the value and
disabling the generated user startup script did not resolve that within the
test window. These are startup observations, not evidence of a KK rendering
failure.

The shipped VR quality presets establish a useful distinction:

| VR preset | `gfx_iAntiAliasingSamples` |
| --- | --- |
| QL1–QL3 | 1 (no MSAA) |
| QL4 | 2 |
| QL5 | 4 |

The menu exposes 1, 2, 4 and 8 samples. Thus Talos is a plausible next test of
the existing MSAA-based KK prototype **with MSAA enabled**, once Vulkan startup
and presentation work. Without MSAA its scene targets will not satisfy the
prototype's image selector. Separate-eye versus packed versus array layout,
actual selected sample count and resolve topology remain unmeasured. Do not
assume Earth VR's two side-by-side cores are appropriate.

Talos's D3D11 renderer is also an available route for separately testing the
existing DXMT foveation work; that does not establish Vulkan compatibility.

## No Man’s Sky

Installed Steam build ID: 26540391. The binary requests
`IVRSystem_026` / `IVRCompositor_029`; the live failure occurs in the implemented
Compositor029 Vulkan-extension query, rather than an unknown-interface error.
Launch used `-HmdEnable 1` and stopped at the missing OpenXR Vulkan extension
described above.

The executable contains `VRFoveatedRendering`, `VREyeTracking`,
`HmdFoveated`, `DrawFoveationMap`, `UpdateFoveatedRendering`,
`VK_KHR_fragment_shading_rate` and
`vkGetPhysicalDeviceFragmentShadingRatesKHR`, plus temporal-AA and FSR code.
These are evidence of included functionality, not proof that the Windows VR
path enables it or that it is compatible with KK. The inspected KK extension
table does not advertise `VK_KHR_fragment_shading_rate`; its experimental
fragment-density-map mechanism is a different API.

After fixing presentation, assess two routes:

- Check whether NMS's own Windows VR foveation can be enabled and what fragment
  shading rate features it actually requires. Supporting that API in KK is a
  possible separate project; existing FDM support does not automatically do so.
- For implicit KK foveation, first measure the scene-target topology. If the
  relevant targets are single-sample, extend selection and the ordinary-image
  output boundary beyond the prototype's MSAA resolve route. Audit any temporal
  history, motion-vector, depth and storage-image/compute operations that touch
  warped targets. The current descriptor lowering translates sampled/fetched
  texture coordinates; its storage-image intrinsic lowering does not provide
  the equivalent rate-map coordinate conversion.

## Evidence and limits

Diagnostic outputs are under `build-in-process/game-checks/`:

- `20261112-221723-talos-vulkan-inspect`: actual D3D11 baseline.
- `20261112-221815-nms-vulkan-inspect`: reproduced Vulkan-extension panic,
  particularly `game/xrizer/xrizer.txt`.
- `20261112-221906-talos-vulkan-confirm` and
  `20261112-222000-talos-vulkan-quoted`: bounded startup stalls.

Capability strings and copied Talos engine logs are preserved under
`build-in-process/vulkan-game-inspection/`. Tests used per-game prefixes;
the runner stopped only the test game's prefix and its private simulator.
Launching Talos created its first-run files. No eye-layout claim, image-quality
comparison, performance result or working gaze integration follows from these
tests. Resolve the shared presentation gap before claiming compatibility.

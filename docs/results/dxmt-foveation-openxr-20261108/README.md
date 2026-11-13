# Target-specific DXMT / OpenXR foveation proof

2026-11-08. See [implementation and API contract](../../dxmt-foveation.md).

## Results

All runs used the private Wine/DXMT runtime and Metal API validation.

| Check | Result |
| --- | --- |
| Target-specific maps, reversed draw order, ordinary control | 100 pixel checks passed |
| Invalid map object and wrong logical map dimensions | Rejected; existing binding preserved |
| Original context-wide map proof | 100 pixel checks passed |
| Targeted negative control (maps deliberately omitted) | Pixel mismatch detected; expected exit 1 |
| Stereo OpenXR proof | Two instance/session lifecycles; 900 pixel checks passed |
| Extension not enabled | Companion function unavailable |
| Release before apply; duplicate apply | Call-order errors, as required |
| Eye-tracked profile on a system with no eye device | Explicit unsupported result; fixed profile preserved |
| Ordinary unfoveated OpenXR swapchains | All runtime-owned 2D/array images and slices passed |
| DXMT backend with older headers lacking the new interface | Compiled successfully |
| Compositor consumption | Traces include two foveated views |

The stereo proof uses two separate 512×512 eye swapchains. It acquires both
eyes, selects both maps, then records eye/control draws. High-strength fixed
profiles move the foveal region with opposite ±15° vertical offsets, alternating
between frames and eyes. Frame 2 disables foveation; later frames restore it.
The native map yields rasterized extents of 288×284 or 288×288, versus 512×512
when disabled. Native pixel checks use the exact map's coordinate conversions,
including for private runtime textures. All four images of each eye's swapchain
are exercised, followed by image reuse.

`openxr-positive.log` contains the actual checks. `manifest.json` records build
revisions, patch hashes and trace summaries. `traces/` contains compressed
compositor/client timing CSVs from the passing OpenXR proof.

This is a simulator correctness proof. Real eye motion, tracking-loss recovery,
compositor output pixel correctness and performance have not been validated
through the new Wine path. The missing-eye-device check is not a substitute
for tracking-loss validation on an active tracker. EXD remains unchanged.

## Private build configuration

The selected installed x86_64 client had Metal/META foveation disabled. A new
private x86_64 client was built from the existing Monado source, preserving its
reference build's public XRT options and Vulkan dependency selection, with:

```text
XRT_FEATURE_OPENXR_FB_SWAPCHAIN_UPDATE_STATE=ON
XRT_FEATURE_OPENXR_FB_FOVEATION=ON
XRT_FEATURE_OPENXR_FB_FOVEATION_CONFIGURATION=ON
XRT_FEATURE_OPENXR_META_FOVEATION_EYE_TRACKED=ON
XRT_FEATURE_OPENXR_MNDX_FOVEATION_METAL=ON
```

The existing ARM64 service was five commits behind that source; version checks
were retained. A matching private ARM64 service was built with the simulated
driver enabled and PS VR2, PS Sense and Rift drivers disabled. The runner
checks those driver gates and forces the offscreen debug-image target. Its
private Mach endpoint and Unix socket are removed after the run. The installed
service and launchd preferences are not altered.

Private outputs are `build-in-process/foveation-monado-x64`,
`foveation-monado-arm64`, `foveation-gate`, and `wine-foveation`.
The test prefix is `build-in-process/prefix-foveation`.

After applying both DXMT patches and building the private runtime, build the
bridge and probes (paths below are this machine's existing SDK/toolchains):

```sh
export DXMT_SOURCE_DIR="$PWD/build-in-process/dxmt-foveation-source"
export MWXR_WINE_SDK="$HOME/Code/wine-crossover/runtime"
export MWXR_WINE_SOURCE="$HOME/Code/wine-crossover/sources/wine"
export MWXR_WINE_RUNTIME="$PWD/build-in-process/wine-foveation"
export MWXR_NATIVE_LOADER="$PWD/build-in-process/openxr-loader-x64/src/loader/libopenxr_loader.dylib"
export OPENXR_SOURCE_DIR="$HOME/Code/OpenXR-SDK"
export MWXR_IN_PROCESS_BUILD="$PWD/build-in-process/foveation-gate"
scripts/build-in-process-gate.zsh
MWXR_FOVEATION_BUILD="$MWXR_IN_PROCESS_BUILD" scripts/build-dxmt-foveation-probe.zsh
```

The private prefix must have been initialized by the standalone proof first.
Run the OpenXR proof against the matching private builds:

```sh
MONADO_FOVEATION_SERVICE="$PWD/build-in-process/foveation-monado-arm64/src/xrt/targets/service/monado-service" \
MONADO_VULKAN_ICD=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json \
MWXR_NATIVE_RUNTIME_JSON="$PWD/build-in-process/foveation-monado-x64/openxr_monado-dev.json" \
MWXR_IN_PROCESS_WINE="$PWD/build-in-process/wine-foveation" \
  scripts/run-openxr-foveation-probe.zsh
```

The initial DXMT build reused unchanged compiled Metal shader helpers because
the downloadable Xcode Metal compiler was unavailable. All changed PE and
native code was rebuilt; see the first-stage shader-cache notes. No new shader
source was introduced by either DXMT patch.

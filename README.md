# macOS Wine XR

Bridge code and tooling for running Windows XR applications under Wine on macOS against native macOS OpenXR runtimes.

The repository split is intentional:

- **Monado** contains generic macOS/Metal runtime, compositor and IPC support.
- **DXMT** owns D3D-to-Metal translation and exposes generic native-sharing metadata.
- **macOS Wine XR** owns Wine transport, DXMT consumption, Windows OpenXR/OpenVR glue, launch tooling and compatibility policy.

## Current architecture

```text
Windows OpenXR application / D3D11
  -> thin Win64 OpenXR thunk
  -> authenticated generic macos-wine-xr RPC
  -> native OpenXR host
  -> native Khronos loader
  -> Monado / Meta XR Simulator / future Metal OpenXR runtime
```

DXMT wraps exported native textures as D3D11 resources. Shareable runtime-owned
images use zero copy; unshareable images receive one Metal GPU blit per image.
The same Win64 DLL has run Khronos `hello_xr` against simulated Monado and
Meta XR Simulator (opaque composition). See
[build/run instructions, evidence and limitations](docs/native-openxr-backend.md).

The original authenticated Monado byte-stream proxy and transitional frontend
remain available as a regression path. DXMT has no Monado dependency; clean
Monado has no Wine/DXMT transport code.

## Active development branches

- Monado integration/reference: `NikNakk/monado:macos-game-mode-upstream-sync-2026-10`
- Monado upstream-oriented cleanup: `NikNakk/monado:macos-upstream-clean`
- DXMT native-sharing work: `NikNakk/dxmt:macos-xr-native-sharing`
- Bridge: this repository, `main`

## Transitional path (retained)

The current path includes:

- consumer-neutral DXMT texture/fence private-data GUIDs;
- shared-Metal transport for both ordinary 2D and Texture2DArray D3D11 swapchains;
- DXMT shared-fence / MTLSharedEvent synchronization;
- a native bootstrap-name resolver;
- publication into Monado's PID-scoped Metal XPC broker;
- a standalone authenticated Wine TCP -> native Monado Unix-IPC proxy;
- proxy-owned minimum-frame-period pacing policy;
- proxy-owned shared-memory snapshots and layer uploads, translated into native
  shared-memory slots and `layer_sync` / `layer_sync_with_semaphore`;
- a bridge-owned D3D11 client compositor and OpenXR D3D requirements helper;
- current-DXMT provisioning/build scripts;
- native-sharing probes; and
- an end-to-end Khronos hello_xr runner.

The Windows OpenXR frontend is currently transitional: it is built in a
disposable private checkout of the last integrated Wine-client Monado branch,
with the live bridge sources overlaid at build time. The native runtime is
`macos-upstream-clean`.

## First end-to-end test

Configure/build `macos-upstream-clean`, then from this repository:

```sh
export MONADO_SOURCE_DIR=/path/to/monado
export MONADO_BUILD_DIR=/path/to/monado/build
MWXR_BOOTSTRAP_SERVICE=1 scripts/run-hello-xr-current-dxmt.zsh
```

`MWXR_BOOTSTRAP_SERVICE=1` deliberately replaces the currently loaded
development Monado LaunchAgent with the clean build. Omit it if the correct
service is already registered.

For the lower-level current-DXMT resource test:

```sh
scripts/run-current-dxmt-sharing-probe.zsh
```

## Upstreaming rule

No upstream pull requests are created automatically. Work remains on the
user-owned forks until explicitly requested.

## Transitional build framework

The target path no longer uses the pinned DXMT v0.80/Basalt patches embedded in
Monado. Development now uses:

- `NikNakk/dxmt:macos-xr-native-sharing` for current DXMT and generic native
  texture/fence metadata;
- `NikNakk/monado:macos-upstream-clean` for the native macOS runtime;
- the standalone authenticated proxy in this repository for the Wine byte
  stream, DXMT bootstrap resolution and Monado XPC token handoff.

The old integrated branch remains useful as a reproducibility reference and as
a temporary build framework for the Win64 OpenXR frontend, but it is not the
native runtime used by the new path.

### Native-sharing proof

On an Apple Silicon Mac with Homebrew CMake, Ninja, Meson and MinGW installed:

```zsh
MACOS_WINE_XR_WINE11_SOURCE="/Applications/Wine Devel.app/Contents/Resources/wine" \
  ./scripts/build-current-dxmt.zsh
./scripts/run-current-dxmt-sharing-probe.zsh
```

Applications use a private Wine 11.10 copy; Wine 8.16 is only the DXMT build SDK.
Source the generated `build-current-dxmt/env.zsh` to select it for the generic
OpenXR launcher. See [Wine 11.10 and Underture](docs/native-openxr-backend.md#wine-1110-and-underture-2026-10-02)
for game validation and OpenComposite configuration.

The probe creates a shared D3D11 `Texture2DArray` and fence under current
DXMT, reads the generic DXMT metadata, and reopens the same objects as native
Metal resources. Set `MONADO_METAL_XPC_CLIENT` to a
`libmonado_metal_xpc_client.dylib` from `macos-upstream-clean` to extend the
probe through Monado's generic XPC broker.

### End-to-end OpenXR proof

Configure a native `macos-upstream-clean` Monado build, then run:

```zsh
MONADO_SOURCE_DIR=/path/to/monado \
MONADO_BUILD_DIR=/path/to/monado/build \
MWXR_BOOTSTRAP_SERVICE=1 \
./scripts/run-hello-xr-current-dxmt.zsh
```

`MWXR_BOOTSTRAP_SERVICE=1` is only needed when the currently loaded
development LaunchAgent is not already the specified clean build.

The script builds the clean native service, current DXMT, the bridge-owned
Win64 D3D11/OpenXR frontend, the standalone proxy and Khronos `hello_xr`, then
runs the D3D11 sample through:

```text
hello_xr (Win64)
  -> D3D11 / current DXMT
  -> macos-wine-xr authenticated proxy
       -> resolve DXMT Metal bootstrap metadata
       -> republish textures/shared event through Monado XPC
  -> normal macos-upstream-clean Unix IPC client connection
  -> native Monado compositor
```

The current Win64 frontend build is transitional: it uses a disposable checkout
of the last integrated Monado branch only for the Windows state-tracker/build
framework, while its D3D11 bridge sources live here. The native runtime and
service come exclusively from `macos-upstream-clean`.


## Native IPC boundary

The proxy terminates every transitional command:
`instance_get_shm_chunk`, `compositor_layer_copy_chunk`, both
`compositor_layer_sync_copy_commit*` commands, all three
`compositor_layer_sync_single*` commands and both `*_metal_bootstrap` imports.
None of these commands is sent to native Monado. The proxy receives and maps
Monado's shared-memory fd once per connection, serves Wine snapshot chunks
locally, stages only layer metadata and submits through native shared-memory
slots. It consumes the native reply even for Wine's asynchronous single-layer
submission, so the next submission uses the returned free slot. The Wine side
still receives no reply for that command.

Textures and shared events retain the existing bootstrap resolution ->
`monado_metal_xpc_publish_textures` / `monado_metal_xpc_publish_shared_event` ->
native token-import path. No image copy or GPU blit is added by this migration.
Each successful translated texture import logs its image count,
`path=shared-metal-zero-copy pixel-copies=0 gpu-blits=0`; successful shared-event
imports log `path=native-token`. These logs describe the proxy's handoff, not
compositor rendering or a completed hardware validation.

`protocol/monado-wine.json` freezes the Wine/proxy command IDs and layouts.
`generate_monado_wire.py` independently generates native IDs from the selected
Monado schemas and checks them against its generated header. It embeds the
Monado commit and protocol SHA-256 in the proxy's startup log. CMake pins `MONADO_HEADER_REVISION` at configuration; changing the selected
commit requires reconfiguration (`run-proxy.zsh` does this). Build against the
same Monado checkout/build used by the service, and rebuild the proxy whenever
that revision changes. Native IDs may shift without rebuilding the Wine client.
`check_monado_wire_compat.py` checks the Wine/proxy ABI separately from the native
commands the proxy sends; removed transitional native commands are allowed.

Local validation on 2026-10-02, before any Monado deletion: native proxy build
and all five configured CTest tests pass against unchanged Monado
`7fd7f2835693d447d46da933e9a54c9f71ddfae9`. The production socket test covers fd
transfer with a fragmented reply, local shared-memory chunks, invalid uploads,
native shared-memory slots, both sync handshakes, async ordering and fragmented
Wine requests. The generator test removes the transitional native commands in
a fixture and checks ID regeneration and incompatible-schema rejection.
Object-symbol inspection confirms the proxy still references the existing
resolve/publish functions and introduces no GPU queue or blit API. Synthetic Monado hello_xr exits 0 with Metal validation; the explicit OpenXR
swapchain probe passes 2D and array-size-2 imports/acquire/wait/release with three
images each. Service logs confirm native token imports for both array sizes.
Meta's generic-host hello_xr also exits 0 with validation (46 frames); Meta
selects its existing `gpu-blit` fallback because its images are not shareable.
No PS VR2 hardware run was performed. Native Monado protocol deletion follows
this simulator gate, as authorized by the user.

Native Monado now has none of these transitional commands, handlers, staging
fields or payload types. Its command count changed from 137 to 128; native token
imports are now IDs 96 (textures) and 128 (shared events). The Wine IDs remain
frozen. Follow-up cleanup also removes unused bootstrap reconstruction helpers.
The proxy CI checkout is pinned to Monado
`6171ca5b46cbb9d154731586ab797880f969e9fc`.

The six bridge regression tests include an assertion that rejects missing,
blitting or mixed Monado sharing paths. Set
`MWXR_EXPECT_SHARING_PATH=shared-metal-zero-copy` with
`scripts/run-generic-openxr.zsh` to enforce it in an application run. The GPU
pattern probe checks every returned image for both 2D and array swapchains.
See [native IPC verification](docs/native-ipc-verification.md) for simulator
results and zero-copy evidence. PS VR2 hardware validation remains pending.

Rebuild against the selected native service revision before headset validation:

```zsh
export MONADO_SOURCE_DIR=/Users/nickkennedy/Code/monado-2
export MONADO_BUILD_DIR=$MONADO_SOURCE_DIR/.build/native-service-check
export MACOS_WINE_XR_CURRENT_DXMT_ROOT=$MONADO_SOURCE_DIR/.build/wine11-current-dxmt
# Use MWXR_BOOTSTRAP_SERVICE=1 only if intentionally replacing the development LaunchAgent.
./scripts/run-hello-xr-current-dxmt.zsh
./scripts/run-current-dxmt-sharing-probe.zsh
ctest --test-dir build-proxy --output-on-failure
```

Record the selected Monado commit, hello_xr result, 2D/array results and import
logs. Optional `MWXR_PROXY_TRACE=1` logs Wine command IDs and sizes; leave it unset
for timing runs. Timing analysis reads `ipc_submit` and `ipc_swapchain` filenames.

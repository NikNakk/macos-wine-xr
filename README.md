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

An experimental third path uses a Proton-style builtin Wine OpenXR runtime
with PE and native macOS halves in the application's process. x86_64 Wine and
its x86_64 native client talk to an ARM64 Monado service. Opaque D3D11 hello_xr
and pixel checks over every runtime-owned 2D/array Metal image pass with Metal
validation. Private XPC endpoint cold activation also passes. Alpha blending,
rendered OpenComposite frames and matched performance comparisons remain
pending. See [the implementation, measurements and acceptance ledger](docs/in-process-openxr.md).

## Active development branches

- Monado integration/reference: `NikNakk/monado:macos-game-mode-upstream-sync-2026-10`
- Monado upstream-oriented cleanup: `NikNakk/monado:macos-upstream-clean`
- DXMT native-sharing work: `NikNakk/dxmt:macos-xr-native-sharing`
- Bridge fallbacks: this repository, `main`
- Experimental in-process bridge: `codex/in-process-wine-openxr`

## Transitional path (retained)

The current path includes:

- consumer-neutral DXMT texture/fence private-data GUIDs;
- shared-Metal transport for both ordinary 2D and Texture2DArray D3D11 swapchains;
- DXMT shared-fence / MTLSharedEvent synchronization;
- a native bootstrap-name resolver;
- publication into Monado's PID-scoped Metal XPC broker;
- a standalone authenticated Wine TCP -> native Monado Unix-IPC proxy;
- proxy-owned minimum-frame-period pacing policy;
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

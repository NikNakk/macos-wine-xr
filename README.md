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

The original authenticated Monado byte-stream proxy was removed on 2026-10-27
(last present in `24b53d8`): clean Monado dropped the name-based Metal bootstrap
messages it translated (monado `e55cef5f8`), so it could no longer pass any
textures or events. DXMT has no Monado dependency; clean Monado has no Wine/DXMT
transport code.

An experimental third path uses a Proton-style builtin Wine OpenXR runtime
with PE and native macOS halves in the application's process. x86_64 Wine and
its x86_64 native client talk to an ARM64 Monado service. Opaque D3D11 hello_xr
and pixel checks over every runtime-owned 2D/array Metal image pass with Metal
validation. The same image checks and Opaque hello_xr now pass on PS VR2;
client-side compositing runs inside Wine but currently paces at 60 Hz on its
120 Hz display. Private XPC endpoint cold activation also passes. Alpha blending,
rendered OpenComposite frames and matched performance comparisons remain
pending. See [the implementation, measurements and acceptance ledger](docs/in-process-openxr.md).

## Active development branches

- Monado integration/reference: `NikNakk/monado:macos-game-mode-upstream-sync-2026-10`
- Monado upstream-oriented cleanup: `NikNakk/monado:macos-upstream-clean`
- DXMT native-sharing work: `NikNakk/dxmt:macos-xr-native-sharing`
- Bridge fallbacks: this repository, `main`
- Experimental in-process bridge: `codex/in-process-wine-openxr`

## Former proxy path (removed)

Until 2026-10-27 the repository also carried a standalone authenticated Wine TCP
to native Monado Unix-IPC proxy (`src/proxy/monado_proxy.m`, `scripts/run-proxy.zsh`)
and a Khronos `hello_xr` runner for it (`scripts/run-hello-xr-current-dxmt.zsh`).
It resolved DXMT's Metal bootstrap names, republished the textures and shared
event through Monado's Metal XPC broker, and owned the minimum-frame-period
policy for those clients. Monado `e55cef5f8` removed the bootstrap messages, so
the path stopped working; see `24b53d8` for its last version. The in-process
wineopenxr path replaces it. Its TCP authentication (`src/rpc/tcp_auth.c`) remains
in use by the generic RPC transport.

## Upstreaming rule

No upstream pull requests are created automatically. Work remains on the
user-owned forks until explicitly requested.

## Build framework

The target path no longer uses the pinned DXMT v0.80/Basalt patches embedded in
Monado. Development now uses:

- `NikNakk/dxmt:macos-xr-native-sharing` for current DXMT and generic native
  texture/fence metadata;
- `NikNakk/monado:macos-upstream-clean` for the native macOS runtime.

The old integrated branch remains a reproducibility reference only. Until
2026-10-27 it was also the build framework for a transitional Monado-based Win64
OpenXR frontend (`scripts/build-wine-openxr-client.zsh`, with
`src/windows/comp_d3d11_bridge.cpp` and `oxr_d3d11_bridge.cpp` overlaid), the
client half of the former proxy path; it was removed with the proxy and is in
`a239e8f`.

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

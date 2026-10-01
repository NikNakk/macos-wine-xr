# macOS Wine XR

Bridge code and tooling for running Windows XR applications under Wine on macOS against a native XR runtime.

This repository is being split out of the macOS Monado port so that:

- **Monado** contains only generic macOS/Metal runtime support.
- **DXMT** exposes generic native macOS sharing metadata for D3D shared textures and fences.
- **macOS Wine XR** owns Wine-specific OpenXR/OpenVR glue, transport, launch tooling, tests, and compatibility policy.

## Repository split

| Repository | Responsibility |
|---|---|
| `NikNakk/monado` | Generic macOS/Metal OpenXR runtime, device support, compositor and native IPC |
| `NikNakk/dxmt` | D3D-to-Metal implementation and generic native-sharing metadata |
| this repository | Wine ↔ native XR bridge, DXMT consumer, OpenVR/OpenComposite integration and game launch tooling |

The previously working DXMT v0.80/Basalt/Monado path is retained under `legacy/` for reproducibility while the bridge is moved to current DXMT.

## Status

Migration is in progress. The working Monado integration remains available on
`NikNakk/monado:macos-game-mode-upstream-sync-2026-10`; the upstream-oriented
Monado cleanup is being developed separately on
`NikNakk/monado:macos-upstream-clean`.

No upstream pull requests are created automatically from this repository.

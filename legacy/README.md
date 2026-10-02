# Legacy embedded implementation

These files are preserved from
`NikNakk/monado:macos-game-mode-upstream-sync-2026-10`.

They document the previously working architecture in which the Wine D3D11
client and DXMT-specific patches lived inside the Monado tree. They are
reference material only and are not the target architecture.

New work should use current DXMT via
`NikNakk/dxmt:macos-xr-native-sharing` and keep Wine-specific code in this
repository.

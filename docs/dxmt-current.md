# Current DXMT native-sharing contract

Development branch: `NikNakk/dxmt:macos-xr-native-sharing`.

The branch adds `include/dxmt_native_interop.h` and publishes two stable
D3D11 private-data values:

- `DXMT_GUID_SHARED_TEXTURE_BOOTSTRAP_NAME`
- `DXMT_GUID_SHARED_FENCE_BOOTSTRAP_NAME`

Each payload is a fixed 54-byte buffer containing the opaque bootstrap name
that DXMT already registers for its underlying `MTLSharedTextureHandle` or
`MTLSharedEvent`.

The GUID numeric values intentionally match the earlier Monado-local
experimental patch so existing captures and transitional tooling can be
adapted without changing the native capability identifier. The names and
ownership are now DXMT-generic.

The bridge must not interpret the bootstrap string. It only transfers it to
the native side, where the existing macOS Metal sharing mechanism resolves it.

The old Basalt IOSurface path remains a legacy fallback only; the current-DXMT
path should first be validated for both ordinary Texture2D and Texture2DArray
swapchains using the native shared-texture mechanism.

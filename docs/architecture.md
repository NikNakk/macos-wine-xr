# Architecture

The project separates Windows/Wine compatibility from the native XR runtime.

## Ownership boundary

### DXMT

DXMT owns translation from D3D11 to Metal and the native representation of
shared D3D resources. The `macos-xr-native-sharing` branch publishes the
existing Metal bootstrap registration names on shared textures and fences
through consumer-neutral D3D11 private-data GUIDs.

DXMT does not know about Monado, OpenXR runtimes, OpenVR, or individual games.

### macOS Wine XR

This repository owns:

- querying DXMT native-sharing metadata;
- the Wine-side transport;
- the native bridge process;
- Windows OpenXR/OpenVR compatibility glue;
- OpenComposite/xrizer integration;
- launch/provisioning/test tooling;
- game-specific compatibility policy where unavoidable.

The transport treats DXMT bootstrap names as opaque capabilities.

### Monado

Monado owns only generic macOS XR functionality:

- Metal client/compositor support;
- generic Metal shared-texture import;
- generic Metal shared-event/semaphore import;
- IOSurface/XPC/native-handle transport;
- PS VR2 and other device support;
- OpenXR state tracking and composition.

The existing `swapchain_import_metal_bootstrap` and
`compositor_semaphore_import_metal_bootstrap` operations are intentionally
kept generic. They accept macOS native resource identifiers and do not know
that DXMT may be their producer.

## Migration

The previously working implementation embedded a Wine D3D11 compositor client,
Wine-specific OpenXR helpers, DXMT patches, provisioning, and game launchers in
the Monado tree. Those sources are retained under `legacy/` while the new
bridge is made independently buildable.

The target path is:

```
Windows XR/OpenVR application
        |
       D3D11
        |
       DXMT
        |
  DXMT native metadata
        |
  Wine-side bridge
        |
 native bridge process
        |
 generic macOS Metal handoff
        |
      Monado
```

No upstream pull requests are created automatically.

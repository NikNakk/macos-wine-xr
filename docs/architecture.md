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

Monado retains generic bootstrap-name import operations as native macOS
capabilities, but the Wine bridge no longer requires `monado-service` itself
to resolve DXMT names. The standalone proxy resolves the producer's opaque
names in its own bootstrap namespace and republishes ordinary
`MTLSharedTextureHandle` / `MTLSharedEventHandle` objects through Monado's
PID-scoped XPC broker. The service then consumes generic tokens from an
ordinary Unix-socket client.

## Migration

The previously working implementation embedded a Wine D3D11 compositor client,
Wine-specific OpenXR helpers, DXMT patches, provisioning, and game launchers in
the Monado tree. Those sources are retained under `legacy/` while the new
bridge is made independently buildable.

The current target path is:

```
Windows XR/OpenVR application
        |
       D3D11
        |
   current DXMT
        |
 opaque bootstrap names
        |
 macos-wine-xr proxy
        |
 resolve native Metal objects
        |
 Monado Metal XPC tokens
        |
 normal Unix-socket Monado IPC
        |
 native monado-service
```

Scalar Monado IPC is forwarded byte-for-byte by the proxy. The proxy rewrites
only compatibility policy that genuinely belongs outside Monado: session
minimum-period pacing, texture bootstrap imports, and shared-event bootstrap
imports. Native-handle IPC commands fail closed rather than being guessed at.

No upstream pull requests are created automatically.

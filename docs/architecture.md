# Architecture

```text
Win64 OpenXR application
        |
thin Win64 Khronos runtime ABI
        |
generic macos-wine-xr RPC (version + schema fingerprint)
        |
native macOS host
        |
native Khronos OpenXR loader
        |
Monado / Meta XR Simulator / future Metal runtime
```

The native host owns OpenXR instance/session/spaces/actions, event and frame
lifecycle, Metal queue, swapchains and graphics capability selection. Win64
keeps lightweight remote-ID mappings and D3D11 resource wrappers. No Monado
IPC structs or Objective-C pointers appear in the generic protocol. Serialization
is generated from bounded, fixed-width records.

```text
runtime-owned Metal texture
        |
shareable and reopenable on the matching GPU?
   yes -> DXMT D3D11 wrapper -> zero copy
   no
        |
host-created shared application texture -> DXMT D3D11 wrapper
        |
producer shared-event completion
        |
one Metal GPU blit -> acquired runtime swapchain image -> release
```

DXMT owns graphics translation and its native import COM interface and Mach
capability protocol. Neither has a Monado/Meta dependency. The host exports
textures/events through revocable capability brokers; object lifetime is
independent of a raw bootstrap-name registration. Array layout is preserved.

The generic native backend requires `XR_KHR_metal_enable`. It selects graphics
strategy by successful handle export/reopening, not runtime name or IOSurface
presence. Simulated Monado supports direct sharing; Meta 207 currently selects
blits. Monado-specific optimizations, if required in future, belong behind the
native backend rather than in Win64. No such specialization is needed for the
proven reverse-sharing route.

See [the backend note](native-openxr-backend.md) for supported limits,
authentication, build/run commands, timings and runtime caveats.

## Transitional regression path

The original bridge-owned Monado-based Windows frontend and standalone
`macos_wine_xr_proxy` remain available. That proxy authenticates Wine TCP,
resolves existing DXMT metadata, publishes native Metal objects using generic
Monado XPC tokens and forwards the ordinary Unix IPC protocol. Its protocol
compatibility checks remain intact. This path preserves the previous
DXMT-owned-texture route until physical-headset comparisons are complete.

Clean Monado owns native XR tracking/composition, generic Metal/XPC imports,
shared events and Unix IPC. Wine resource resolution, transport and application
policy live in this repository. Historical integrated sources remain under
`legacy/`; they do not define the generic Win64 runtime.

No upstream PRs are created automatically.

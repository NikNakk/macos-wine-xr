// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Renderer-neutral D3D <-> native Metal interop for the in-process runtime.
//
// OpenXR code talks only to GraphicsInterop. A backend knows how one D3D
// translation layer (DXMT, Apple D3DMetal) wraps native Metal objects; the
// native OpenXR runtime only ever sees ordinary Metal session/swapchain objects.
#pragma once
#include <d3d11_4.h>
#include <stdint.h>

enum mw_gfx_capability : uint32_t {
    MW_GFX_CAP_D3D11                = 1u << 0,
    MW_GFX_CAP_D3D12                = 1u << 1,
    MW_GFX_CAP_METAL_TEXTURE_IMPORT = 1u << 2,  // native MTLTexture -> D3D resource
    MW_GFX_CAP_IOSURFACE_IMPORT     = 1u << 3,  // includes IOSurface-backed MTLTextures
    MW_GFX_CAP_IOSURFACE_EXPORT     = 1u << 4,
    MW_GFX_CAP_SHARED_TEXTURE       = 1u << 5,  // D3D shared-handle textures
    MW_GFX_CAP_SHARED_EVENT         = 1u << 6,  // D3D fence over the session MTLSharedEvent
    MW_GFX_CAP_KEYED_MUTEX          = 1u << 7,
    MW_GFX_CAP_ZERO_COPY_IMPORT     = 1u << 8,
    MW_GFX_CAP_ZERO_COPY_EXPORT     = 1u << 9,
    MW_GFX_CAP_COPY_FALLBACK        = 1u << 10, // D3D-owned image blitted into the runtime image
};

enum class GraphicsSync {
    None,
    SharedEventGpu,  // D3D fence signals the session MTLSharedEvent; native queue waits on the GPU
    FenceCpuWait,    // D3D fence completion is waited on the CPU, then the event is signalled
};

// Issues one mw_gfx_native_params operation through the hosting module's unix call.
struct GraphicsNativeHost { int32_t (*call)(void *params); };

// Zero-initialised storage is a valid empty image.
struct GraphicsImage {
    ID3D11Texture2D *texture;  // app-visible, owned (+1)
    void *copy_source;         // native MTLTexture (+1), only for an explicit copy fallback
    bool zero_copy, iosurface;
};

class GraphicsInterop {
public:
    virtual ~GraphicsInterop() = default;
    virtual const char *name() const = 0;
    virtual uint32_t capabilities() const = 0;
    // Borrowed MTLDevice behind the D3D device; must match the runtime device.
    virtual void *metal_device() const = 0;
    // Bind the session's MTLSharedEvent as the producer completion primitive.
    virtual HRESULT bind_completion_event(void *metal_shared_event) = 0;
    virtual GraphicsSync sync() const = 0;
    // XrSwapchainUsageFlags the backend needs on native images beyond the app's.
    virtual uint64_t extra_swapchain_usage() const { return 0; }
    // Expose one runtime-owned native image to the application as D3D11.
    virtual HRESULT import_image(void *metal_texture, const D3D11_TEXTURE2D_DESC &desc, GraphicsImage *image) = 0;
    virtual void release_image(GraphicsImage *image) = 0;
    // Order all D3D work recorded so far before `value` on the completion event
    // and submit it. Only FenceCpuWait may block until the GPU completes.
    virtual HRESULT signal_completion(uint64_t value) = 0;
    virtual void flush() = 0;
};

// MWXR_GRAPHICS_BACKEND=auto (default), dxmt, d3dmetal or d3dmetal-copy.
// Only d3dmetal-copy may select a copy; auto never does.
HRESULT mw_graphics_interop_open(ID3D11Device *device, const GraphicsNativeHost &host, GraphicsInterop **out);
const char *mw_graphics_sync_name(GraphicsSync sync);
void mw_graphics_format_caps(uint32_t caps, char *buffer, size_t size);

// Backend factories: S_FALSE when the device belongs to another backend.
HRESULT mw_graphics_open_dxmt(ID3D11Device *device, const GraphicsNativeHost &host, GraphicsInterop **out);
HRESULT mw_graphics_open_d3dmetal(ID3D11Device *device, const GraphicsNativeHost &host, bool allow_copy,
                                  GraphicsInterop **out);

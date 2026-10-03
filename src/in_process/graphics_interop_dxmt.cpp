// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// DXMT backend: IDXMTNativeDevice2 retains the exact native Metal objects.
#include "graphics_interop.h"
#include "graphics_interop_native.h"
#include <stdio.h>
#include "dxmt_native_interop.h"

namespace {
class DxmtInterop final : public GraphicsInterop {
public:
    DxmtInterop(const GraphicsNativeHost &host) : host(host) {}
    ~DxmtInterop() override
    {
        if (fence) fence->Release();
        if (context) context->Release();
        if (native) native->Release();
    }
    HRESULT init(ID3D11Device *device)
    {
        HRESULT hr = device->QueryInterface(DXMT_IID_NATIVE_DEVICE2, (void **)&native);
        if (FAILED(hr)) return S_FALSE;
        ID3D11DeviceContext *immediate = nullptr;
        device->GetImmediateContext(&immediate);
        hr = immediate->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&context);
        immediate->Release();
        if (SUCCEEDED(hr)) hr = native->GetMetalDevice(&device_object);
        return hr;
    }
    const char *name() const override { return "dxmt"; }
    uint32_t capabilities() const override
    {
        // IDXMTNativeDevice2 wraps any MTLTexture, IOSurface-backed or not.
        // Its broker-name export belongs to the transitional host path only.
        return MW_GFX_CAP_D3D11 | MW_GFX_CAP_METAL_TEXTURE_IMPORT | MW_GFX_CAP_IOSURFACE_IMPORT |
               MW_GFX_CAP_SHARED_EVENT | MW_GFX_CAP_ZERO_COPY_IMPORT;
    }
    void *metal_device() const override { return (void *)device_object; }
    HRESULT bind_completion_event(void *event) override { return native->ImportMetalSharedEvent((UINT64)event, &fence); }
    GraphicsSync sync() const override { return fence ? GraphicsSync::SharedEventGpu : GraphicsSync::None; }
    HRESULT import_image(void *texture, const D3D11_TEXTURE2D_DESC &desc, GraphicsImage *image) override
    {
        mw_gfx_native_params info = {MW_GFX_NATIVE_TEXTURE_INFO};
        info.object = (uint64_t)texture;
        host.call(&info);
        HRESULT hr = native->ImportMetalTexture((UINT64)texture, &desc, &image->texture);
        if (FAILED(hr)) return hr;
        image->zero_copy = true;
        image->iosurface = !info.status && info.iosurface;
        return hr;
    }
    void release_image(GraphicsImage *image) override
    {
        if (image->texture) image->texture->Release();
        *image = {};
    }
    HRESULT signal_completion(uint64_t value) override
    {
        HRESULT hr = context->Signal(fence, value);
        if (SUCCEEDED(hr)) context->Flush();
        return hr;
    }
    void flush() override { context->Flush(); }
private:
    GraphicsNativeHost host;
    IDXMTNativeDevice2 *native = nullptr;
    ID3D11DeviceContext4 *context = nullptr;
    ID3D11Fence *fence = nullptr;
    UINT64 device_object = 0;
};
}

HRESULT mw_graphics_open_dxmt(ID3D11Device *device, const GraphicsNativeHost &host, GraphicsInterop **out)
{
    auto *interop = new DxmtInterop(host);
    if (!interop) return E_OUTOFMEMORY;
    HRESULT hr = interop->init(device);
    if (hr != S_OK) {
        if (FAILED(hr)) fprintf(stderr, "wineopenxr: ERROR DXMT direct-object interface/context unavailable hr=%#lx\n", hr);
        delete interop;
        return hr;
    }
    *out = interop;
    return S_OK;
}

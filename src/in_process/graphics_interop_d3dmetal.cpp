// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Experimental Apple D3DMetal (Game Porting Toolkit) backend.
//
// D3DMetal has no API to wrap an existing MTLTexture or MTLSharedEvent, and
// its D3D sharing entry points are stubs. Instead the native interposer is
// armed on this thread around an ordinary CreateTexture2D/CreateFence call, so
// D3DMetal's own Metal allocation returns the runtime-owned object. Every
// import is verified; a refused or unreached substitution is an error unless
// the explicit copy mode was selected, which is then logged as a copy.
#include "graphics_interop.h"
#include "graphics_interop_native.h"
#include <stdio.h>

namespace {
const char *detail_name(uint32_t detail)
{
    static const char *names[] = {"not-reached", "substituted", "captured", "device-mismatch",
        "descriptor-mismatch", "usage-not-subset", "storage-mismatch-or-heapless-shared",
        "heap-suballocation", "private-mtlevent", "interposer-unavailable"};
    return detail < sizeof(names) / sizeof(names[0]) ? names[detail] : "unknown";
}

void log_refusal(const char *what, const mw_gfx_native_params &p)
{
    fprintf(stderr, "wineopenxr: d3dmetal %s: %s; requested %llux%llu array=%llu fmt=%llu usage=%#llx storage=%llu "
            "runtime %llux%llu array=%llu fmt=%llu usage=%#llx storage=%llu\n", what, detail_name(p.detail),
            (unsigned long long)p.requested.width, (unsigned long long)p.requested.height,
            (unsigned long long)p.requested.array_length, (unsigned long long)p.requested.pixel_format,
            (unsigned long long)p.requested.usage, (unsigned long long)p.requested.storage_mode,
            (unsigned long long)p.actual.width, (unsigned long long)p.actual.height,
            (unsigned long long)p.actual.array_length, (unsigned long long)p.actual.pixel_format,
            (unsigned long long)p.actual.usage, (unsigned long long)p.actual.storage_mode);
}

class D3DMetalInterop final : public GraphicsInterop {
public:
    D3DMetalInterop(const GraphicsNativeHost &host, bool allow_copy) : host(host), allow_copy(allow_copy) {}
    ~D3DMetalInterop() override
    {
        if (completion) CloseHandle(completion);
        if (fence) fence->Release();
        if (context) context->Release();
        if (device5) device5->Release();
        if (device) device->Release();
    }
    HRESULT init(ID3D11Device *app_device)
    {
        mw_gfx_native_params p = {MW_GFX_NATIVE_IDENTIFY};
        p.object = *(uint64_t *)app_device;  // COM vtable address
        host.call(&p);
        if (p.kind == MW_GFX_NATIVE_KIND_UNKNOWN) return S_FALSE;
        fprintf(stderr, "wineopenxr: d3dmetal device %s (%s)\n",
                p.kind == MW_GFX_NATIVE_KIND_D3DMETAL ? "native object" : "PE object, framework loaded", p.text);
        device = app_device; device->AddRef();
        HRESULT hr = device->QueryInterface(__uuidof(ID3D11Device5), (void **)&device5);
        ID3D11DeviceContext *immediate = nullptr;
        if (SUCCEEDED(hr)) { device->GetImmediateContext(&immediate);
            hr = immediate->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&context);
            immediate->Release(); }
        if (FAILED(hr)) { fprintf(stderr, "wineopenxr: ERROR d3dmetal lacks D3D11.4 fences hr=%#lx\n", hr); return hr; }
        p = {MW_GFX_NATIVE_INTERPOSE};
        host.call(&p);
        if (p.status) { fprintf(stderr, "wineopenxr: ERROR d3dmetal allocation interposer unavailable\n"); return E_FAIL; }
        fprintf(stderr, "wineopenxr: d3dmetal interposer hooks=%llu heap-pool-switch=%s\n",
                (unsigned long long)p.value, p.detail ? "yes" : "no");
        // Learn D3DMetal's MTLDevice from one captured allocation.
        D3D11_TEXTURE2D_DESC desc = {1, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                     D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
        ID3D11Texture2D *probe = nullptr;
        p = arm_and_create(0, desc, &probe);
        if (probe) probe->Release();
        if (p.object) release_native(p.object);
        if (p.detail != MW_GFX_NATIVE_CAPTURED || !p.device) {
            fprintf(stderr, "wineopenxr: ERROR d3dmetal CreateTexture2D did not reach an interposed Metal "
                    "allocation on this thread (%s); zero-copy import is impossible\n", detail_name(p.detail));
            return E_FAIL;
        }
        device_object = p.device;
        return S_OK;
    }
    const char *name() const override { return allow_copy ? "d3dmetal-copy" : "d3dmetal"; }
    uint32_t capabilities() const override
    {
        // IOSurface-backed runtime images use Shared storage, which the
        // interposer refuses (see graphics_interop_native.m).
        return MW_GFX_CAP_D3D11 | MW_GFX_CAP_METAL_TEXTURE_IMPORT | MW_GFX_CAP_ZERO_COPY_IMPORT |
               (sync_mode == GraphicsSync::SharedEventGpu ? MW_GFX_CAP_SHARED_EVENT : 0) |
               (allow_copy ? MW_GFX_CAP_COPY_FALLBACK : 0);
    }
    void *metal_device() const override { return (void *)device_object; }
    HRESULT bind_completion_event(void *event) override
    {
        mw_gfx_native_params p = {MW_GFX_NATIVE_ARM_EVENT};
        p.object = (uint64_t)event;
        host.call(&p);
        HRESULT hr = device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence), (void **)&fence);
        p = {MW_GFX_NATIVE_DISARM_EVENT};
        host.call(&p);
        if (FAILED(hr)) return hr;
        session_event = (uint64_t)event;
        if (p.detail == MW_GFX_NATIVE_SUBSTITUTED) { sync_mode = GraphicsSync::SharedEventGpu; return S_OK; }
        // D3DMetal's fence is not the session event: wait for its completion on
        // the CPU and then signal the event, so the native queue wait still holds.
        fprintf(stderr, "wineopenxr: d3dmetal fence is not the session MTLSharedEvent (%s); using CPU completion wait\n",
                detail_name(p.detail));
        completion = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!completion) return E_OUTOFMEMORY;
        sync_mode = GraphicsSync::FenceCpuWait;
        return S_OK;
    }
    GraphicsSync sync() const override { return sync_mode; }
    HRESULT import_image(void *texture, const D3D11_TEXTURE2D_DESC &desc, GraphicsImage *image) override
    {
        mw_gfx_native_params p = arm_and_create((uint64_t)texture, desc, &image->texture);
        if (p.detail == MW_GFX_NATIVE_SUBSTITUTED && image->texture) { image->zero_copy = true; return S_OK; }
        log_refusal("zero-copy import refused", p);
        if (image->texture) { image->texture->Release(); image->texture = nullptr; }
        if (!allow_copy) return E_FAIL;
        // Explicit fallback: a D3DMetal-owned image, blitted into the runtime image at release.
        p = arm_and_create(0, desc, &image->texture);
        if (p.detail != MW_GFX_NATIVE_CAPTURED || !p.object || !image->texture) {
            if (p.object) release_native(p.object);
            if (image->texture) { image->texture->Release(); image->texture = nullptr; }
            return E_FAIL;
        }
        image->copy_source = (void *)p.object;
        return S_OK;
    }
    void release_image(GraphicsImage *image) override
    {
        if (image->texture) image->texture->Release();
        if (image->copy_source) release_native((uint64_t)image->copy_source);
        *image = {};
    }
    HRESULT signal_completion(uint64_t value) override
    {
        HRESULT hr = context->Signal(fence, value);
        if (FAILED(hr)) return hr;
        if (sync_mode == GraphicsSync::SharedEventGpu) { context->Flush(); return S_OK; }
        hr = fence->SetEventOnCompletion(value, completion);
        context->Flush();
        if (FAILED(hr)) return hr;
        if (fence->GetCompletedValue() < value && WaitForSingleObject(completion, 5000) != WAIT_OBJECT_0) {
            fprintf(stderr, "wineopenxr: ERROR d3dmetal fence value %llu did not complete\n", (unsigned long long)value);
            return E_FAIL;
        }
        mw_gfx_native_params p = {MW_GFX_NATIVE_SIGNAL_EVENT};
        p.object = session_event; p.value = value;
        host.call(&p);
        return p.status ? E_FAIL : S_OK;
    }
    void flush() override { context->Flush(); }
private:
    mw_gfx_native_params arm_and_create(uint64_t texture, const D3D11_TEXTURE2D_DESC &desc, ID3D11Texture2D **out)
    {
        mw_gfx_native_params p = {MW_GFX_NATIVE_ARM_TEXTURE};
        p.object = texture;
        host.call(&p);
        *out = nullptr;
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, out);
        p = {MW_GFX_NATIVE_DISARM_TEXTURE};
        host.call(&p);
        if (FAILED(hr)) { fprintf(stderr, "wineopenxr: d3dmetal CreateTexture2D hr=%#lx\n", hr); *out = nullptr; }
        return p;
    }
    void release_native(uint64_t object)
    {
        mw_gfx_native_params p = {MW_GFX_NATIVE_RELEASE};
        p.object = object;
        host.call(&p);
    }
    GraphicsNativeHost host;
    bool allow_copy;
    ID3D11Device *device = nullptr;
    ID3D11Device5 *device5 = nullptr;
    ID3D11DeviceContext4 *context = nullptr;
    ID3D11Fence *fence = nullptr;
    HANDLE completion = nullptr;
    uint64_t device_object = 0, session_event = 0;
    GraphicsSync sync_mode = GraphicsSync::None;
};
}

HRESULT mw_graphics_open_d3dmetal(ID3D11Device *device, const GraphicsNativeHost &host, bool allow_copy,
                                  GraphicsInterop **out)
{
    auto *interop = new D3DMetalInterop(host, allow_copy);
    if (!interop) return E_OUTOFMEMORY;
    HRESULT hr = interop->init(device);
    if (hr != S_OK) { delete interop; return hr; }
    *out = interop;
    return S_OK;
}

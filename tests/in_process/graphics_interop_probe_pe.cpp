// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// PE half of the graphics interop probe. It drives whichever GraphicsInterop
// backend recognises the caller's D3D11 device, without any OpenXR runtime:
// native image -> D3D11 render -> native readback, then native write -> D3D11
// readback, with completion-event ordering checked in both directions.
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
extern "C" {
#include "wine/unixlib.h"
}
#include <d3d11_4.h>
#include <stdio.h>
#include "graphics_interop.h"
#include "graphics_interop_native.h"
#include "graphics_interop_probe.h"

static int32_t interop_call(void *params) { return WINE_UNIX_CALL(PROBE_NATIVE_INTEROP, params); }

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, void *reserved)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(module);
    return !__wine_init_unix_call();
}

static int failures;
static void check(bool ok, const char *what, unsigned kind, unsigned slice, uint32_t pixel)
{
    printf("%s kind=%u slice=%u pixel=%08x %s\n", ok ? "PASS" : "FAIL", kind, slice, pixel, what);
    failures += !ok;
}

static int run_case(ID3D11Device *device, ID3D11DeviceContext *context, GraphicsInterop *interop,
                    graphics_probe_params &event, unsigned kind)
{
    static const char *names[] = {"private-2d", "private-array", "iosurface-shared-2d"};
    graphics_probe_params native = {(uint64_t)interop->metal_device()};
    native.kind = kind;
    WINE_UNIX_CALL(PROBE_CREATE, &native);
    if (native.status) { check(false, "native texture creation", kind, 0, 0); return 1; }
    unsigned slices = kind == PROBE_PRIVATE_ARRAY ? 2 : 1;
    D3D11_TEXTURE2D_DESC desc = {8, 8, 1, slices, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                 D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    GraphicsImage image = {};
    HRESULT hr = interop->import_image((void *)native.object, desc, &image);
    printf("INFO %s backend=%s import=%#lx zero-copy=%s iosurface=%s copy-source=%p\n", names[kind], interop->name(),
           hr, image.zero_copy ? "yes" : "no", image.iosurface ? "yes" : "no", image.copy_source);
    if (FAILED(hr)) {
        // A refusal is a correct result for a backend that cannot wrap this
        // allocation without copying; it must not be reported as zero-copy.
        printf("REFUSED %s by %s (no implicit copy)\n", names[kind], interop->name());
        WINE_UNIX_CALL(PROBE_RELEASE, &native);
        return 0;
    }
    check(image.zero_copy && !image.copy_source, "D3D11 texture refers to the native allocation", kind, 0, 0);
    // Forward: D3D11 renders, the completion event orders it, native reads the same object.
    for (unsigned slice = 0; slice < slices; ++slice) {
        D3D11_RENDER_TARGET_VIEW_DESC view = {DXGI_FORMAT_R8G8B8A8_UNORM};
        if (slices == 1) view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        else { view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.FirstArraySlice = slice; view.Texture2DArray.ArraySize = 1; }
        ID3D11RenderTargetView *rtv = nullptr;
        if (FAILED(device->CreateRenderTargetView(image.texture, &view, &rtv))) { check(false, "RTV", kind, slice, 0); continue; }
        float color[4] = {slice ? 0.f : 1.f, slice ? 1.f : 0.f, kind == PROBE_IOSURFACE_2D ? 1.f : 0.f, 1.f};
        context->ClearRenderTargetView(rtv, color);
        rtv->Release();
    }
    uint64_t value = ++event.value;
    graphics_probe_params early = native;
    early.event = event.object; early.value = value; early.timeout_ms = 20;
    WINE_UNIX_CALL(PROBE_VERIFY, &early);
    check(early.status == 2, "completion event not signalled before signal_completion", kind, 0, 0);
    hr = interop->signal_completion(value);
    check(SUCCEEDED(hr), "signal_completion", kind, 0, 0);
    for (unsigned slice = 0; slice < slices; ++slice) {
        graphics_probe_params read = native;
        read.event = event.object; read.value = value; read.slice = slice; read.timeout_ms = 5000;
        read.pixel_in = 0xff000000u | (kind == PROBE_IOSURFACE_2D ? 0xff0000u : 0) | (slice ? 0xff00u : 0xffu);
        WINE_UNIX_CALL(PROBE_VERIFY, &read);
        check(!read.status, "native Metal reads D3D11 writes after the completion event", kind, slice, read.pixel_out);
    }
    // Reverse: native writes the same object, D3D11 reads it through a staging copy (test readback only).
    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING; staging_desc.BindFlags = 0; staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging = nullptr;
    hr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
    for (unsigned slice = 0; SUCCEEDED(hr) && slice < slices; ++slice) {
        graphics_probe_params write = native;
        write.slice = slice; write.pixel_in = 0xff336699u + slice;
        WINE_UNIX_CALL(PROBE_FILL, &write);
        context->CopyResource(staging, image.texture);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        uint32_t pixel = 0;
        if (SUCCEEDED(context->Map(staging, D3D11CalcSubresource(0, slice, 1), D3D11_MAP_READ, 0, &mapped))) {
            pixel = *(const uint32_t *)mapped.pData;
            context->Unmap(staging, D3D11CalcSubresource(0, slice, 1));
        }
        check(!write.status && pixel == write.pixel_in, "D3D11 reads native Metal writes", kind, slice, pixel);
    }
    if (staging) staging->Release();
    interop->release_image(&image);
    WINE_UNIX_CALL(PROBE_RELEASE, &native);
    return 0;
}

extern "C" __declspec(dllexport) int WINAPI MWXRGraphicsInteropProbe(ID3D11Device *device)
{
    ID3D11DeviceContext *context = nullptr;
    device->GetImmediateContext(&context);
    GraphicsInterop *interop = nullptr;
    HRESULT hr = mw_graphics_interop_open(device, GraphicsNativeHost{interop_call}, &interop);
    if (FAILED(hr)) { printf("FAIL no graphics interop backend hr=%#lx\n", hr); context->Release(); return 1; }
    graphics_probe_params event = {(uint64_t)interop->metal_device()};
    event.kind = PROBE_EVENT;
    WINE_UNIX_CALL(PROBE_CREATE, &event);
    event.event = event.object;
    hr = event.status ? E_FAIL : interop->bind_completion_event((void *)event.object);
    printf("INFO backend=%s sync=%s bind=%#lx\n", interop->name(), mw_graphics_sync_name(interop->sync()), hr);
    if (SUCCEEDED(hr))
        for (unsigned kind = PROBE_PRIVATE_2D; kind <= PROBE_IOSURFACE_2D; ++kind) run_case(device, context, interop, event, kind);
    else ++failures;
    interop->flush();
    delete interop;
    graphics_probe_params release = event;
    WINE_UNIX_CALL(PROBE_RELEASE, &release);
    context->Release();
    printf("%s graphics interop probe failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures;
}

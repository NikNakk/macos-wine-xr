// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
#include <windows.h>
#include <d3d11_4.h>
#include <cstdio>
#include "dxmt_native_interop.h"
#include "../in_process/metal_probe.h"
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main()
{
    HMODULE helper = LoadLibraryA("wine_metal_probe.dll");
    REQUIRE(helper);
    auto probe = reinterpret_cast<LONG (WINAPI *)(UINT, void *)>(GetProcAddress(helper, "MWXRMetalProbe"));
    REQUIRE(probe);
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context)));
    IDXMTNativeDevice2 *native = nullptr;
    ID3D11DeviceContext4 *context4 = nullptr;
    REQUIRE(SUCCEEDED(device->QueryInterface(DXMT_IID_NATIVE_DEVICE2, reinterpret_cast<void **>(&native))));
    REQUIRE(SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void **>(&context4))));
    UINT64 metal_device = 0;
    REQUIRE(SUCCEEDED(native->GetMetalDevice(&metal_device)) && metal_device);
    for (unsigned arrays = 1; arrays <= 2; ++arrays) {
        for (unsigned image = 0; image < 3; ++image) {
            metal_probe_params p = {};
            p.device = metal_device; p.arrays = arrays;
            REQUIRE(probe(0, &p) == 0 && p.status == 0);
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = desc.Height = 8; desc.MipLevels = 1; desc.ArraySize = arrays;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            ID3D11Texture2D *texture = nullptr;
            desc.Width = 9;
            REQUIRE(native->ImportMetalTexture(p.texture, &desc, &texture) == E_INVALIDARG && !texture);
            desc.Width = 8;
            REQUIRE(SUCCEEDED(native->ImportMetalTexture(p.texture, &desc, &texture)) && texture);
            ID3D11Fence *fence = nullptr;
            REQUIRE(native->ImportMetalSharedEvent(p.texture, &fence) == E_INVALIDARG && !fence);
            REQUIRE(SUCCEEDED(native->ImportMetalSharedEvent(p.event, &fence)) && fence);
            p.value = 1; p.timeout_ms = 20;
            REQUIRE(probe(1, &p) == 0 && p.status == 2); // Unsignaled producer must time out.
            for (unsigned slice = 0; slice < arrays; ++slice) {
                D3D11_RENDER_TARGET_VIEW_DESC view = {};
                view.Format = desc.Format;
                if (arrays == 1) view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                else { view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                    view.Texture2DArray.FirstArraySlice = slice; view.Texture2DArray.ArraySize = 1; }
                ID3D11RenderTargetView *rtv = nullptr;
                REQUIRE(SUCCEEDED(device->CreateRenderTargetView(texture, &view, &rtv)));
                float color[4] = {slice ? 0.0f : 1.0f, slice ? 1.0f : 0.0f, image == 1 ? 1.0f : 0.0f, 1.0f};
                context->ClearRenderTargetView(rtv, color);
                rtv->Release();
            }
            REQUIRE(SUCCEEDED(context4->Signal(fence, p.value)));
            context->Flush();
            for (unsigned slice = 0; slice < arrays; ++slice) {
                p.slice = slice; p.timeout_ms = 5000;
                p.expected_pixel = 0xff000000u | (image == 1 ? 0x00ff0000u : 0) | (slice ? 0xff00u : 0xffu);
                REQUIRE(probe(1, &p) == 0 && p.status == 0);
                std::printf("PASS image=%u arrays=%u slice=%u Metal=%llx pixel=%08x fence=%llu\n",
                    image, arrays, slice, static_cast<unsigned long long>(p.texture), p.pixel,
                    static_cast<unsigned long long>(fence->GetCompletedValue()));
            }
            fence->Release(); texture->Release();
            REQUIRE(probe(2, &p) == 0);
        }
    }
    context4->Release(); native->Release(); context->Release(); device->Release(); FreeLibrary(helper);
    return 0;
}

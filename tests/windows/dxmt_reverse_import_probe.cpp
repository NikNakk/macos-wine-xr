// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "macos_wine_xr/dxmt_native_sharing.h"
#include <dxmt_native_interop.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>

using Microsoft::WRL::ComPtr;
#define CHECK(call)                                                                                                    \
	do {                                                                                                           \
		HRESULT hr = (call);                                                                                   \
		if (FAILED(hr)) {                                                                                      \
			std::fprintf(stderr, "%s: HRESULT=0x%08lx\n", #call, (unsigned long)hr);                       \
			return 1;                                                                                      \
		}                                                                                                      \
	} while (0)

int main(int argc, char **argv)
{
	if (argc != 7) return 2;
	unsigned array_size = (unsigned)std::strtoul(argv[1], nullptr, 10);
	if (array_size != 1 && array_size != 2) return 2;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
	CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1,
	                        D3D11_SDK_VERSION, &device, nullptr, &context));
	ComPtr<IDXMTNativeDevice> native;
	CHECK(device->QueryInterface(DXMT_IID_NATIVE_DEVICE, reinterpret_cast<void **>(native.GetAddressOf())));
	ComPtr<ID3D11Texture2D> images[3];
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = 64; desc.Height = 32; desc.MipLevels = 1; desc.ArraySize = array_size;
	desc.Format = (DXGI_FORMAT)std::strtoul(argv[2], nullptr, 10);
	if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) return 2;
	desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	// Reject mismatches without creating a resource or leaking a lookup right.
	for (unsigned i = 0; i < 64; ++i) {
		D3D11_TEXTURE2D_DESC bad = desc; ++bad.Width;
		ComPtr<ID3D11Texture2D> rejected;
		HRESULT hr = native->ImportSharedTexture(argv[3], &bad, &rejected);
		if (hr != E_INVALIDARG || rejected) return 1;
	}
	for (unsigned i = 0; i < 3; ++i) {
		CHECK(native->ImportSharedTexture(argv[3 + i], &desc, &images[i]));
		D3D11_TEXTURE2D_DESC actual = {};
		images[i]->GetDesc(&actual);
		if (actual.Width != desc.Width || actual.Height != desc.Height || actual.ArraySize != array_size ||
		    actual.Format != desc.Format || actual.MipLevels != 1 || actual.SampleDesc.Count != 1) return 1;
	}
	ComPtr<ID3D11DeviceContext4> context4;
	ComPtr<ID3D11Fence> fence;
	CHECK(context.As(&context4));
	CHECK(native->ImportSharedEvent(argv[6], &fence));
	std::printf("imported\n"); std::fflush(stdout);
	// Native side revokes registrations before these resources are used.
	unsigned image_index = 0;
	if (std::scanf("%u",&image_index) != 1 || image_index >= 3) return 1;
	(void)std::getchar();
	ComPtr<ID3D11Texture2D> expired_image;
	ComPtr<ID3D11Fence> expired_event;
	if (native->ImportSharedTexture(argv[3],&desc,&expired_image) != E_INVALIDARG || expired_image ||
	    native->ImportSharedEvent(argv[6],&expired_event) != E_INVALIDARG || expired_event) return 1;
	for (unsigned frame = 0; frame < 3; ++frame) {
		if (frame) {
			if (std::scanf("%u",&image_index) != 1 || image_index >= 3) return 1;
			(void)std::getchar();
		}
		unsigned i = image_index;
		for (unsigned slice = 0; slice < array_size; ++slice) {
			D3D11_RENDER_TARGET_VIEW_DESC view = {};
			view.Format = desc.Format;
			view.ViewDimension = array_size == 1 ? D3D11_RTV_DIMENSION_TEXTURE2D : D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
			if (array_size > 1) { view.Texture2DArray.FirstArraySlice = slice; view.Texture2DArray.ArraySize = 1; }
			ComPtr<ID3D11RenderTargetView> rtv;
			CHECK(device->CreateRenderTargetView(images[i].Get(), &view, &rtv));
			// A no-op copy must fail verification: initialize destination to white.
			float color[4] = {1, 1, 1, 1};
			context->ClearRenderTargetView(rtv.Get(), color);
		}
		ComPtr<ID3D11Texture2D> source;
		D3D11_TEXTURE2D_DESC source_desc = desc;
		source_desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
		CHECK(device->CreateTexture2D(&source_desc, nullptr, &source));
		for (unsigned slice = 0; slice < array_size; ++slice) {
			D3D11_RENDER_TARGET_VIEW_DESC view = {};
			view.Format = desc.Format;
			view.ViewDimension =
			    array_size == 1 ? D3D11_RTV_DIMENSION_TEXTURE2D : D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
			if (array_size > 1) {
				view.Texture2DArray.FirstArraySlice = slice;
				view.Texture2DArray.ArraySize = 1;
			}
			ComPtr<ID3D11RenderTargetView> rtv;
			CHECK(device->CreateRenderTargetView(source.Get(), &view, &rtv));
			float color[4] = {0, 0, 0, 1};
			color[(i + slice) % 3] = 1;
			context->ClearRenderTargetView(rtv.Get(), color);
		}
		context->CopyResource(images[i].Get(), source.Get());
		CHECK(context4->Signal(fence.Get(), frame+1));
		context->Flush();
		std::printf("rendered=%u\n",image_index); std::fflush(stdout);
	}
	// Keep wrappers/fence alive until native GPU verification completes.
	if (std::getchar() == EOF) return 1;
	return 0;
}

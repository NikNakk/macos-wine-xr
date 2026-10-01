#include "macos_wine_xr/dxmt_native_sharing.h"

#include <cstdio>

#include <d3d11_4.h>
#include <dxgi.h>

static int
fail(const char *what, HRESULT hr)
{
	std::fprintf(stderr, "%s failed: 0x%08lx\n", what, static_cast<unsigned long>(hr));
	return 1;
}

int
main()
{
	ID3D11Device *device = nullptr;
	ID3D11DeviceContext *context = nullptr;
	D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;

	HRESULT hr = D3D11CreateDevice(nullptr,
	                               D3D_DRIVER_TYPE_HARDWARE,
	                               nullptr,
	                               0,
	                               &level,
	                               1,
	                               D3D11_SDK_VERSION,
	                               &device,
	                               nullptr,
	                               &context);
	if (FAILED(hr)) {
		return fail("D3D11CreateDevice", hr);
	}

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = 64;
	desc.Height = 64;
	desc.MipLevels = 1;
	desc.ArraySize = 2;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

	ID3D11Texture2D *texture = nullptr;
	hr = device->CreateTexture2D(&desc, nullptr, &texture);
	if (FAILED(hr)) {
		context->Release();
		device->Release();
		return fail("CreateTexture2D(Texture2DArray)", hr);
	}

	macos_wine_xr_native_share_name texture_name = {};
	hr = macos_wine_xr_get_dxmt_texture_share_name(texture, &texture_name);
	if (FAILED(hr)) {
		texture->Release();
		context->Release();
		device->Release();
		return fail("DXMT texture native-sharing metadata", hr);
	}

	ID3D11Device5 *device5 = nullptr;
	hr = device->QueryInterface(IID_ID3D11Device5, reinterpret_cast<void **>(&device5));
	if (FAILED(hr)) {
		texture->Release();
		context->Release();
		device->Release();
		return fail("QueryInterface(ID3D11Device5)", hr);
	}

	ID3D11Fence *fence = nullptr;
	hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_ID3D11Fence, reinterpret_cast<void **>(&fence));
	if (FAILED(hr)) {
		device5->Release();
		texture->Release();
		context->Release();
		device->Release();
		return fail("CreateFence(shared)", hr);
	}

	macos_wine_xr_native_share_name fence_name = {};
	hr = macos_wine_xr_get_dxmt_fence_share_name(fence, &fence_name);
	if (FAILED(hr)) {
		fence->Release();
		device5->Release();
		texture->Release();
		context->Release();
		device->Release();
		return fail("DXMT fence native-sharing metadata", hr);
	}

	std::printf("texture=%s\n", texture_name.value);
	std::printf("fence=%s\n", fence_name.value);
	std::printf("resources are live; press Enter after the native macOS probe finishes\n");
	std::fflush(stdout);
	(void)std::getchar();

	fence->Release();
	device5->Release();
	texture->Release();
	context->Release();
	device->Release();
	return 0;
}

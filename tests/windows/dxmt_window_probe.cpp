// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdlib>

static void
check(HRESULT hr, const char *operation)
{
	if (FAILED(hr)) {
		std::fprintf(stderr, "%s failed: 0x%08lx\n", operation, (unsigned long)hr);
		std::exit(1);
	}
}
static void
pump()
{
	MSG message;
	while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
}
int
main()
{
	HINSTANCE instance = GetModuleHandleW(nullptr);
	WNDCLASSW cls = {};
	cls.hInstance = instance;
	cls.lpfnWndProc = DefWindowProcW;
	cls.lpszClassName = L"DXMTWindowProbe";
	if (!RegisterClassW(&cls))
		return 1;
	HWND windows[2] = {};
	IDXGISwapChain *chains[2] = {};
	ID3D11RenderTargetView *targets[2] = {};
	for (unsigned i = 0; i < 2; ++i) {
		windows[i] =
		    CreateWindowW(cls.lpszClassName, i ? L"DXMT green" : L"DXMT blue", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
		                  80 + i * 480, 80, 460, 320, nullptr, nullptr, instance, nullptr);
		if (!windows[i])
			return 1;
		UpdateWindow(windows[i]);
	}
	pump();
	// The foreground window is deliberately different from the first swapchain's HWND.
	SetForegroundWindow(windows[1]);
	ID3D11Device *device = nullptr;
	ID3D11DeviceContext *context = nullptr;
	check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
	                        nullptr, &context),
	      "D3D11CreateDevice");
	IDXGIDevice *dxgi = nullptr;
	IDXGIAdapter *adapter = nullptr;
	IDXGIFactory *factory = nullptr;
	check(device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi), "IDXGIDevice");
	check(dxgi->GetAdapter(&adapter), "GetAdapter");
	check(adapter->GetParent(__uuidof(IDXGIFactory), (void **)&factory), "GetParent");
	for (unsigned i = 0; i < 2; ++i) {
		DXGI_SWAP_CHAIN_DESC desc = {};
		desc.BufferDesc.Width = 400;
		desc.BufferDesc.Height = 240;
		desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		desc.BufferCount = 2;
		desc.OutputWindow = windows[i];
		desc.Windowed = TRUE;
		desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		check(factory->CreateSwapChain(device, &desc, &chains[i]), "CreateSwapChain");
		ID3D11Texture2D *buffer = nullptr;
		check(chains[i]->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&buffer), "GetBuffer");
		check(device->CreateRenderTargetView(buffer, nullptr, &targets[i]), "CreateRenderTargetView");
		buffer->Release();
	}
	for (unsigned frame = 0; frame < 45; ++frame) {
		pump();
		if (frame == 15) {
			targets[0]->Release();
			context->ClearState();
			context->Flush();
			SetWindowPos(windows[0], nullptr, 0, 0, 600, 420, SWP_NOMOVE | SWP_NOZORDER);
			pump();
			check(chains[0]->ResizeBuffers(2, 540, 340, DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers");
			ID3D11Texture2D *buffer = nullptr;
			check(chains[0]->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&buffer),
			      "GetBuffer resized");
			check(device->CreateRenderTargetView(buffer, nullptr, &targets[0]), "RTV resized");
			buffer->Release();
		}
		for (unsigned i = 0; i < 2; ++i) {
			const float blue[] = {0, 0, 1, 1}, green[] = {0, 1, 0, 1};
			context->ClearRenderTargetView(targets[i], i ? green : blue);
			check(chains[i]->Present(1, 0), "Present");
		}
	}
	context->ClearState();
	context->Flush();
	for (unsigned i = 0; i < 2; ++i) {
		targets[i]->Release();
		chains[i]->Release();
		DestroyWindow(windows[i]);
	}
	factory->Release();
	adapter->Release();
	dxgi->Release();
	context->Release();
	device->Release();
	UnregisterClassW(cls.lpszClassName, instance);
	std::puts("two-window DXGI create/present/resize/destroy passed");
	return 0;
}

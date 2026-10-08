// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
// A D3D11 swapchain on another process's window, as Chromium's GPU process
// makes for its browser process. The parent creates a window (and loads
// d3d11.dll, as a Chromium browser process does) and starts a child with the
// window handle; the child creates a swapchain on it and presents red, green
// and blue for a second each, for the given number of seconds.
//
//   x86_64-w64-mingw32-gcc -O1 -o d3d11_xproc_swapchain.exe d3d11_xproc_swapchain.c -ld3d11 -ldxgi -lgdi32 -luser32
//   d3d11_xproc_swapchain.exe [seconds]
#define COBJMACROS
#include <windows.h>

#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>

static LRESULT CALLBACK
proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	return DefWindowProcA(hwnd, msg, wp, lp);
}

static int
child(HWND hwnd, int seconds)
{
	DXGI_SWAP_CHAIN_DESC desc = {0};
	desc.BufferDesc.Width = 320;
	desc.BufferDesc.Height = 240;
	desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = 2;
	desc.OutputWindow = hwnd;
	desc.Windowed = TRUE;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	IDXGISwapChain *swapchain = NULL;
	ID3D11Device *device = NULL;
	ID3D11DeviceContext *context = NULL;
	HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
	                                           &desc, &swapchain, &device, NULL, &context);
	printf("child: D3D11CreateDeviceAndSwapChain on %p: 0x%08lx\n", (void *)hwnd, (unsigned long)hr);
	if (FAILED(hr)) {
		return 1;
	}
	ID3D11Texture2D *back = NULL;
	IDXGISwapChain_GetBuffer(swapchain, 0, &IID_ID3D11Texture2D, (void **)&back);
	ID3D11RenderTargetView *rtv = NULL;
	ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)back, NULL, &rtv);
	const float colours[3][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};
	int presents = 0;
	DWORD start = GetTickCount();
	while (GetTickCount() - start < (DWORD)seconds * 1000) {
		int which = (int)((GetTickCount() - start) / 1000) % 3;
		ID3D11DeviceContext_ClearRenderTargetView(context, rtv, colours[which]);
		if (FAILED(IDXGISwapChain_Present(swapchain, 1, 0))) {
			break;
		}
		++presents;
	}
	printf("child: %d presents\n", presents);
	ID3D11RenderTargetView_Release(rtv);
	ID3D11Texture2D_Release(back);
	IDXGISwapChain_Release(swapchain);
	ID3D11DeviceContext_Release(context);
	ID3D11Device_Release(device);
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc > 2 && !strcmp(argv[1], "--child")) {
		return child((HWND)(uintptr_t)strtoull(argv[2], NULL, 16), argc > 3 ? atoi(argv[3]) : 6);
	}
	int seconds = argc > 1 ? atoi(argv[1]) : 6;
	LoadLibraryA("d3d11.dll"); // DXMT's host thread starts here
	WNDCLASSA wc = {0};
	wc.lpfnWndProc = proc;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.lpszClassName = "d3d11_xproc_swapchain";
	wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	RegisterClassA(&wc);
	HWND hwnd = CreateWindowA("d3d11_xproc_swapchain", "cross-process swapchain", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
	                          200, 200, 336, 279, NULL, NULL, wc.hInstance, NULL);
	char cmd[MAX_PATH + 64], self[MAX_PATH];
	GetModuleFileNameA(NULL, self, sizeof(self));
	snprintf(cmd, sizeof(cmd), "\"%s\" --child %llx %d", self, (unsigned long long)(uintptr_t)hwnd, seconds);
	STARTUPINFOA si = {sizeof(si)};
	PROCESS_INFORMATION pi;
	if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
		printf("parent: CreateProcess failed %lu\n", GetLastError());
		return 1;
	}
	MSG msg;
	while (MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, INFINITE, QS_ALLINPUT) != WAIT_OBJECT_0) {
		while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
			DispatchMessageA(&msg);
		}
	}
	printf("parent: child exited\n");
	Sleep(500);
	DestroyWindow(hwnd);
	return 0;
}

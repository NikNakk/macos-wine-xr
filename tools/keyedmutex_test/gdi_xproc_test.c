// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
// Can one Wine process draw into another process's window with GDI? The
// parent creates a window filled with black and starts a child with the
// window handle; the child fills it red with GetDC/FillRect (and, with
// "bitblt", with StretchDIBits). The parent then reads its own window's
// pixels back.
//
//   x86_64-w64-mingw32-gcc -O1 -o gdi_xproc_test.exe gdi_xproc_test.c -lgdi32 -luser32
//   gdi_xproc_test.exe [fill|bitblt]
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>

static LRESULT CALLBACK
proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	return DefWindowProcA(hwnd, msg, wp, lp);
}

static void
pump(DWORD ms)
{
	DWORD end = GetTickCount() + ms;
	MSG msg;
	while (GetTickCount() < end) {
		while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageA(&msg);
		}
		Sleep(10);
	}
}

static int
child(HWND hwnd, const char *mode)
{
	RECT rect;
	GetClientRect(hwnd, &rect);
	HDC dc = GetDC(hwnd);
	printf("child: GetDC(%p) -> %p, client %ldx%ld\n", (void *)hwnd, (void *)dc, rect.right, rect.bottom);
	if (!dc) {
		return 1;
	}
	BOOL ok;
	if (!strcmp(mode, "bitblt")) {
		int w = rect.right, h = rect.bottom;
		DWORD *pixels = malloc((size_t)w * h * 4);
		for (int i = 0; i < w * h; ++i) {
			pixels[i] = 0x00ff0000; // BGRA red
		}
		BITMAPINFO info = {0};
		info.bmiHeader.biSize = sizeof(info.bmiHeader);
		info.bmiHeader.biWidth = w;
		info.bmiHeader.biHeight = -h;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;
		ok = StretchDIBits(dc, 0, 0, w, h, 0, 0, w, h, pixels, &info, DIB_RGB_COLORS, SRCCOPY) == h;
		free(pixels);
	} else {
		HBRUSH red = CreateSolidBrush(RGB(255, 0, 0));
		ok = FillRect(dc, &rect, red);
		DeleteObject(red);
	}
	GdiFlush();
	printf("child: %s %s\n", mode, ok ? "ok" : "failed");
	ReleaseDC(hwnd, dc);
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc > 2 && !strcmp(argv[1], "--child")) {
		return child((HWND)(uintptr_t)strtoull(argv[2], NULL, 16), argc > 3 ? argv[3] : "fill");
	}
	const char *mode = argc > 1 ? argv[1] : "fill";
	WNDCLASSA wc = {0};
	wc.lpfnWndProc = proc;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.lpszClassName = "gdi_xproc_test";
	wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	RegisterClassA(&wc);
	HWND hwnd = CreateWindowA("gdi_xproc_test", "gdi_xproc_test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
	                          320, 240, NULL, NULL, wc.hInstance, NULL);
	pump(1000);
	char cmd[MAX_PATH + 64], self[MAX_PATH];
	GetModuleFileNameA(NULL, self, sizeof(self));
	snprintf(cmd, sizeof(cmd), "\"%s\" --child %llx %s", self, (unsigned long long)(uintptr_t)hwnd, mode);
	STARTUPINFOA si = {sizeof(si)};
	PROCESS_INFORMATION pi;
	if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
		printf("parent: CreateProcess failed %lu\n", GetLastError());
		return 1;
	}
	while (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) {
		pump(50);
	}
	pump(500);
	HDC dc = GetDC(hwnd);
	COLORREF centre = GetPixel(dc, 160, 100);
	ReleaseDC(hwnd, dc);
	printf("parent: pixel at centre %06lx (%s)\n", (unsigned long)centre,
	       centre == RGB(255, 0, 0) ? "red: the child's drawing arrived" : "not red");
	DestroyWindow(hwnd);
	return 0;
}

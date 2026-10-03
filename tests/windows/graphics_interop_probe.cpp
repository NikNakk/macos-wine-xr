// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Creates an ordinary D3D11 device with whichever translation layer the Wine
// runtime provides (DXMT or D3DMetal) and runs the graphics interop probe.
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
int main()
{
    HMODULE probe = LoadLibraryA("mwxr_graphics_probe.dll");
    auto run = probe ? (int (WINAPI *)(ID3D11Device *))GetProcAddress(probe, "MWXRGraphicsInteropProbe") : nullptr;
    if (!run) { std::puts("FAIL mwxr_graphics_probe.dll unavailable"); return 1; }
    ID3D11Device *device = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                   &device, nullptr, nullptr);
    if (FAILED(hr)) { std::printf("FAIL D3D11CreateDevice hr=%#lx\n", hr); return 1; }
    int failures = run(device);
    device->Release();
    return failures ? 1 : 0;
}

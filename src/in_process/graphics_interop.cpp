// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Backend selection for the renderer-neutral graphics interop layer.
#include "graphics_interop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Builtin PE modules link no C++ runtime: backends are allocated from the CRT
// heap, built without exceptions or RTTI, and callers check for null.
void *operator new(size_t size) { return malloc(size); }
void operator delete(void *pointer) noexcept { free(pointer); }
void operator delete(void *pointer, size_t) noexcept { free(pointer); }

const char *mw_graphics_sync_name(GraphicsSync sync)
{
    switch (sync) {
    case GraphicsSync::SharedEventGpu: return "shared-event-gpu-wait";
    case GraphicsSync::FenceCpuWait: return "fence-cpu-wait";
    default: return "none";
    }
}

void mw_graphics_format_caps(uint32_t caps, char *buffer, size_t size)
{
    static const struct { uint32_t bit; const char *name; } names[] = {
        {MW_GFX_CAP_D3D11, "d3d11"}, {MW_GFX_CAP_D3D12, "d3d12"},
        {MW_GFX_CAP_METAL_TEXTURE_IMPORT, "metal-texture-import"},
        {MW_GFX_CAP_IOSURFACE_IMPORT, "iosurface-import"}, {MW_GFX_CAP_IOSURFACE_EXPORT, "iosurface-export"},
        {MW_GFX_CAP_SHARED_TEXTURE, "shared-texture"}, {MW_GFX_CAP_SHARED_EVENT, "shared-event"},
        {MW_GFX_CAP_KEYED_MUTEX, "keyed-mutex"}, {MW_GFX_CAP_ZERO_COPY_IMPORT, "zero-copy-import"},
        {MW_GFX_CAP_ZERO_COPY_EXPORT, "zero-copy-export"}, {MW_GFX_CAP_COPY_FALLBACK, "copy-fallback"},
    };
    size_t used = 0;
    buffer[0] = 0;
    for (const auto &entry : names)
        if ((caps & entry.bit) && used < size)
            used += snprintf(buffer + used, size - used, "%s%s", used ? "," : "", entry.name);
}

static void log_selection(GraphicsInterop *interop, const char *mode)
{
    char caps[256];
    mw_graphics_format_caps(interop->capabilities(), caps, sizeof(caps));
    fprintf(stderr, "wineopenxr: graphics backend=%s selection=%s device=%p caps=%s\n",
            interop->name(), mode, interop->metal_device(), caps);
}

HRESULT mw_graphics_interop_open(ID3D11Device *device, const GraphicsNativeHost &host, GraphicsInterop **out)
{
    *out = nullptr;
    const char *mode = getenv("MWXR_GRAPHICS_BACKEND");
    if (!mode || !mode[0]) mode = "auto";
    if (strcmp(mode, "auto") && strcmp(mode, "dxmt")) {
        fprintf(stderr, "wineopenxr: ERROR MWXR_GRAPHICS_BACKEND=%s; use auto or dxmt\n", mode);
        return E_INVALIDARG;
    }
    // Each factory declines (S_FALSE) a device owned by another translation layer.
    HRESULT hr = mw_graphics_open_dxmt(device, host, out);
    if (hr == S_FALSE) {
        fprintf(stderr, "wineopenxr: ERROR no graphics backend (selection=%s) recognises this D3D11 device\n", mode);
        return E_NOINTERFACE;
    }
    if (SUCCEEDED(hr)) log_selection(*out, mode);
    return hr;
}

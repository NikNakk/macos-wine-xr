// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Fixed-width ABI between the PE graphics interop backends and their native
// helper. Object fields carry borrowed Objective-C pointers valid only in this
// process; nothing here is a sharing handle or crosses a process boundary.
#pragma once
#include <stdint.h>

enum mw_gfx_native_op {
    MW_GFX_NATIVE_IDENTIFY = 1,   // object: COM vtable address -> kind, text = owning Mach-O image
    MW_GFX_NATIVE_TEXTURE_INFO,   // object: MTLTexture -> desc, iosurface
    MW_GFX_NATIVE_INTERPOSE,      // install the Metal allocation interposer (idempotent)
    MW_GFX_NATIVE_ARM_TEXTURE,    // object: MTLTexture to hand out, or 0 to capture only
    MW_GFX_NATIVE_DISARM_TEXTURE, // -> detail, device; object: captured texture (+1) when capture only
    MW_GFX_NATIVE_ARM_EVENT,      // object: MTLSharedEvent to hand out
    MW_GFX_NATIVE_DISARM_EVENT,   // -> detail
    MW_GFX_NATIVE_SIGNAL_EVENT,   // object: MTLSharedEvent, value: CPU signal after completion
    MW_GFX_NATIVE_RELEASE,        // object: release one reference
};

enum mw_gfx_native_kind { MW_GFX_NATIVE_KIND_UNKNOWN, MW_GFX_NATIVE_KIND_D3DMETAL };

// DISARM results. Anything other than SUBSTITUTED/CAPTURED means the D3D
// object does not refer to the requested native allocation.
enum mw_gfx_native_detail {
    MW_GFX_NATIVE_NOT_REACHED,        // no interposed Metal allocation ran on this thread
    MW_GFX_NATIVE_SUBSTITUTED,        // the exact armed object was returned to the D3D runtime
    MW_GFX_NATIVE_CAPTURED,           // capture only: the D3D runtime's own allocation is recorded
    MW_GFX_NATIVE_MISMATCH_DEVICE,
    MW_GFX_NATIVE_MISMATCH_DESCRIPTOR,
    MW_GFX_NATIVE_MISMATCH_USAGE,
    MW_GFX_NATIVE_MISMATCH_STORAGE,
    MW_GFX_NATIVE_HEAP_PLACEMENT,     // reached through an MTLHeap suballocation; never substituted
    MW_GFX_NATIVE_UNSUPPORTED,        // interposer unavailable
};

struct mw_gfx_native_desc {
    uint64_t width, height, array_length, pixel_format, usage, storage_mode, texture_type, mip_levels, samples;
};

struct mw_gfx_native_params {
    uint32_t op;
    int32_t status;   // 0 on success
    uint64_t object;
    uint64_t value;
    uint64_t device;  // MTLDevice that served the interposed allocation
    uint32_t kind, detail, iosurface, reserved;
    struct mw_gfx_native_desc requested, actual;
    char text[256];
};

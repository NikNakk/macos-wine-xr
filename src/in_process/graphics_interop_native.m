// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Native half of the graphics interop layer: plain Metal object queries and
// operations requested by the PE backends. It knows no OpenXR state.
#if 0
#pragma makedep unix
#endif
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include "graphics_interop_native.h"

static void describe(id<MTLTexture> texture, struct mw_gfx_native_desc *desc)
{
    desc->width = texture.width; desc->height = texture.height; desc->array_length = texture.arrayLength;
    desc->pixel_format = texture.pixelFormat; desc->usage = texture.usage; desc->storage_mode = texture.storageMode;
    desc->texture_type = texture.textureType; desc->mip_levels = texture.mipmapLevelCount;
    desc->samples = texture.sampleCount;
}

static void identify(struct mw_gfx_native_params *p)
{
    // A COM object's vtable lives in the image that implements it. DXMT's
    // vtables are in PE modules (no Mach-O image); D3DMetal's are in its framework.
    Dl_info info = {0};
    p->kind = MW_GFX_NATIVE_KIND_UNKNOWN;
    if (!p->object || !dladdr((const void *)p->object, &info) || !info.dli_fname) return;
    snprintf(p->text, sizeof(p->text), "%s", info.dli_fname);
    if (strstr(info.dli_fname, "/D3DMetal.framework/")) p->kind = MW_GFX_NATIVE_KIND_D3DMETAL;
}

int32_t mw_graphics_native_call(void *args)
{
    struct mw_gfx_native_params *p = args;
    p->status = 0;
    @autoreleasepool {
        switch (p->op) {
        case MW_GFX_NATIVE_IDENTIFY: identify(p); break;
        case MW_GFX_NATIVE_TEXTURE_INFO: {
            id<MTLTexture> texture = (id<MTLTexture>)p->object;
            if (!texture || ![texture conformsToProtocol:@protocol(MTLTexture)]) { p->status = -1; break; }
            describe(texture, &p->actual);
            p->iosurface = texture.iosurface != NULL;
            p->device = (uint64_t)texture.device;
            break;
        }
        case MW_GFX_NATIVE_SIGNAL_EVENT: {
            id<MTLSharedEvent> event = (id<MTLSharedEvent>)p->object;
            if (!event || ![event conformsToProtocol:@protocol(MTLSharedEvent)]) { p->status = -1; break; }
            if (event.signaledValue < p->value) event.signaledValue = p->value;
            break;
        }
        case MW_GFX_NATIVE_RELEASE: [(id)p->object release]; break;
        default: p->status = -1; break;
        }
    }
    return 0;
}

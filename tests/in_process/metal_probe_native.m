// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <unistd.h>
typedef int32_t NTSTATUS;
typedef NTSTATUS (*unixlib_entry_t)(void *);
#define STATUS_SUCCESS 0
#include "metal_probe.h"
static NTSTATUS create_objects(void *args)
{
    struct metal_probe_params *p = args;
    p->status = 1;
    @autoreleasepool {
        id<MTLDevice> device = (id<MTLDevice>)p->device;
        if (!device || (p->arrays != 1 && p->arrays != 2)) return STATUS_SUCCESS;
        MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:8 height:8 mipmapped:NO];
        desc.textureType = p->arrays == 1 ? MTLTextureType2D : MTLTextureType2DArray;
        desc.arrayLength = p->arrays;
        desc.storageMode = MTLStorageModeShared;
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
        id<MTLSharedEvent> event = [device newSharedEvent];
        if (!texture || !event) { [texture release]; [event release]; return STATUS_SUCCESS; }
        p->texture = (uint64_t)texture;
        p->event = (uint64_t)event;
        p->status = 0;
    }
    return STATUS_SUCCESS;
}
static NTSTATUS wait_read(void *args)
{
    struct metal_probe_params *p = args;
    p->status = 1;
    @autoreleasepool {
        id<MTLSharedEvent> event = (id<MTLSharedEvent>)p->event;
        id<MTLTexture> texture = (id<MTLTexture>)p->texture;
        for (uint32_t i = 0; event.signaledValue < p->value && i < p->timeout_ms; ++i) usleep(1000);
        if (event.signaledValue < p->value) { p->status = 2; return STATUS_SUCCESS; }
        if (p->slice >= texture.arrayLength) return STATUS_SUCCESS;
        uint32_t pixels[64];
        [texture getBytes:pixels bytesPerRow:8*4 bytesPerImage:8*8*4
            fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0 slice:p->slice];
        p->pixel = pixels[0];
        for (uint32_t i = 0; i < 64; ++i) if (pixels[i] != p->expected_pixel) return STATUS_SUCCESS;
        p->status = 0;
    }
    return STATUS_SUCCESS;
}
static NTSTATUS release_objects(void *args)
{
    struct metal_probe_params *p = args;
    @autoreleasepool { [(id)p->texture release]; [(id)p->event release]; }
    p->texture = p->event = 0;
    return STATUS_SUCCESS;
}
const unixlib_entry_t __wine_unix_call_funcs[] = {create_objects, wait_read, release_objects};

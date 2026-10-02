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
        // Diagnostic readback only: production image sharing never copies.
        // Private runtime textures require a GPU blit into a CPU-visible buffer.
        id<MTLDevice> device = (id<MTLDevice>)p->device;
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLBuffer> buffer = [device newBufferWithLength:256*8 options:MTLResourceStorageModeShared];
        if (!queue || !buffer) { [queue release]; [buffer release]; return STATUS_SUCCESS; }
        id<MTLCommandBuffer> command = [queue commandBuffer];
        [command encodeWaitForEvent:event value:p->value];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        [blit copyFromTexture:texture sourceSlice:p->slice sourceLevel:0
            sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1)
            toBuffer:buffer destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256*8];
        [blit endEncoding]; [command commit]; [command waitUntilCompleted];
        BOOL correct = command.status == MTLCommandBufferStatusCompleted;
        for (uint32_t y = 0; y < 8; ++y) {
            const uint32_t *pixels = (const uint32_t *)((const char *)buffer.contents + y*256);
            if (!y) p->pixel = pixels[0];
            for (uint32_t x = 0; x < 8; ++x) if (pixels[x] != p->expected_pixel) correct = NO;
        }
        [buffer release]; [queue release];
        if (!correct) return STATUS_SUCCESS;
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
static NTSTATUS create_event(void *args)
{
    struct metal_probe_params *p = args;
    @autoreleasepool { p->event = (uint64_t)[(id<MTLDevice>)p->device newSharedEvent]; }
    p->status = p->event ? 0 : 1;
    return STATUS_SUCCESS;
}
static NTSTATUS release_event(void *args)
{
    struct metal_probe_params *p = args;
    @autoreleasepool { [(id)p->event release]; }
    p->event = 0;
    return STATUS_SUCCESS;
}
const unixlib_entry_t __wine_unix_call_funcs[] = {create_objects, wait_read, release_objects, create_event, release_event};

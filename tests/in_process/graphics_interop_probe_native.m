// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Native half of the graphics interop probe: creates runtime-like Metal
// objects, writes them on the GPU and reads them back. Readback blits are
// diagnostic only; the interop path under test never copies.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#include <unistd.h>
#include "graphics_interop_probe.h"
typedef int32_t (*unixlib_entry_t)(void *);
int32_t mw_graphics_native_call(void *params);
enum { SIZE = 8, ROW = 256 };

static int32_t create(void *args)
{
    struct graphics_probe_params *p = args;
    p->status = 1;
    @autoreleasepool {
        id<MTLDevice> device = (id<MTLDevice>)p->device;
        if (p->kind == PROBE_EVENT) { p->object = (uint64_t)[device newSharedEvent]; p->status = !p->object; return 0; }
        MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:SIZE height:SIZE mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        if (p->kind == PROBE_IOSURFACE_2D) {
            // As Monado's IOSurface service swapchain: Shared storage over an IOSurface.
            NSDictionary *props = @{(id)kIOSurfaceWidth: @(SIZE), (id)kIOSurfaceHeight: @(SIZE),
                                    (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfacePixelFormat: @((uint32_t)'RGBA')};
            IOSurfaceRef surface = IOSurfaceCreate((CFDictionaryRef)props);
            desc.storageMode = MTLStorageModeShared;
            p->object = (uint64_t)[device newTextureWithDescriptor:desc iosurface:surface plane:0];
            if (surface) CFRelease(surface);
        } else {
            // As Monado's direct swapchain: Private storage from newSharedTextureWithDescriptor.
            desc.textureType = p->kind == PROBE_PRIVATE_ARRAY ? MTLTextureType2DArray : MTLTextureType2D;
            desc.arrayLength = p->kind == PROBE_PRIVATE_ARRAY ? 2 : 1;
            desc.storageMode = MTLStorageModePrivate;
            p->object = (uint64_t)[device newSharedTextureWithDescriptor:desc];
        }
        p->status = !p->object;
    }
    return 0;
}

static int32_t verify(void *args)
{
    struct graphics_probe_params *p = args;
    p->status = 1;
    @autoreleasepool {
        id<MTLSharedEvent> event = (id<MTLSharedEvent>)p->event;
        id<MTLTexture> texture = (id<MTLTexture>)p->object;
        for (uint32_t i = 0; event.signaledValue < p->value && i < p->timeout_ms; ++i) usleep(1000);
        if (event.signaledValue < p->value) { p->status = 2; return 0; }
        id<MTLDevice> device = texture.device;
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLBuffer> buffer = [device newBufferWithLength:ROW * SIZE options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> command = [queue commandBuffer];
        [command encodeWaitForEvent:event value:p->value];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        [blit copyFromTexture:texture sourceSlice:p->slice sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
            sourceSize:MTLSizeMake(SIZE, SIZE, 1) toBuffer:buffer destinationOffset:0
            destinationBytesPerRow:ROW destinationBytesPerImage:ROW * SIZE];
        [blit endEncoding]; [command commit]; [command waitUntilCompleted];
        BOOL correct = command.status == MTLCommandBufferStatusCompleted;
        for (uint32_t y = 0; y < SIZE; ++y) {
            const uint32_t *row = (const uint32_t *)((const char *)buffer.contents + y * ROW);
            if (!y) p->pixel_out = row[0];
            for (uint32_t x = 0; x < SIZE; ++x) correct &= row[x] == p->pixel_in;
        }
        [buffer release]; [queue release];
        p->status = correct ? 0 : 3;
    }
    return 0;
}

static int32_t fill(void *args)
{
    struct graphics_probe_params *p = args;
    @autoreleasepool {
        id<MTLTexture> texture = (id<MTLTexture>)p->object;
        id<MTLCommandQueue> queue = [texture.device newCommandQueue];
        id<MTLCommandBuffer> command = [queue commandBuffer];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = texture;
        pass.colorAttachments[0].slice = p->slice;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        uint32_t c = p->pixel_in;  // RGBA8 little-endian: R in the low byte
        pass.colorAttachments[0].clearColor = MTLClearColorMake((c & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0,
                                                                ((c >> 16) & 0xff) / 255.0, (c >> 24) / 255.0);
        [[command renderCommandEncoderWithDescriptor:pass] endEncoding];
        [command commit]; [command waitUntilCompleted];
        p->status = command.status == MTLCommandBufferStatusCompleted ? 0 : 1;
        [queue release];
    }
    return 0;
}

static int32_t release(void *args)
{
    struct graphics_probe_params *p = args;
    @autoreleasepool { [(id)p->object release]; }
    p->object = 0;
    return 0;
}

const unixlib_entry_t __wine_unix_call_funcs[] = {mw_graphics_native_call, create, verify, fill, release};

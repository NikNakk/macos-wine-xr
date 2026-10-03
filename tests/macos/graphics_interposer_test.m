// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Exercises the graphics interop allocation interposer without a D3D runtime:
// direct Metal allocations stand in for those a translation layer such as
// D3DMetal makes inside ID3D11Device::CreateTexture2D / CreateFence.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#include <pthread.h>
#include <stdio.h>
#include "../../src/in_process/graphics_interop_native.h"

int32_t mw_graphics_native_call(void *params);
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static struct mw_gfx_native_params call(uint32_t op, id object)
{
    struct mw_gfx_native_params p = {op};
    p.object = (uint64_t)object;
    mw_graphics_native_call(&p);
    return p;
}

static MTLTextureDescriptor *descriptor(MTLStorageMode storage, NSUInteger arrays)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:8 height:8 mipmapped:NO];
    d.textureType = arrays > 1 ? MTLTextureType2DArray : MTLTextureType2D;
    d.arrayLength = arrays;
    d.storageMode = storage;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return d;
}

// Allocate as a translation layer would, between arm and disarm.
static id<MTLTexture> allocate(id<MTLDevice> device, id armed, MTLTextureDescriptor *request, uint32_t *detail)
{
    call(MW_GFX_NATIVE_ARM_TEXTURE, armed);
    id<MTLTexture> result = [device newTextureWithDescriptor:request];
    struct mw_gfx_native_params p = call(MW_GFX_NATIVE_DISARM_TEXTURE, nil);
    *detail = p.detail;
    if (p.object) { CHECK(!armed && (id)p.object == result); [(id)p.object release]; }
    return result;
}

static void *other_thread(void *device)
{
    return [(id<MTLDevice>)device newTextureWithDescriptor:descriptor(MTLStorageModePrivate, 1)];
}

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { puts("SKIP no Metal device"); return 77; }
        // Runtime-owned images, created before the interposer exists.
        id<MTLTexture> runtime2d = [device newSharedTextureWithDescriptor:descriptor(MTLStorageModePrivate, 1)];
        id<MTLTexture> runtime_array = [device newSharedTextureWithDescriptor:descriptor(MTLStorageModePrivate, 2)];
        NSDictionary *props = @{(id)kIOSurfaceWidth: @8, (id)kIOSurfaceHeight: @8, (id)kIOSurfaceBytesPerElement: @4,
                                (id)kIOSurfacePixelFormat: @((uint32_t)'RGBA')};
        IOSurfaceRef surface = IOSurfaceCreate((CFDictionaryRef)props);
        id<MTLTexture> runtime_iosurface = [device newTextureWithDescriptor:descriptor(MTLStorageModeShared, 1)
                                                                 iosurface:surface plane:0];
        id<MTLSharedEvent> runtime_event = [device newSharedEvent];
        CHECK(runtime2d && runtime_array && runtime_iosurface && runtime_event);

        struct mw_gfx_native_params p = call(MW_GFX_NATIVE_INTERPOSE, nil);
        CHECK(p.status == 0 && p.value >= 4);
        printf("interposer hooks=%llu\n", (unsigned long long)p.value);

        uint32_t detail;
        id<MTLTexture> t = allocate(device, runtime2d, descriptor(MTLStorageModePrivate, 1), &detail);
        CHECK(t == runtime2d && detail == MW_GFX_NATIVE_SUBSTITUTED);
        [t release];
        t = allocate(device, runtime_array, descriptor(MTLStorageModePrivate, 2), &detail);
        CHECK(t == runtime_array && detail == MW_GFX_NATIVE_SUBSTITUTED);
        [t release];
        printf("PASS substitution returns the exact runtime 2D and array objects\n");

        MTLTextureDescriptor *request = descriptor(MTLStorageModePrivate, 1);
        request.width = 16;
        t = allocate(device, runtime2d, request, &detail);
        CHECK(t && t != runtime2d && detail == MW_GFX_NATIVE_MISMATCH_DESCRIPTOR);
        [t release];
        request = descriptor(MTLStorageModePrivate, 1);
        request.usage |= MTLTextureUsageShaderWrite;
        t = allocate(device, runtime2d, request, &detail);
        CHECK(t && t != runtime2d && detail == MW_GFX_NATIVE_MISMATCH_USAGE);
        [t release];
        t = allocate(device, runtime2d, descriptor(MTLStorageModeShared, 1), &detail);
        CHECK(t && t != runtime2d && detail == MW_GFX_NATIVE_MISMATCH_STORAGE);
        [t release];
        t = allocate(device, runtime_iosurface, descriptor(MTLStorageModeShared, 1), &detail);
        CHECK(t && t != runtime_iosurface && detail == MW_GFX_NATIVE_MISMATCH_STORAGE);
        [t release];
        printf("PASS incompatible descriptor, usage and heapless Shared/IOSurface storage are refused\n");

        t = allocate(device, nil, descriptor(MTLStorageModePrivate, 1), &detail);
        CHECK(t && detail == MW_GFX_NATIVE_CAPTURED);
        [t release];
        call(MW_GFX_NATIVE_ARM_TEXTURE, runtime2d);
        pthread_t thread;
        id<MTLTexture> foreign = nil;
        pthread_create(&thread, NULL, other_thread, device);
        pthread_join(thread, (void **)&foreign);
        p = call(MW_GFX_NATIVE_DISARM_TEXTURE, nil);
        CHECK(foreign && foreign != runtime2d && p.detail == MW_GFX_NATIVE_NOT_REACHED);
        [foreign release];
        t = [device newTextureWithDescriptor:descriptor(MTLStorageModePrivate, 1)];
        CHECK(t && t != runtime2d);
        [t release];
        printf("PASS capture-only, per-thread arming and unarmed pass-through\n");

        MTLHeapDescriptor *heap_descriptor = [[MTLHeapDescriptor alloc] init];
        heap_descriptor.size = 1 << 20;
        heap_descriptor.storageMode = MTLStorageModePrivate;
        id<MTLHeap> heap = [device newHeapWithDescriptor:heap_descriptor];
        [heap_descriptor release];
        call(MW_GFX_NATIVE_ARM_TEXTURE, runtime2d);
        t = [heap newTextureWithDescriptor:descriptor(MTLStorageModePrivate, 1)];
        p = call(MW_GFX_NATIVE_DISARM_TEXTURE, nil);
        CHECK(heap && t && t != runtime2d && p.detail == MW_GFX_NATIVE_HEAP_PLACEMENT);
        [t release]; [heap release];
        printf("PASS heap suballocation is reported and never substituted\n");

        call(MW_GFX_NATIVE_ARM_EVENT, runtime_event);
        id<MTLSharedEvent> event = [device newSharedEvent];
        p = call(MW_GFX_NATIVE_DISARM_EVENT, nil);
        CHECK(event == runtime_event && p.detail == MW_GFX_NATIVE_SUBSTITUTED);
        [event release];
        call(MW_GFX_NATIVE_ARM_EVENT, runtime_event);
        id<MTLEvent> private_event = [device newEvent];
        p = call(MW_GFX_NATIVE_DISARM_EVENT, nil);
        CHECK(private_event && p.detail == MW_GFX_NATIVE_PRIVATE_EVENT);
        [private_event release];
        p = (struct mw_gfx_native_params){MW_GFX_NATIVE_SIGNAL_EVENT};
        p.object = (uint64_t)runtime_event; p.value = 7;
        mw_graphics_native_call(&p);
        CHECK(p.status == 0 && runtime_event.signaledValue == 7);
        printf("PASS shared-event substitution, private-event report and CPU signal\n");

        p = call(MW_GFX_NATIVE_TEXTURE_INFO, runtime_iosurface);
        CHECK(p.status == 0 && p.iosurface && p.actual.width == 8 && p.actual.storage_mode == MTLStorageModeShared);
        p = call(MW_GFX_NATIVE_TEXTURE_INFO, runtime2d);
        CHECK(p.status == 0 && !p.iosurface && p.actual.storage_mode == MTLStorageModePrivate);
        p = call(MW_GFX_NATIVE_IDENTIFY, (id)(uintptr_t)&MTLCreateSystemDefaultDevice);
        CHECK(p.kind == MW_GFX_NATIVE_KIND_UNKNOWN && strstr(p.text, "Metal"));
        printf("PASS texture description and non-D3DMetal image identity (%s)\n", p.text);

        [runtime2d release]; [runtime_array release]; [runtime_iosurface release]; [runtime_event release];
        CFRelease(surface); [device release];
    }
    if (failures) { printf("FAILED %d check(s)\n", failures); return 1; }
    puts("PASS graphics interposer");
    return 0;
}

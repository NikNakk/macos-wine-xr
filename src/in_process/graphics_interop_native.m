// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Native half of the graphics interop layer: plain Metal object queries and
// operations requested by the PE backends. It knows no OpenXR state.
//
// The allocation interposer serves translation layers, such as Apple's
// D3DMetal, that have no API to wrap an existing MTLTexture. While a PE
// backend has armed this thread, the next interposed Metal allocation returns
// the armed runtime-owned object (if its descriptor is compatible), so the D3D
// resource created by that call refers to the same allocation. The technique
// follows utmapp/d3dmetal-native (MIT): no code or Apple binaries are copied.
// Unarmed threads and other processes are unaffected.
#if 0
#pragma makedep unix
#endif
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <pthread.h>
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

static void describe_request(MTLTextureDescriptor *request, struct mw_gfx_native_desc *desc)
{
    desc->width = request.width; desc->height = request.height; desc->array_length = request.arrayLength;
    desc->pixel_format = request.pixelFormat; desc->usage = request.usage; desc->storage_mode = request.storageMode;
    desc->texture_type = request.textureType; desc->mip_levels = request.mipmapLevelCount;
    desc->samples = request.sampleCount;
}

static char d3dmetal_image[256];

static void identify(struct mw_gfx_native_params *p)
{
    // A COM object's vtable lives in the image that implements it. DXMT's and
    // PE-thunked GPTK objects have PE vtables (no Mach-O image); a native
    // D3DMetal object's vtable is in its framework.
    Dl_info info = {0};
    p->kind = MW_GFX_NATIVE_KIND_UNKNOWN;
    if (p->object && dladdr((const void *)p->object, &info) && info.dli_fname) {
        snprintf(p->text, sizeof(p->text), "%s", info.dli_fname);
        if (strstr(info.dli_fname, "/D3DMetal.framework/")) p->kind = MW_GFX_NATIVE_KIND_D3DMETAL;
        else return;
    } else {
        for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
            const char *name = _dyld_get_image_name(i);
            if (name && strstr(name, "/D3DMetal.framework/")) {
                snprintf(p->text, sizeof(p->text), "%s", name);
                p->kind = MW_GFX_NATIVE_KIND_D3DMETAL_LOADED;
                break;
            }
        }
        if (!p->kind) return;
    }
    snprintf(d3dmetal_image, sizeof(d3dmetal_image), "%s", p->text);
}

// Per-thread arm state. Objects are borrowed from the PE caller for the
// duration of one arm/allocate/disarm sequence on this thread.
static __thread struct {
    int texture_armed, event_armed, event_via_private;
    id<MTLTexture> texture;   // nil: capture the runtime's own allocation
    id<MTLSharedEvent> event;
    id device, captured;      // captured is retained
    uint32_t texture_detail, event_detail;
    struct mw_gfx_native_desc requested;
} arm;

struct hook { Method method; IMP original; };
static struct hook hooks[32];
static unsigned hook_count;
static pthread_mutex_t hook_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *internal_heaps;  // D3DMDevice::UseInternalHeaps (GPTK 4), if exported
static unsigned char saved_internal_heaps;

// The nearest hooked implementation in self's class chain. A class that
// overrides the method and calls super reaches the hook with its own class.
static IMP original(id self, SEL selector)
{
    IMP result = NULL;
    pthread_mutex_lock(&hook_mutex);
    for (Class cls = object_getClass(self); cls && !result; cls = class_getSuperclass(cls)) {
        Method method = class_getInstanceMethod(cls, selector);
        for (unsigned i = 0; i < hook_count && !result; ++i)
            if (hooks[i].method == method) result = hooks[i].original;
    }
    pthread_mutex_unlock(&hook_mutex);
    return result;
}

static void install(Class cls, SEL selector, IMP replacement)
{
    Method method = class_getInstanceMethod(cls, selector);
    if (!method) return;
    pthread_mutex_lock(&hook_mutex);
    int present = 0;
    for (unsigned i = 0; i < hook_count; ++i) present |= hooks[i].method == method;
    if (!present && hook_count < sizeof(hooks) / sizeof(hooks[0]) && method_getImplementation(method) != replacement) {
        hooks[hook_count].method = method;
        hooks[hook_count].original = method_setImplementation(method, replacement);
        ++hook_count;
    }
    pthread_mutex_unlock(&hook_mutex);
}

static uint32_t check_substitute(id device, MTLTextureDescriptor *request, id<MTLTexture> texture)
{
    if (device != texture.device && [(id<MTLDevice>)device registryID] != texture.device.registryID)
        return MW_GFX_NATIVE_MISMATCH_DEVICE;
    if (request.textureType != texture.textureType || request.pixelFormat != texture.pixelFormat ||
        request.width != texture.width || request.height != texture.height || request.depth != texture.depth ||
        request.mipmapLevelCount != texture.mipmapLevelCount || request.sampleCount != texture.sampleCount ||
        request.arrayLength != texture.arrayLength)
        return MW_GFX_NATIVE_MISMATCH_DESCRIPTOR;
    if (request.usage & ~texture.usage) return MW_GFX_NATIVE_MISMATCH_USAGE;
    // d3dmetal-native found D3DMetal 3.0 zero-fills new Shared-storage textures
    // through -[texture heap]; a heapless runtime texture would crash it.
    // D3DMetal 4.0b2 requests Shared for DEFAULT textures but only uses them on
    // the GPU; a Private runtime texture serves that request (verified under
    // Metal validation, which would assert on any CPU access).
    BOOL private_for_shared = request.storageMode == MTLStorageModeShared && texture.storageMode == MTLStorageModePrivate;
    if ((request.storageMode != texture.storageMode && !private_for_shared) ||
        (texture.storageMode == MTLStorageModeShared && !texture.heap))
        return MW_GFX_NATIVE_MISMATCH_STORAGE;
    return MW_GFX_NATIVE_SUBSTITUTED;
}

static id hook_device_texture(id self, SEL selector, MTLTextureDescriptor *request)
{
    IMP next = original(self, selector);
    if (arm.texture_armed) {
        arm.texture_armed = 0;
        arm.device = self;
        describe_request(request, &arm.requested);
        if (arm.texture) {
            arm.texture_detail = check_substitute(self, request, arm.texture);
            if (arm.texture_detail == MW_GFX_NATIVE_SUBSTITUTED) return [arm.texture retain];
            // Let the runtime allocate normally; the PE backend discards the result.
        } else {
            id texture = ((id (*)(id, SEL, MTLTextureDescriptor *))next)(self, selector, request);
            arm.captured = [texture retain];
            arm.texture_detail = texture ? MW_GFX_NATIVE_CAPTURED : MW_GFX_NATIVE_NOT_REACHED;
            return texture;
        }
    }
    return ((id (*)(id, SEL, MTLTextureDescriptor *))next)(self, selector, request);
}

static id hook_heap_texture(id self, SEL selector, MTLTextureDescriptor *request)
{
    if (arm.texture_armed) { arm.texture_armed = 0; arm.texture_detail = MW_GFX_NATIVE_HEAP_PLACEMENT; }
    return ((id (*)(id, SEL, MTLTextureDescriptor *))original(self, selector))(self, selector, request);
}

static id hook_heap_texture_offset(id self, SEL selector, MTLTextureDescriptor *request, NSUInteger offset)
{
    if (arm.texture_armed) { arm.texture_armed = 0; arm.texture_detail = MW_GFX_NATIVE_HEAP_PLACEMENT; }
    return ((id (*)(id, SEL, MTLTextureDescriptor *, NSUInteger))original(self, selector))(self, selector, request, offset);
}

static id hook_device_heap(id self, SEL selector, MTLHeapDescriptor *request)
{
    id heap = ((id (*)(id, SEL, MTLHeapDescriptor *))original(self, selector))(self, selector, request);
    if (heap) {
        install(object_getClass(heap), @selector(newTextureWithDescriptor:), (IMP)hook_heap_texture);
        install(object_getClass(heap), @selector(newTextureWithDescriptor:offset:), (IMP)hook_heap_texture_offset);
    }
    return heap;
}

static id hook_device_shared_event(id self, SEL selector)
{
    if (arm.event_armed) {
        arm.event_armed = 0;
        arm.device = self;
        // MTLSharedEvent.device is legitimately nil outside the validation layer.
        id<MTLDevice> owner = arm.event.device;
        if (!owner || self == owner || [(id<MTLDevice>)self registryID] == owner.registryID) {
            arm.event_detail = MW_GFX_NATIVE_SUBSTITUTED;
            return [arm.event retain];
        }
        arm.event_detail = MW_GFX_NATIVE_MISMATCH_DEVICE;
    }
    return ((id (*)(id, SEL))original(self, selector))(self, selector);
}

static id hook_device_event(id self, SEL selector)
{
    // D3DMetal 4.0b2 backs ID3D11Fence with a plain MTLEvent. An MTLSharedEvent
    // is an MTLEvent, so the fence's GPU signals land on the session event.
    if (arm.event_armed) {
        arm.event_armed = 0;
        arm.device = self;
        arm.event_detail = MW_GFX_NATIVE_SUBSTITUTED;
        arm.event_via_private = 1;
        return [arm.event retain];
    }
    return ((id (*)(id, SEL))original(self, selector))(self, selector);
}

static void interpose(struct mw_gfx_native_params *p)
{
    NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
    id<MTLDevice> preferred = MTLCreateSystemDefaultDevice();
    NSMutableArray *all = [NSMutableArray arrayWithArray:devices];
    if (preferred) [all addObject:preferred];
    for (id device in all) {
        Class cls = object_getClass(device);
        install(cls, @selector(newTextureWithDescriptor:), (IMP)hook_device_texture);
        install(cls, @selector(newHeapWithDescriptor:), (IMP)hook_device_heap);
        install(cls, @selector(newSharedEvent), (IMP)hook_device_shared_event);
        install(cls, @selector(newEvent), (IMP)hook_device_event);
    }
    [devices release]; [preferred release];
    if (d3dmetal_image[0] && !internal_heaps) {
        void *image = dlopen(d3dmetal_image, RTLD_LAZY | RTLD_NOLOAD);
        if (image) internal_heaps = dlsym(image, "_ZN10D3DMDevice16UseInternalHeapsE");
    }
    p->value = hook_count;
    p->detail = internal_heaps != NULL;  // whether the GPTK 4 heap-pool switch exists
    if (!hook_count) p->status = -1;
}

static void arm_texture(struct mw_gfx_native_params *p)
{
    [arm.captured release];
    arm.captured = nil; arm.device = nil;
    memset(&arm.requested, 0, sizeof(arm.requested));
    arm.texture = (id<MTLTexture>)p->object;
    arm.texture_detail = hook_count ? MW_GFX_NATIVE_NOT_REACHED : MW_GFX_NATIVE_UNSUPPORTED;
    arm.texture_armed = hook_count != 0;
    // GPTK 4 suballocates from internal heaps; request a dedicated allocation.
    if (internal_heaps) { saved_internal_heaps = *internal_heaps; *internal_heaps = 0; }
}

static void disarm_texture(struct mw_gfx_native_params *p)
{
    if (internal_heaps) *internal_heaps = saved_internal_heaps;
    arm.texture_armed = 0;
    p->detail = arm.texture_detail;
    p->device = (uint64_t)arm.device;
    p->requested = arm.requested;
    if (arm.texture) describe(arm.texture, &p->actual);
    p->object = (uint64_t)arm.captured;  // ownership passes to the caller
    arm.captured = nil; arm.texture = nil;
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
        case MW_GFX_NATIVE_INTERPOSE: interpose(p); break;
        case MW_GFX_NATIVE_ARM_TEXTURE: arm_texture(p); break;
        case MW_GFX_NATIVE_DISARM_TEXTURE: disarm_texture(p); break;
        case MW_GFX_NATIVE_ARM_EVENT:
            arm.event_via_private = 0;
            arm.event = (id<MTLSharedEvent>)p->object;
            arm.event_detail = hook_count ? MW_GFX_NATIVE_NOT_REACHED : MW_GFX_NATIVE_UNSUPPORTED;
            arm.event_armed = hook_count && arm.event;
            break;
        case MW_GFX_NATIVE_DISARM_EVENT:
            arm.event_armed = 0; arm.event = nil;
            p->detail = arm.event_detail;
            p->value = arm.event_via_private;  // substituted through newEvent
            p->device = (uint64_t)arm.device;
            break;
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

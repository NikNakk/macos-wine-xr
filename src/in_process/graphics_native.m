// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Native graphics adaptation: Khronos Metal binding, runtime-owned images,
// and event ordering before Monado's release-queue completion barrier.
#if 0
#pragma makedep unix
#endif
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#undef LoadResource
#define LoadResource WINE_LoadResource
#define BOOL WINE_BOOL
#include "openxr_private.h"
#undef BOOL
#undef LoadResource
#include <stdio.h>
#include "graphics_vulkan.h"
static wine_XrSession *active_session;
static const int64_t dxgi_formats[] = {28, 29, 87, 91, 10};
static const int64_t vulkan_formats[] = {37, 43, 44, 50, 97};
static const int64_t metal_formats[] = {70, 71, 80, 81, 112};
static int64_t map_format(int64_t format, int to_metal, int vulkan)
{
    const int64_t *app_formats = vulkan ? vulkan_formats : dxgi_formats;
    for (unsigned i = 0; i < ARRAY_SIZE(dxgi_formats); ++i)
        if (format == (to_metal ? app_formats[i] : metal_formats[i]))
            return to_metal ? metal_formats[i] : app_formats[i];
    return 0;
}
void mw_native_instance_cleanup(wine_XrInstance *instance)
{
    @autoreleasepool { [(id)instance->required_metal_device release]; }
    instance->required_metal_device = NULL;
}
static XrResult native_graphics_requirements(XrInstance instance, XrSystemId system)
{
    wine_XrInstance *wrapper = wine_instance_from_handle(instance);
    XrGraphicsRequirementsMetalKHR native = {XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR};
    XrResult result = g_xr_host_instance_dispatch_table.p_xrGetMetalGraphicsRequirementsKHR(
        wrapper->host_instance, system, &native);
    if (XR_SUCCEEDED(result)) @autoreleasepool {
        [(id)wrapper->required_metal_device release];
        wrapper->required_metal_device = [(id)native.metalDevice retain];
    }
    return result;
}
XrResult wine_xrGetD3D11GraphicsRequirementsKHR(XrInstance instance, XrSystemId system,
                                              XrGraphicsRequirementsD3D11KHR *requirements)
{
    return native_graphics_requirements(instance, system);
}
XrResult wine_xrCreateSession(XrInstance instance, const XrSessionCreateInfo *info, XrSession *session, void *wrapper_ptr)
{
    wine_XrSession *wrapper = wrapper_ptr;
    wine_XrInstance *parent = wine_instance_from_handle(instance);
    if (active_session) return XR_ERROR_LIMIT_REACHED;
    if (!parent->required_metal_device) return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
    @autoreleasepool {
        id<MTLDevice> device = (id<MTLDevice>)wrapper->metal_device;
        if (!device || device.registryID != [(id<MTLDevice>)parent->required_metal_device registryID])
            return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLSharedEvent> event = [device newSharedEvent];
        if (!queue || !event) { [queue release]; [event release]; return XR_ERROR_OUT_OF_MEMORY; }
        XrGraphicsBindingMetalKHR binding = {XR_TYPE_GRAPHICS_BINDING_METAL_KHR, NULL, queue};
        XrSessionCreateInfo native = *info;
        native.next = &binding;
        XrResult result = g_xr_host_instance_dispatch_table.p_xrCreateSession(parent->host_instance, &native, session);
        if (XR_FAILED(result)) { [queue release]; [event release]; return result; }
        wrapper->metal_queue = queue;
        wrapper->metal_event = event;
        active_session = wrapper;
        fprintf(stderr, "wineopenxr: native Metal session queue=%p completion-event=%p\n", queue, event);
        return result;
    }
}
static void gaze_forget_session(wine_XrSession *session);
XrResult wine_xrDestroySession(XrSession session)
{
    wine_XrSession *wrapper = wine_session_from_handle(session);
    gaze_forget_session(wrapper); // its foveation profile dies with the runtime session
    XrResult result = g_xr_host_instance_dispatch_table.p_xrDestroySession(wrapper->host_session);
    if (XR_SUCCEEDED(result)) @autoreleasepool {
        [(id)wrapper->metal_queue release]; [(id)wrapper->metal_event release];
        wrapper->metal_queue = wrapper->metal_event = NULL;
        active_session = NULL;
    }
    return result;
}
XrResult wine_xrEnumerateSwapchainFormats(XrSession session, uint32_t capacity, uint32_t *count, int64_t *formats)
{
    if (!count || (capacity && !formats)) return XR_ERROR_VALIDATION_FAILURE;
    wine_XrSession *wrapper = wine_session_from_handle(session);
    uint32_t native_count = 0;
    XrResult result = g_xr_host_instance_dispatch_table.p_xrEnumerateSwapchainFormats(wrapper->host_session, 0, &native_count, NULL);
    if (XR_FAILED(result)) return result;
    int64_t *native = calloc(native_count, sizeof(*native));
    if (!native) return XR_ERROR_OUT_OF_MEMORY;
    result = g_xr_host_instance_dispatch_table.p_xrEnumerateSwapchainFormats(wrapper->host_session, native_count, &native_count, native);
    *count = 0;
    if (XR_SUCCEEDED(result)) for (uint32_t i = 0; i < native_count; ++i) {
        int64_t mapped = map_format(native[i], 0, wrapper->vulkan);
        if (mapped) {
            if (formats && *count < capacity) formats[*count] = mapped;
            ++*count;
        }
    }
    free(native);
    if (XR_SUCCEEDED(result) && capacity && capacity < *count) return XR_ERROR_SIZE_INSUFFICIENT;
    return result;
}
XrResult wine_xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo *info, XrSwapchain *swapchain)
{
    fprintf(stderr, "wineopenxr: swapchain %ux%u array=%u samples=%u usage=%#llx format=%lld\n", info->width,
            info->height, info->arraySize, info->sampleCount, (unsigned long long)info->usageFlags, (long long)info->format);
    XrSwapchainCreateInfo native = *info;
    native.format = map_format(info->format, 1, wine_session_from_handle(session)->vulkan);
    if (!native.format) return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    const XrSwapchainCreateInfoFoveationFB *foveation = info->next;
    bool valid_next = !foveation ||
        ((wine_session_from_handle(session)->instance->foveation_extensions & (1u << 1)) &&
         foveation->type == XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB && !foveation->next);
    if (!valid_next || info->faceCount != 1 || info->sampleCount != 1 || info->mipCount != 1 || !info->arraySize ||
        (info->usageFlags & ~(XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                             XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                             XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT))) {
        fprintf(stderr, "wineopenxr: unsupported color swapchain next=%p faces=%u samples=%u mips=%u array=%u usage=0x%llx\n",
                info->next, info->faceCount, info->sampleCount, info->mipCount, info->arraySize,
                (unsigned long long)info->usageFlags);
        return XR_ERROR_FEATURE_UNSUPPORTED;
    }
    // OpenComposite requests transfer-destination usage for its OpenVR submit
    // operation. Preserve the flag; importing these runtime-owned images still
    // introduces no copy in the D3D11-to-Metal runtime bridge.
    // Gaze foveation updates this swapchain's runtime foveation state; it never selects a
    // runtime map, so submitted images stay ordinary for the compositor.
    XrSwapchainCreateInfoFoveationFB gaze_info = {XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB};
    if (wine_session_from_handle(session)->instance->gaze_foveation == 1 && !native.next) native.next = &gaze_info;
    return g_xr_host_instance_dispatch_table.p_xrCreateSwapchain(wine_session_from_handle(session)->host_session, &native, swapchain);
}

int32_t mw_foveation_call(void *args)
{
    struct mw_foveation_params *p = args;
    p->map = 0;
    wine_XrSwapchain *swap = wine_swapchain_from_handle(p->swapchain);
    wine_XrInstance *instance = swap->session->instance;
    if (!instance->foveation_d3d11_enabled || !instance->native_get_foveation_metal) {
        p->result = XR_ERROR_FUNCTION_UNSUPPORTED;
        return 0;
    }
    XrFoveationMetalStateMNDX state = {XR_TYPE_FOVEATION_METAL_STATE_MNDX};
    p->result = ((PFN_xrGetFoveationMetalStateMNDX)instance->native_get_foveation_metal)(
        swap->host_swapchain, p->view_index, 0, &state);
    if (XR_SUCCEEDED(p->result)) {
        p->state->foveationEnabled = state.foveationEnabled;
        p->state->physicalWidth = state.physicalWidth;
        p->state->physicalHeight = state.physicalHeight;
        p->state->revision = state.revision;
        p->map = (uint64_t)state.rasterizationRateMap;
    }
    return 0;
}
// MWXR_CAPTURE_DIR=dir MWXR_CAPTURE_EVERY=n [MWXR_CAPTURE_MAX=m]: every n-th release of a Vulkan session's
// image, up to six times, write each array layer as dir/eye-K-L.png (debugging only;
// it waits for the GPU).
#pragma push_macro("interface")
#undef interface
#import <CoreGraphics/CGBitmapContext.h>
#import <ImageIO/CGImageDestination.h>
#pragma pop_macro("interface")
static void capture_release(id<MTLCommandQueue> queue, id<MTLTexture> texture)
{
    static const char *dir; static int every = -1, taken, most = 6; static unsigned count;
    if (every < 0) {
        const char *e = getenv("MWXR_CAPTURE_EVERY"); dir = getenv("MWXR_CAPTURE_DIR");
        every = dir && *dir && e ? atoi(e) : 0;
        const char *m = getenv("MWXR_CAPTURE_MAX"); // default 6
        if (m && atoi(m) > 0) most = atoi(m);
    }
    if (every <= 0 || !texture || taken >= most || ++count % every) return;
    MTLPixelFormat f = texture.pixelFormat;
    BOOL bgra = f == MTLPixelFormatBGRA8Unorm || f == MTLPixelFormatBGRA8Unorm_sRGB;
    if (!bgra && f != MTLPixelFormatRGBA8Unorm && f != MTLPixelFormatRGBA8Unorm_sRGB) return;
    ++taken;
    NSUInteger w = texture.width, h = texture.height, layers = texture.arrayLength, row = w * 4;
    id<MTLBuffer> buffer = [texture.device newBufferWithLength:row * h * layers options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    for (NSUInteger l = 0; l < layers; ++l)
        [blit copyFromTexture:texture sourceSlice:l sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(w, h, 1) toBuffer:buffer destinationOffset:l * row * h
      destinationBytesPerRow:row destinationBytesPerImage:row * h];
    [blit endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    for (NSUInteger l = 0; l < layers; ++l) {
        CGBitmapInfo info = bgra ? (kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst)
                                 : (kCGBitmapByteOrder32Big | kCGImageAlphaNoneSkipLast);
        CGContextRef ctx = CGBitmapContextCreate((char *)buffer.contents + l * row * h, w, h, 8, row, space, info);
        CGImageRef image = CGBitmapContextCreateImage(ctx);
        NSString *path = [NSString stringWithFormat:@"%s/eye-%d-%lu.png", dir, taken, (unsigned long)l];
        CGImageDestinationRef dest = CGImageDestinationCreateWithURL((CFURLRef)[NSURL fileURLWithPath:path],
                                                                     CFSTR("public.png"), 1, NULL);
        if (dest) { CGImageDestinationAddImage(dest, image, NULL); CGImageDestinationFinalize(dest); CFRelease(dest); }
        CGImageRelease(image); CGContextRelease(ctx);
    }
    CGColorSpaceRelease(space);
    [buffer release];
    fprintf(stderr, "wineopenxr: captured %lux%lu x%lu to %s (eye-%d-*)\n", (unsigned long)w, (unsigned long)h,
            (unsigned long)layers, dir, taken);
}
XrResult wine_xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo *info)
{
    wine_XrSwapchain *wrapper = wine_swapchain_from_handle(swapchain);
    wine_XrSession *session = wrapper->session;
    @autoreleasepool {
        id<MTLCommandBuffer> buffer = [(id<MTLCommandQueue>)session->metal_queue commandBuffer];
        // D3D11 signals the event from DXMT; Vulkan from the application's queue (a timeline
        // semaphore on the same event). A Vulkan session still on the CPU fence wait has
        // fence_value 0, which is already reached.
        [buffer encodeWaitForEvent:(id<MTLSharedEvent>)session->metal_event value:session->fence_value];
        if (wrapper->copy_source) {
            // Explicit, logged graphics copy fallback only; zero-copy backends never set it.
            id<MTLBlitCommandEncoder> blit = [buffer blitCommandEncoder];
            [blit copyFromTexture:(id<MTLTexture>)wrapper->copy_source toTexture:(id<MTLTexture>)wrapper->copy_target];
            [blit endEncoding];
        }
        [buffer commit];
        if (wrapper->capture_texture) capture_release((id<MTLCommandQueue>)session->metal_queue, (id<MTLTexture>)wrapper->capture_texture);
        // Monado commits and completes an empty command buffer on this same
        // queue in release, so it waits for the earlier DXMT producer signal.
        return g_xr_host_instance_dispatch_table.p_xrReleaseSwapchainImage(wrapper->host_swapchain, info);
    }
}

// --- MWXR_GAZE_FOVEATION="outer[,core,edge[,step]]" -------------------------------------
// One separable rate map over the packed stereo target, in angular terms through each
// view's own FOV: full rate within `core` degrees of the centre, smoothstep to `outer` at
// `edge` degrees. The centre is Monado's private gaze (XR_META_foveation_eye_tracked),
// quantised to `step` degrees. Without valid gaze the centre is straight ahead with the
// wider fixed profile (20/35 degrees), as DXMT_STAY_WARPED_FIXED_PROFILE=psvr2 does.
// MWXR_GAZE_FOVEATION_FIXED=1 forces that; MWXR_GAZE_FOVEATION_TEST=1 sweeps a synthetic gaze
// (2 holds it straight ahead, for timing a profile).
#include <math.h>
static struct {
    wine_XrSession *session; XrSession host_session; XrFoveationProfileFB profile;
    int eye_tracked, configured, force_fixed, test;
    float outer, core, edge, step, yaw, pitch; int have_gaze, mode; unsigned invalid_calls, built; uint64_t calls;
    NSMutableDictionary *maps; uint64_t current; int valid_logged;
    uint32_t logged_width, logged_height; int32_t logged_extent;
} gaze;
// A session's profile belongs to its runtime session. NMS (through xrizer) destroys its first
// session and creates another, which can reuse the wrapper's address: the state must not
// outlive the session, or every update names a destroyed profile (XR_ERROR_HANDLE_INVALID).
static void gaze_forget_session(wine_XrSession *session)
{
    if (gaze.session != session) return;
    if (gaze.profile) g_xr_host_instance_dispatch_table.p_xrDestroyFoveationProfileFB(gaze.profile);
    @autoreleasepool { [gaze.maps release]; }
    memset(&gaze, 0, sizeof(gaze));
}
static float gaze_rate(float delta, float core, float edge, float outer)
{
    float t = (fabsf(delta) - core) / fmaxf(edge - core, .01f);
    t = fminf(1.f, fmaxf(0.f, t));
    t = t * t * (3.f - 2.f * t);
    return 1.f + (outer - 1.f) * t;
}
// MWXR_KK_FOVEATION=1|flip: also give the profile to KosmicKrisp in this process, for a
// Vulkan or OpenGL (Zink) application that renders its eyes itself (KK_IMPLICIT_FDM=auto;
// docs/kk-implicit-foveation.md). Per eye, so it fits a per-eye or a side-by-side target.
// "flip": the application's images are bottom-up (OpenGL).
#include <dlfcn.h>
#include <mach-o/dyld.h>
static void gaze_push_kosmickrisp(const struct mw_gaze_foveation_params *p, float yaw, float pitch,
                                  float core, float edge)
{
    enum { count = 64 };
    typedef void (*set_profile_fn)(uint32_t, uint32_t, uint32_t, const float *, const float *, const float *);
    static set_profile_fn set_profile;
    static int mode = -1, pushed;
    static unsigned attempts;
    static float last[5];
    if (mode < 0) {
        const char *text = getenv("MWXR_KK_FOVEATION");
        mode = !text ? 0 : !strcmp(text, "flip") ? 2 : !strcmp(text, "1") ? 1 : 0;
    }
    if (!mode || p->rects[0].extent.width <= 0 || p->rects[0].extent.height <= 0) return;
    if (!set_profile) {
        // The Vulkan loader opens the driver privately, so find it by its image name.
        if (attempts++ % 300) return;
        for (uint32_t i = 0; i < _dyld_image_count() && !set_profile; ++i) {
            const char *name = _dyld_get_image_name(i);
            if (!name || !strstr(name, "libvulkan_kosmickrisp")) continue;
            void *handle = dlopen(name, RTLD_LAZY | RTLD_NOLOAD);
            if (handle) set_profile = (set_profile_fn)dlsym(handle, "kk_implicit_fdm_set_profile");
        }
        if (!set_profile) {
            if (attempts == 1) fprintf(stderr, "wineopenxr: KosmicKrisp foveation: driver not loaded yet\n");
            return;
        }
        fprintf(stderr, "wineopenxr: KosmicKrisp foveation: driver found%s\n", mode == 2 ? ", bottom-up images" : "");
    }
    const float now[5] = {yaw, pitch, core, edge, (float)p->rects[0].extent.width * 4096.f + p->rects[0].extent.height};
    if (pushed && !memcmp(now, last, sizeof(now))) return;
    memcpy(last, now, sizeof(now));
    const float deg = 57.29577951308232f;
    float horizontal[2][count], vertical[count];
    for (unsigned i = 0; i < count; ++i) {
        float rate = gaze.outer;
        for (unsigned v = 0; v < 2; ++v) {
            float l = tanf(p->fovs[v].angleLeft), r = tanf(p->fovs[v].angleRight);
            float angle = atanf(l + (i + .5f) / count * (r - l)) * deg;
            horizontal[v][i] = gaze_rate(angle - yaw, core, edge, gaze.outer);
            float u = tanf(p->fovs[v].angleUp), d = tanf(p->fovs[v].angleDown);
            angle = atanf(u - (i + .5f) / count * (u - d)) * deg;
            rate = fmaxf(rate, gaze_rate(angle - pitch, core, edge, gaze.outer));
        }
        vertical[mode == 2 ? count - 1 - i : i] = rate;
    }
    set_profile((uint32_t)p->rects[0].extent.width, (uint32_t)p->rects[0].extent.height, count,
                horizontal[0], horizontal[1], vertical);
    if (!pushed++) fprintf(stderr, "wineopenxr: KosmicKrisp foveation: view %dx%d, first profile sent\n",
                           p->rects[0].extent.width, p->rects[0].extent.height);
}
static id<MTLRasterizationRateMap> gaze_build(id<MTLDevice> device, const struct mw_gaze_foveation_params *p,
                                              float yaw, float pitch, float core, float edge)
{
    enum { columns = 64, rows = 32 };
    const float deg = 57.29577951308232f;
    MTLRasterizationRateLayerDescriptor *layer =
        [[MTLRasterizationRateLayerDescriptor alloc] initWithSampleCount:MTLSizeMake(columns, rows, 1)];
    for (unsigned i = 0; i < columns; ++i) {
        float x = (i + .5f) / columns * p->width, rate = gaze.outer;
        for (unsigned v = 0; v < 2; ++v) {
            const XrRect2Di *r = &p->rects[v];
            if (x < r->offset.x || x >= r->offset.x + r->extent.width) continue;
            float l = tanf(p->fovs[v].angleLeft), rt = tanf(p->fovs[v].angleRight);
            float angle = atanf(l + (x - r->offset.x) / r->extent.width * (rt - l)) * deg;
            rate = fmaxf(rate, gaze_rate(angle - yaw, core, edge, gaze.outer));
        }
        layer.horizontal[i] = @(rate);
    }
    for (unsigned j = 0; j < rows; ++j) {
        float y = (j + .5f) / rows * p->height, rate = gaze.outer;
        for (unsigned v = 0; v < 2; ++v) {
            const XrRect2Di *r = &p->rects[v];
            if (y < r->offset.y || y >= r->offset.y + r->extent.height) continue;
            float u = tanf(p->fovs[v].angleUp), d = tanf(p->fovs[v].angleDown);
            float angle = atanf(u - (y - r->offset.y) / r->extent.height * (u - d)) * deg;
            rate = fmaxf(rate, gaze_rate(angle - pitch, core, edge, gaze.outer));
        }
        layer.vertical[j] = @(rate);
    }
    MTLRasterizationRateMapDescriptor *desc = [MTLRasterizationRateMapDescriptor
        rasterizationRateMapDescriptorWithScreenSize:MTLSizeMake(p->width, p->height, 1) layer:layer];
    id<MTLRasterizationRateMap> map = [device newRasterizationRateMapWithDescriptor:desc];
    [layer release];
    return map;
}
int32_t mw_gaze_foveation_call(void *args)
{
    struct mw_gaze_foveation_params *p = args;
    p->map = 0; p->result = XR_SUCCESS;
    wine_XrSwapchain *swap = wine_swapchain_from_handle(p->swapchain);
    wine_XrSession *session = swap->session;
    id<MTLDevice> device = (id<MTLDevice>)session->metal_device;
    if (!session->instance->gaze_foveation || !device || !p->width || !p->height ||
        ![device supportsRasterizationRateMapWithLayerCount:1]) return 0;
    @autoreleasepool {
        if (gaze.session != session || gaze.host_session != session->host_session) {
            if (gaze.profile && gaze.host_session == session->host_session)
                g_xr_host_instance_dispatch_table.p_xrDestroyFoveationProfileFB(gaze.profile);
            [gaze.maps release];
            memset(&gaze, 0, sizeof(gaze));
            gaze.session = session; gaze.host_session = session->host_session;
            gaze.maps = [NSMutableDictionary new];
            gaze.outer = .3f; gaze.core = 12.f; gaze.edge = 27.f; gaze.step = 2.f;
            sscanf(getenv("MWXR_GAZE_FOVEATION"), "%f,%f,%f,%f", &gaze.outer, &gaze.core, &gaze.edge, &gaze.step);
            gaze.outer = fminf(1.f, fmaxf(.1f, gaze.outer)); gaze.step = fminf(10.f, fmaxf(.25f, gaze.step));
            gaze.core = fmaxf(1.f, gaze.core); gaze.edge = fmaxf(gaze.core + 1.f, gaze.edge);
            const char *text = getenv("MWXR_GAZE_FOVEATION_FIXED");
            gaze.force_fixed = (text && !strcmp(text, "1")) || session->instance->gaze_foveation != 1;
            text = getenv("MWXR_GAZE_FOVEATION_TEST");
            gaze.test = text && !strcmp(text, "1") ? 1 : text && !strcmp(text, "2") ? 2 : 0; // 2: held straight ahead
            const char *fixed = getenv("MWXR_GAZE_FOVEATION_FIXED");
            if (fixed && !strcmp(fixed, "1")) gaze.test = 0;
            if (!gaze.force_fixed && !gaze.test) {
                XrFoveationEyeTrackedProfileCreateInfoMETA eye = {XR_TYPE_FOVEATION_EYE_TRACKED_PROFILE_CREATE_INFO_META};
                XrFoveationLevelProfileCreateInfoFB level = {XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB, &eye,
                    XR_FOVEATION_LEVEL_HIGH_FB, 0.f, XR_FOVEATION_DYNAMIC_DISABLED_FB};
                XrFoveationProfileCreateInfoFB info = {XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB, &level};
                XrResult r = g_xr_host_instance_dispatch_table.p_xrCreateFoveationProfileFB(
                    session->host_session, &info, &gaze.profile);
                gaze.eye_tracked = XR_SUCCEEDED(r);
                if (!gaze.eye_tracked) gaze.profile = XR_NULL_HANDLE;
            }
            fprintf(stderr, "wineopenxr: gaze foveation outer=%.2f core=%.1f edge=%.1f step=%.2f source=%s\n", gaze.outer,
                    gaze.core, gaze.edge, gaze.step, gaze.test ? "synthetic" : gaze.eye_tracked ? "runtime gaze" : "fixed");
        }
        const float deg = 57.29577951308232f;
        int valid = 0; float yaw = 0, pitch = 0;
        if (gaze.test) {
            double t = gaze.calls / 90.0;
            yaw = 25.f * sinf(t * 2.1); pitch = 15.f * sinf(t * 1.3); valid = 1;
            if (gaze.test == 2) yaw = pitch = 0; // the given core and edge, without the sweep
        } else if (gaze.eye_tracked) {
            // Each update makes the runtime resample its private gaze for this swapchain.
            XrSwapchainStateFoveationFB update = {XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB, NULL, 0, gaze.profile};
            XrFoveationEyeTrackedStateMETA state = {XR_TYPE_FOVEATION_EYE_TRACKED_STATE_META};
            if (XR_SUCCEEDED(g_xr_host_instance_dispatch_table.p_xrUpdateSwapchainFB(
                    swap->host_swapchain, (const XrSwapchainStateBaseHeaderFB *)&update)) &&
                XR_SUCCEEDED(g_xr_host_instance_dispatch_table.p_xrGetFoveationEyeTrackedStateMETA(
                    session->host_session, &state)) &&
                (state.flags & XR_FOVEATION_EYE_TRACKED_STATE_VALID_BIT_META)) {
                // The centre is NDC within view 0's tangent-space FOV; one gaze direction serves both eyes.
                float l = tanf(p->fovs[0].angleLeft), r = tanf(p->fovs[0].angleRight);
                float d = tanf(p->fovs[0].angleDown), u = tanf(p->fovs[0].angleUp);
                yaw = atanf(l + (state.foveationCenter[0].x + 1.f) * .5f * (r - l)) * deg;
                pitch = atanf(d + (state.foveationCenter[0].y + 1.f) * .5f * (u - d)) * deg;
                valid = isfinite(yaw) && isfinite(pitch);
            }
        }
        ++gaze.calls;
        if (valid) {
            gaze.invalid_calls = 0;
            // Hysteresis: keep the quantised centre until the gaze leaves its cell by a margin.
            if (!gaze.have_gaze || fabsf(yaw - gaze.yaw) > .6f * gaze.step || fabsf(pitch - gaze.pitch) > .6f * gaze.step) {
                gaze.yaw = fminf(45.f, fmaxf(-45.f, roundf(yaw / gaze.step) * gaze.step));
                gaze.pitch = fminf(45.f, fmaxf(-45.f, roundf(pitch / gaze.step) * gaze.step));
            }
            gaze.have_gaze = 1;
        } else if (gaze.have_gaze && ++gaze.invalid_calls > 30) {
            gaze.have_gaze = 0; // a blink keeps the last centre; longer loss falls back to fixed
        }
        if (valid != gaze.valid_logged || gaze.calls % 1800 == 1) {
            gaze.valid_logged = valid;
            fprintf(stderr, "wineopenxr: gaze foveation call=%llu valid=%d centre=(%+.1f,%+.1f) maps=%u\n",
                    (unsigned long long)gaze.calls, valid, gaze.yaw, gaze.pitch, gaze.built);
        }
        float core = gaze.have_gaze ? gaze.core : 20.f, edge = gaze.have_gaze ? gaze.edge : 35.f;
        float cy = gaze.have_gaze ? gaze.yaw : 0.f, cp = gaze.have_gaze ? gaze.pitch : 0.f;
        gaze_push_kosmickrisp(p, cy, cp, core, edge);
        NSString *key = [NSString stringWithFormat:@"%d,%d,%d,%u,%u,%d,%d,%d,%d,%d,%d,%d,%d", (int)lroundf(cy * 8),
                         (int)lroundf(cp * 8), gaze.have_gaze, p->width, p->height,
                         p->rects[0].offset.x, p->rects[0].offset.y, p->rects[0].extent.width, p->rects[0].extent.height,
                         p->rects[1].offset.x, p->rects[1].offset.y, p->rects[1].extent.width, p->rects[1].extent.height];
        if (p->width != gaze.logged_width || p->height != gaze.logged_height || p->rects[0].extent.width != gaze.logged_extent) {
            gaze.logged_width = p->width; gaze.logged_height = p->height; gaze.logged_extent = p->rects[0].extent.width;
            fprintf(stderr, "wineopenxr: gaze foveation target %ux%u eyes (%d,%d %dx%d) (%d,%d %dx%d)\n", p->width, p->height,
                    p->rects[0].offset.x, p->rects[0].offset.y, p->rects[0].extent.width, p->rects[0].extent.height,
                    p->rects[1].offset.x, p->rects[1].offset.y, p->rects[1].extent.width, p->rects[1].extent.height);
        }
        id<MTLRasterizationRateMap> map = gaze.maps[key];
        if (!map) {
            if (gaze.maps.count >= 256) [gaze.maps removeAllObjects]; // the translation layer retains what it uses
            map = gaze_build(device, p, cy, cp, core, edge);
            if (map) { gaze.maps[key] = map; [map release]; ++gaze.built; }
        }
        p->map = (uint64_t)map;
    }
    return 0;
}

// Vulkan graphics queries use Metal identities; no native Vulkan handles enter here.
int32_t mw_vulkan_native_call(void *args)
{
    struct mw_vulkan_native_params *p = args;
    p->result = XR_ERROR_GRAPHICS_DEVICE_INVALID;
    @autoreleasepool {
        if (p->op == MW_VK_REQUIREMENTS) {
            p->result = native_graphics_requirements((XrInstance)p->instance, p->system);
        } else if (p->op == MW_VK_MATCH_DEVICE) {
            if (p->instance->required_metal_device && p->object &&
                [(id<MTLDevice>)(uintptr_t)p->object registryID] ==
                [(id<MTLDevice>)p->instance->required_metal_device registryID]) p->result = XR_SUCCESS;
        } else if (p->op == MW_VK_TEXTURE_TYPE && p->object) {
            id<MTLTexture> texture = (id<MTLTexture>)(uintptr_t)p->object;
            p->array_texture = texture.textureType == MTLTextureType2DArray;
            if (texture.textureType == MTLTextureType2D || p->array_texture) p->result = XR_SUCCESS;
        }
    }
    return 0;
}

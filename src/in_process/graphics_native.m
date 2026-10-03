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
static wine_XrSession *active_session;
static const int64_t dxgi_formats[] = {28, 29, 87, 91, 10};
static const int64_t metal_formats[] = {70, 71, 80, 81, 112};
static int64_t map_format(int64_t format, int to_metal)
{
    for (unsigned i = 0; i < ARRAY_SIZE(dxgi_formats); ++i)
        if (format == (to_metal ? dxgi_formats[i] : metal_formats[i]))
            return to_metal ? metal_formats[i] : dxgi_formats[i];
    return 0;
}
void mw_native_instance_cleanup(wine_XrInstance *instance)
{
    @autoreleasepool { [(id)instance->required_metal_device release]; }
    instance->required_metal_device = NULL;
}
XrResult wine_xrGetD3D11GraphicsRequirementsKHR(XrInstance instance, XrSystemId system,
                                              XrGraphicsRequirementsD3D11KHR *requirements)
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
        fprintf(stderr, "wineopenxr: selected direct-object zero-copy Metal/DXMT, queue=%p event=%p\n", queue, event);
        return result;
    }
}
XrResult wine_xrDestroySession(XrSession session)
{
    wine_XrSession *wrapper = wine_session_from_handle(session);
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
        int64_t mapped = map_format(native[i], 0);
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
    XrSwapchainCreateInfo native = *info;
    native.format = map_format(info->format, 1);
    if (!native.format) return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    if (info->next || info->faceCount != 1 || info->sampleCount != 1 || info->mipCount != 1 || !info->arraySize ||
        (info->usageFlags & ~(XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                             XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT))) {
        fprintf(stderr, "wineopenxr: unsupported color swapchain next=%p faces=%u samples=%u mips=%u array=%u usage=0x%llx\n",
                info->next, info->faceCount, info->sampleCount, info->mipCount, info->arraySize,
                (unsigned long long)info->usageFlags);
        return XR_ERROR_FEATURE_UNSUPPORTED;
    }
    // OpenComposite requests transfer-destination usage for its OpenVR submit
    // operation. Preserve the flag; importing these runtime-owned images still
    // introduces no copy in the D3D11-to-Metal runtime bridge.
    return g_xr_host_instance_dispatch_table.p_xrCreateSwapchain(wine_session_from_handle(session)->host_session, &native, swapchain);
}
XrResult wine_xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo *info)
{
    wine_XrSwapchain *wrapper = wine_swapchain_from_handle(swapchain);
    wine_XrSession *session = wrapper->session;
    @autoreleasepool {
        id<MTLCommandBuffer> buffer = [(id<MTLCommandQueue>)session->metal_queue commandBuffer];
        [buffer encodeWaitForEvent:(id<MTLSharedEvent>)session->metal_event value:session->fence_value];
        [buffer commit];
        // Monado commits and completes an empty command buffer on this same
        // queue in release, so it waits for the earlier DXMT producer signal.
        return g_xr_host_instance_dispatch_table.p_xrReleaseSwapchainImage(wrapper->host_swapchain, info);
    }
}

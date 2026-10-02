// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Native manual operations. The Khronos loader is linked, as in Proton.
#if 0
#pragma makedep unix
#endif
#include "openxr_private.h"
#include <stdio.h>
#include <unistd.h>
struct openxr_instance_funcs g_xr_host_instance_dispatch_table;
static XrInstance live_instance;
static pthread_mutex_t instance_mutex = PTHREAD_MUTEX_INITIALIZER;

NTSTATUS init_openxr(void *args)
{
    fprintf(stderr, "wineopenxr: in-process native loader, pid=%d, core gate; graphics unavailable\n", getpid());
    return STATUS_SUCCESS;
}

NTSTATUS is_available_instance_function_openxr(void *args)
{
    struct is_available_instance_function_openxr_params *params = args;
    PFN_xrVoidFunction fn;
    params->ret = xrGetInstanceProcAddr(params->instance ? wine_instance_from_handle(params->instance)->host_instance :
                                      XR_NULL_HANDLE, params->name, &fn);
    return STATUS_SUCCESS;
}

XrResult wine_xrCreateInstance(const XrInstanceCreateInfo *info, XrInstance *instance)
{
    pthread_mutex_lock(&instance_mutex);
    if (live_instance) {
        pthread_mutex_unlock(&instance_mutex);
        return XR_ERROR_LIMIT_REACHED;
    }
    if (info->enabledExtensionCount || info->enabledApiLayerCount) {
        pthread_mutex_unlock(&instance_mutex);
        return info->enabledExtensionCount ? XR_ERROR_EXTENSION_NOT_PRESENT : XR_ERROR_API_LAYER_NOT_PRESENT;
    }
    XrResult result = xrCreateInstance(info, instance);
    if (XR_SUCCEEDED(result)) {
        live_instance = *instance;
#define USE_XR_FUNC(name) xrGetInstanceProcAddr(*instance, #name, (PFN_xrVoidFunction *)&g_xr_host_instance_dispatch_table.p_##name);
        ALL_XR_INSTANCE_FUNCS()
#undef USE_XR_FUNC
    }
    pthread_mutex_unlock(&instance_mutex);
    return result;
}

XrResult wine_xrEnumerateInstanceExtensionProperties(const char *layer, uint32_t capacity,
                                                    uint32_t *count, XrExtensionProperties *properties)
{
    if (!count) return XR_ERROR_VALIDATION_FAILURE;
    if (layer) return XR_ERROR_API_LAYER_NOT_PRESENT;
    *count = 0;
    return XR_SUCCESS;
}

XrResult wine_xrCreateSession(XrInstance instance, const XrSessionCreateInfo *info, XrSession *session)
{
    return XR_ERROR_FEATURE_UNSUPPORTED;
}

XrResult wine_xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo *info, XrSwapchain *swapchain)
{
    return XR_ERROR_FEATURE_UNSUPPORTED;
}

XrResult wine_xrDestroyInstance(XrInstance instance)
{
    pthread_mutex_lock(&instance_mutex);
    XrResult result = g_xr_host_instance_dispatch_table.p_xrDestroyInstance(wine_instance_from_handle(instance)->host_instance);
    if (XR_SUCCEEDED(result)) {
        live_instance = XR_NULL_HANDLE;
        memset(&g_xr_host_instance_dispatch_table, 0, sizeof(g_xr_host_instance_dispatch_table));
    }
    pthread_mutex_unlock(&instance_mutex);
    return result;
}

XrResult wine_xrEnumerateApiLayerProperties(uint32_t capacity, uint32_t *count, XrApiLayerProperties *properties)
{
    if (!count) return XR_ERROR_VALIDATION_FAILURE;
    *count = 0;
    return XR_SUCCESS;
}

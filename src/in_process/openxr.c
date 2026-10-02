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
    const char *manifest = getenv("MWXR_NATIVE_RUNTIME_JSON");
    if (manifest && manifest[0] && setenv("XR_RUNTIME_JSON", manifest, 1)) return STATUS_UNSUCCESSFUL;
    fprintf(stderr, "wineopenxr: in-process native loader, pid=%d, D3D11 to Metal, direct objects required\n", getpid());
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

XrResult wine_xrCreateInstance(const XrInstanceCreateInfo *info, XrInstance *instance, void *wrapper_ptr)
{
    pthread_mutex_lock(&instance_mutex);
    if (live_instance) {
        pthread_mutex_unlock(&instance_mutex);
        return XR_ERROR_LIMIT_REACHED;
    }
    wine_XrInstance *wrapper = wrapper_ptr;
    if (info->enabledApiLayerCount) {
        pthread_mutex_unlock(&instance_mutex);
        return XR_ERROR_API_LAYER_NOT_PRESENT;
    }
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        if (strcmp(info->enabledExtensionNames[i], XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) {
            pthread_mutex_unlock(&instance_mutex);
            return XR_ERROR_EXTENSION_NOT_PRESENT;
        }
        wrapper->d3d11_enabled = 1;
    }
    const char *metal = XR_KHR_METAL_ENABLE_EXTENSION_NAME;
    XrInstanceCreateInfo native_info = *info;
    native_info.enabledExtensionCount = wrapper->d3d11_enabled ? 1 : 0;
    native_info.enabledExtensionNames = wrapper->d3d11_enabled ? &metal : NULL;
    XrResult result = xrCreateInstance(&native_info, instance);
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
    uint32_t native_count = 0;
    XrResult result = xrEnumerateInstanceExtensionProperties(NULL, 0, &native_count, NULL);
    if (XR_FAILED(result)) return result;
    XrExtensionProperties *native = calloc(native_count, sizeof(*native));
    if (native_count && !native) return XR_ERROR_OUT_OF_MEMORY;
    for (uint32_t i = 0; i < native_count; ++i) native[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    result = xrEnumerateInstanceExtensionProperties(NULL, native_count, &native_count, native);
    *count = 0;
    if (XR_SUCCEEDED(result)) for (uint32_t i = 0; i < native_count; ++i)
        if (!strcmp(native[i].extensionName, XR_KHR_METAL_ENABLE_EXTENSION_NAME)) *count = 1;
    free(native);
    if (XR_FAILED(result)) return result;
    if (capacity && *count) {
        if (!properties || properties[0].type != XR_TYPE_EXTENSION_PROPERTIES) return XR_ERROR_VALIDATION_FAILURE;
        strcpy(properties[0].extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        properties[0].extensionVersion = XR_KHR_D3D11_enable_SPEC_VERSION;
    }
    return XR_SUCCESS;
}

XrResult wine_xrDestroyInstance(XrInstance instance)
{
    pthread_mutex_lock(&instance_mutex);
    XrResult result = g_xr_host_instance_dispatch_table.p_xrDestroyInstance(wine_instance_from_handle(instance)->host_instance);
    if (XR_SUCCEEDED(result)) {
        extern void mw_native_instance_cleanup(wine_XrInstance *);
        mw_native_instance_cleanup(wine_instance_from_handle(instance));
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

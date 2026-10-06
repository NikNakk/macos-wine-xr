// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Native manual operations. The Khronos loader is linked, as in Proton.
#if 0
#pragma makedep unix
#endif
#include "openxr_private.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
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

static const char *const win32_time_extension = XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME;
static const char *const native_timespec_extension = "XR_KHR_convert_timespec_time";
static const char *const display_distortion_extension = XR_MNDX_DISPLAY_DISTORTION_EXTENSION_NAME;
typedef XrResult (XRAPI_PTR *pfn_timespec_to_time)(XrInstance, const struct timespec *, XrTime *);
typedef XrResult (XRAPI_PTR *pfn_time_to_timespec)(XrInstance, XrTime, struct timespec *);

static int native_extension_present(const char *name)
{
    uint32_t count = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(NULL, 0, &count, NULL)) || !count) return 0;
    XrExtensionProperties *native = calloc(count, sizeof(*native));
    if (!native) return 0;
    for (uint32_t i = 0; i < count; ++i) native[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    int found = 0;
    if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(NULL, count, &count, native)))
        for (uint32_t i = 0; i < count && !found; ++i) found = !strcmp(native[i].extensionName, name);
    free(native);
    return found;
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
        const char *name = info->enabledExtensionNames[i];
        if (!strcmp(name, XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) {
            wrapper->d3d11_enabled = 1;
        } else if (!strcmp(name, win32_time_extension) && native_extension_present(native_timespec_extension)) {
            wrapper->win32_time_enabled = 1;
        } else if (!strcmp(name, display_distortion_extension) &&
                   native_extension_present(display_distortion_extension)) {
            wrapper->display_distortion_enabled = 1;
        } else {
            pthread_mutex_unlock(&instance_mutex);
            return XR_ERROR_EXTENSION_NOT_PRESENT;
        }
    }
    const char *native_names[3];
    uint32_t native_count = 0;
    if (wrapper->d3d11_enabled) native_names[native_count++] = XR_KHR_METAL_ENABLE_EXTENSION_NAME;
    if (wrapper->win32_time_enabled) native_names[native_count++] = native_timespec_extension;
    if (wrapper->display_distortion_enabled) native_names[native_count++] = display_distortion_extension;
    XrInstanceCreateInfo native_info = *info;
    native_info.enabledExtensionCount = native_count;
    native_info.enabledExtensionNames = native_count ? native_names : NULL;
    XrResult result = xrCreateInstance(&native_info, instance);
    if (XR_SUCCEEDED(result)) {
        live_instance = *instance;
#define USE_XR_FUNC(name) xrGetInstanceProcAddr(*instance, #name, (PFN_xrVoidFunction *)&g_xr_host_instance_dispatch_table.p_##name);
        ALL_XR_INSTANCE_FUNCS()
#undef USE_XR_FUNC
        if (wrapper->win32_time_enabled) {
            xrGetInstanceProcAddr(*instance, "xrConvertTimespecTimeToTimeKHR",
                                  (PFN_xrVoidFunction *)&wrapper->native_timespec_to_time);
            xrGetInstanceProcAddr(*instance, "xrConvertTimeToTimespecTimeKHR",
                                  (PFN_xrVoidFunction *)&wrapper->native_time_to_timespec);
        }
    }
    pthread_mutex_unlock(&instance_mutex);
    return result;
}

XrResult wine_xrEnumerateInstanceExtensionProperties(const char *layer, uint32_t capacity,
                                                    uint32_t *count, XrExtensionProperties *properties)
{
    if (!count) return XR_ERROR_VALIDATION_FAILURE;
    if (layer) return XR_ERROR_API_LAYER_NOT_PRESENT;
    const char *names[3];
    uint32_t versions[3], exposed = 0;
    if (native_extension_present(XR_KHR_METAL_ENABLE_EXTENSION_NAME)) {
        names[exposed] = XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
        versions[exposed++] = XR_KHR_D3D11_enable_SPEC_VERSION;
    }
    if (native_extension_present(native_timespec_extension)) {
        names[exposed] = win32_time_extension;
        versions[exposed++] = XR_KHR_win32_convert_performance_counter_time_SPEC_VERSION;
    }
    if (native_extension_present(display_distortion_extension)) {
        names[exposed] = display_distortion_extension;
        versions[exposed++] = XR_MNDX_display_distortion_SPEC_VERSION;
    }
    *count = exposed;
    if (!capacity) return XR_SUCCESS;
    if (capacity < exposed) return XR_ERROR_SIZE_INSUFFICIENT;
    if (!properties) return XR_ERROR_VALIDATION_FAILURE;
    for (uint32_t i = 0; i < exposed; ++i) {
        if (properties[i].type != XR_TYPE_EXTENSION_PROPERTIES) return XR_ERROR_VALIDATION_FAILURE;
        strcpy(properties[i].extensionName, names[i]);
        properties[i].extensionVersion = versions[i];
    }
    return XR_SUCCESS;
}

/* Win32 performance counters, following Proton: sample the Wine counter and
 * CLOCK_MONOTONIC together, then convert through XR_KHR_convert_timespec_time.
 * Wine's counter runs at 10 MHz. */
#define TICKS_PER_SECOND 10000000LL
#define NANOSECONDS_PER_TICK 100LL

static LONGLONG qpc_to_monotonic_offset(void)
{
    LARGE_INTEGER qpc;
    struct timespec ts;
    NtQueryPerformanceCounter(&qpc, NULL);
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return qpc.QuadPart - ((LONGLONG)ts.tv_sec * TICKS_PER_SECOND + ts.tv_nsec / NANOSECONDS_PER_TICK);
}

XrResult wine_xrConvertWin32PerformanceCounterToTimeKHR(XrInstance instance, const LARGE_INTEGER *performanceCounter,
                                                        XrTime *time)
{
    wine_XrInstance *wrapper = wine_instance_from_handle(instance);
    if (!performanceCounter || !time) return XR_ERROR_VALIDATION_FAILURE;
    if (!wrapper->win32_time_enabled || !wrapper->native_timespec_to_time) return XR_ERROR_FUNCTION_UNSUPPORTED;
    LONGLONG monotonic = performanceCounter->QuadPart - qpc_to_monotonic_offset();
    struct timespec ts = {.tv_sec = monotonic / TICKS_PER_SECOND,
                          .tv_nsec = (monotonic % TICKS_PER_SECOND) * NANOSECONDS_PER_TICK};
    return ((pfn_timespec_to_time)wrapper->native_timespec_to_time)(wrapper->host_instance, &ts, time);
}

XrResult wine_xrConvertTimeToWin32PerformanceCounterKHR(XrInstance instance, XrTime time,
                                                        LARGE_INTEGER *performanceCounter)
{
    wine_XrInstance *wrapper = wine_instance_from_handle(instance);
    if (!performanceCounter) return XR_ERROR_VALIDATION_FAILURE;
    if (!wrapper->win32_time_enabled || !wrapper->native_time_to_timespec) return XR_ERROR_FUNCTION_UNSUPPORTED;
    struct timespec ts;
    XrResult result = ((pfn_time_to_timespec)wrapper->native_time_to_timespec)(wrapper->host_instance, time, &ts);
    if (XR_FAILED(result)) return result;
    performanceCounter->QuadPart = (LONGLONG)ts.tv_sec * TICKS_PER_SECOND + ts.tv_nsec / NANOSECONDS_PER_TICK +
                                   qpc_to_monotonic_offset();
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

/* XR_MNDX_display_distortion: the structures are plain data with the same
 * layout on both sides, so the native functions are called directly. */
int32_t mw_display_distortion_call(void *args)
{
    struct mw_display_distortion_params *params = args;
    wine_XrInstance *wrapper = params->instance ? wine_instance_from_handle(params->instance) : NULL;
    params->result = XR_ERROR_FUNCTION_UNSUPPORTED;
    if (!wrapper || !wrapper->display_distortion_enabled) return STATUS_SUCCESS;
    if (params->op == MW_DISPLAY_DISTORTION_PROPERTIES) {
        PFN_xrGetDisplayDistortionPropertiesMNDX get = NULL;
        if (XR_SUCCEEDED(xrGetInstanceProcAddr(wrapper->host_instance, "xrGetDisplayDistortionPropertiesMNDX",
                                               (PFN_xrVoidFunction *)&get)) && get)
            params->result = get(wrapper->host_instance, params->system, params->properties);
    } else if (params->op == MW_DISPLAY_DISTORTION_COMPUTE) {
        PFN_xrComputeDisplayDistortionMNDX compute = NULL;
        if (XR_SUCCEEDED(xrGetInstanceProcAddr(wrapper->host_instance, "xrComputeDisplayDistortionMNDX",
                                               (PFN_xrVoidFunction *)&compute)) && compute)
            params->result = compute(wrapper->host_instance, params->system, params->view_index,
                                     params->point_count, params->points, params->red, params->green, params->blue);
    } else {
        params->result = XR_ERROR_VALIDATION_FAILURE;
    }
    return STATUS_SUCCESS;
}

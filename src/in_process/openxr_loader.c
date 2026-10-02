// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// PE manual entry points, following Proton's loader/unixlib structure.
#include "openxr_loader.h"

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, void *reserved)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(module);
    return !__wine_init_unix_call() && !UNIX_CALL(init, NULL);
}

XrResult WINAPI xrCreateInstance(const XrInstanceCreateInfo *info, XrInstance *instance)
{
    if (!info || !instance) return XR_ERROR_VALIDATION_FAILURE;
    *instance = XR_NULL_HANDLE;
    if (info->type != XR_TYPE_INSTANCE_CREATE_INFO || info->next) return XR_ERROR_VALIDATION_FAILURE;
    if (XR_VERSION_MAJOR(info->applicationInfo.apiVersion) != 1 ||
        XR_VERSION_MINOR(info->applicationInfo.apiVersion) != 0)
        return XR_ERROR_API_VERSION_UNSUPPORTED;
    wine_XrInstance *wrapper = calloc(1, sizeof(*wrapper));
    if (!wrapper) return XR_ERROR_OUT_OF_MEMORY;
    struct xrCreateInstance_params params = {.createInfo = info, .instance = &wrapper->host_instance, .wine_instance = wrapper};
    NTSTATUS status = UNIX_CALL(xrCreateInstance, &params);
    if (status || XR_FAILED(params.result)) {
        free(wrapper);
        return status ? XR_ERROR_INITIALIZATION_FAILED : params.result;
    }
    *instance = (XrInstance)wrapper;
    return params.result;
}

XrResult WINAPI xrDestroyInstance(XrInstance instance)
{
    if (!instance) return XR_ERROR_HANDLE_INVALID;
    extern XrResult mw_pe_cleanup_instance(wine_XrInstance *);
    XrResult cleanup = mw_pe_cleanup_instance(wine_instance_from_handle(instance));
    if (XR_FAILED(cleanup)) return cleanup;
    struct xrDestroyInstance_params params = {.instance = instance};
    if (UNIX_CALL(xrDestroyInstance, &params)) return XR_ERROR_RUNTIME_FAILURE;
    if (XR_SUCCEEDED(params.result)) free((void *)instance);
    return params.result;
}

XrResult WINAPI xrGetInstanceProcAddr(XrInstance instance, const char *name, PFN_xrVoidFunction *fn)
{
    if (!name || !fn) return XR_ERROR_VALIDATION_FAILURE;
    *fn = NULL;
    if (!strcmp(name, "xrGetMetalGraphicsRequirementsKHR")) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (!strcmp(name, "xrGetD3D11GraphicsRequirementsKHR")) {
        if (!instance || !wine_instance_from_handle(instance)->d3d11_enabled) return XR_ERROR_FUNCTION_UNSUPPORTED;
        *fn = (PFN_xrVoidFunction)xrGetD3D11GraphicsRequirementsKHR;
        return XR_SUCCESS;
    }
    struct is_available_instance_function_openxr_params params = {.instance = instance, .name = name};
    if (UNIX_CALL(is_available_instance_function, &params)) return XR_ERROR_RUNTIME_FAILURE;
    if (XR_FAILED(params.ret)) return params.ret;
    *fn = wine_xr_get_instance_proc_addr(name);
    return *fn ? XR_SUCCESS : XR_ERROR_FUNCTION_UNSUPPORTED;
}

XrResult WINAPI xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo *info,
                                                 XrNegotiateRuntimeRequest *request)
{
    if (!info || !request || info->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        info->structVersion != XR_LOADER_INFO_STRUCT_VERSION || info->structSize != sizeof(*info) ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
        request->structVersion != XR_RUNTIME_INFO_STRUCT_VERSION || request->structSize != sizeof(*request) ||
        info->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION ||
        info->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION ||
        info->minApiVersion > XR_MAKE_VERSION(1, 0, 0) || info->maxApiVersion < XR_MAKE_VERSION(1, 0, 0))
        return XR_ERROR_INITIALIZATION_FAILED;
    request->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    request->runtimeApiVersion = XR_MAKE_VERSION(1, 0, 0);
    request->getInstanceProcAddr = xrGetInstanceProcAddr;
    return XR_SUCCESS;
}

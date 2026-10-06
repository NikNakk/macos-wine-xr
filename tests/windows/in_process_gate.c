// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
#include <windows.h>
#include <openxr/openxr.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    HMODULE dll = LoadLibraryA("wineopenxr.dll");
    if (!dll) { fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError()); return 1; }
    PFN_xrGetInstanceProcAddr gipa = (void *)GetProcAddress(dll, "xrGetInstanceProcAddr");
    PFN_xrCreateInstance create;
    PFN_xrGetSystem get_system;
    PFN_xrGetInstanceProperties get_properties;
    PFN_xrDestroyInstance destroy;
    if (!gipa || XR_FAILED(gipa(XR_NULL_HANDLE, "xrCreateInstance", (void *)&create))) return 2;
    PFN_xrEnumerateInstanceExtensionProperties enumerate;
    PFN_xrEnumerateApiLayerProperties enumerate_layers;
    uint32_t count = 99;
    if (XR_FAILED(gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (void *)&enumerate)) ||
        XR_FAILED(enumerate(NULL, 0, &count, NULL)) || count > 3 ||
        XR_FAILED(gipa(XR_NULL_HANDLE, "xrEnumerateApiLayerProperties", (void *)&enumerate_layers)) ||
        XR_FAILED(enumerate_layers(0, &count, NULL)) || count != 0) return 5;
    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ci.applicationInfo.applicationName, "Wine in-process architecture gate");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    XrInstance instance = XR_NULL_HANDLE;
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 1, 0);
    if (create(&ci, &instance) != XR_ERROR_API_VERSION_UNSUPPORTED || instance != XR_NULL_HANDLE) return 11;
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    XrResult result = create(&ci, &instance);
    printf("Windows pid=%lu xrCreateInstance=%d\n", GetCurrentProcessId(), result);
    if (XR_FAILED(result)) return 3;
    XrInstance duplicate = XR_NULL_HANDLE;
    if (create(&ci, &duplicate) != XR_ERROR_LIMIT_REACHED || duplicate != XR_NULL_HANDLE) return 6;
    if (XR_FAILED(gipa(instance, "xrGetSystem", (void *)&get_system)) ||
        XR_FAILED(gipa(instance, "xrGetInstanceProperties", (void *)&get_properties)) ||
        XR_FAILED(gipa(instance, "xrDestroyInstance", (void *)&destroy))) return 4;
    XrSystemGetInfo si = {XR_TYPE_SYSTEM_GET_INFO, NULL, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    XrSystemId system = XR_NULL_SYSTEM_ID;
    result = get_system(instance, &si, &system);
    XrInstanceProperties properties = {XR_TYPE_INSTANCE_PROPERTIES};
    XrResult property_result = get_properties(instance, &properties);
    printf("xrGetSystem=%d system=%llu properties=%d runtime=%s\n", result,
           (unsigned long long)system, property_result, properties.runtimeName);
    XrResult destroy_result = destroy(instance);
    printf("xrDestroyInstance=%d\n", destroy_result);
    if (XR_FAILED(destroy_result)) return 7;
    XrInstance recreated = XR_NULL_HANDLE;
    XrResult recreate_result = create(&ci, &recreated);
    if (XR_FAILED(recreate_result)) return 8;
    XrResult second_destroy = destroy(recreated);
    printf("recreate=%d second_destroy=%d\n", recreate_result, second_destroy);
    if (XR_FAILED(second_destroy)) return 9;
    const char *unsupported = "XR_NOT_SUPPORTED_prototype";
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = &unsupported;
    if (create(&ci, &recreated) != XR_ERROR_EXTENSION_NOT_PRESENT || recreated != XR_NULL_HANDLE) return 10;
    FreeLibrary(dll);
    return XR_FAILED(result) || XR_FAILED(property_result) || XR_FAILED(destroy_result);
}

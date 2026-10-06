// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// XR_MNDX_display_distortion through the in-process runtime.
#include <windows.h>
#include <openxr/openxr.h>
#include <stdio.h>
#include <string.h>
#include "../../src/in_process/mndx_display_distortion.h"

#define CHECK(cond, code)                                                                                              \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "FAIL %d: %s\n", code, #cond);                                                             \
            return code;                                                                                               \
        }                                                                                                              \
    } while (0)

int main(void)
{
    HMODULE dll = LoadLibraryA("wineopenxr.dll");
    CHECK(dll, 1);
    PFN_xrGetInstanceProcAddr gipa = (PFN_xrGetInstanceProcAddr)(void *)GetProcAddress(dll, "xrGetInstanceProcAddr");
    PFN_xrCreateInstance create;
    PFN_xrEnumerateInstanceExtensionProperties enumerate;
    CHECK(gipa && XR_SUCCEEDED(gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&create)), 2);
    CHECK(XR_SUCCEEDED(gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction *)&enumerate)), 3);
    XrExtensionProperties props[4];
    uint32_t count = 0, found = 0;
    for (int i = 0; i < 4; ++i) props[i] = (XrExtensionProperties){XR_TYPE_EXTENSION_PROPERTIES};
    CHECK(XR_SUCCEEDED(enumerate(NULL, 4, &count, props)), 4);
    for (uint32_t i = 0; i < count; ++i) found |= !strcmp(props[i].extensionName, XR_MNDX_DISPLAY_DISTORTION_EXTENSION_NAME);
    CHECK(found, 5);

    const char *ext = XR_MNDX_DISPLAY_DISTORTION_EXTENSION_NAME;
    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ci.applicationInfo.applicationName, "in-process distortion probe");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = &ext;
    XrInstance instance = XR_NULL_HANDLE;
    CHECK(XR_SUCCEEDED(create(&ci, &instance)), 6);
    PFN_xrGetSystem get_system;
    PFN_xrGetDisplayDistortionPropertiesMNDX get_props;
    PFN_xrComputeDisplayDistortionMNDX compute;
    PFN_xrDestroyInstance destroy;
    CHECK(XR_SUCCEEDED(gipa(instance, "xrGetSystem", (PFN_xrVoidFunction *)&get_system)), 7);
    CHECK(XR_SUCCEEDED(gipa(instance, "xrGetDisplayDistortionPropertiesMNDX", (PFN_xrVoidFunction *)&get_props)), 8);
    CHECK(XR_SUCCEEDED(gipa(instance, "xrComputeDisplayDistortionMNDX", (PFN_xrVoidFunction *)&compute)), 9);
    CHECK(XR_SUCCEEDED(gipa(instance, "xrDestroyInstance", (PFN_xrVoidFunction *)&destroy)), 10);
    XrSystemGetInfo si = {XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    CHECK(XR_SUCCEEDED(get_system(instance, &si, &system)), 11);

    XrDisplayDistortionPropertiesMNDX dp = {XR_TYPE_DISPLAY_DISTORTION_PROPERTIES_MNDX};
    XrResult r = get_props(instance, system, &dp);
    printf("properties: %d display %dx%d %.2f Hz, %u views\n", r, dp.displaySize.width, dp.displaySize.height,
           dp.nominalRefreshRate, dp.viewCount);
    CHECK(XR_SUCCEEDED(r) && dp.viewCount >= 1 && dp.displaySize.width > 0, 12);
    for (uint32_t v = 0; v < dp.viewCount; ++v) {
        const XrDisplayDistortionViewMNDX *view = &dp.views[v];
        printf("view %u: viewport %d,%d %dx%d fov L%.1f R%.1f U%.1f D%.1f\n", v, view->viewport.offset.x,
               view->viewport.offset.y, view->viewport.extent.width, view->viewport.extent.height,
               view->fov.angleLeft * 57.29578f, view->fov.angleRight * 57.29578f, view->fov.angleUp * 57.29578f,
               view->fov.angleDown * 57.29578f);
        XrVector2f points[5] = {{0.5f, 0.5f}, {0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};
        XrVector2f red[5], green[5], blue[5];
        r = compute(instance, system, v, 5, points, red, green, blue);
        CHECK(XR_SUCCEEDED(r), 13);
        for (int i = 0; i < 5; ++i)
            printf("  (%.2f,%.2f) -> r %.4f,%.4f g %.4f,%.4f b %.4f,%.4f\n", points[i].x, points[i].y, red[i].x,
                   red[i].y, green[i].x, green[i].y, blue[i].x, blue[i].y);
    }
    XrVector2f one = {0.5f, 0.5f}, out[3];
    CHECK(compute(instance, system, 7, 1, &one, &out[0], &out[1], &out[2]) == XR_ERROR_VALIDATION_FAILURE, 14);
    CHECK(XR_SUCCEEDED(destroy(instance)), 15);
    printf("PASS\n");
    return 0;
}

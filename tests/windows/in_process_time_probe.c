// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// XR_KHR_win32_convert_performance_counter_time through the in-process runtime.
#include <windows.h>
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <stdio.h>
#include <string.h>

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
    CHECK(gipa, 2);
    PFN_xrEnumerateInstanceExtensionProperties enumerate;
    PFN_xrCreateInstance create;
    CHECK(XR_SUCCEEDED(gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction *)&enumerate)), 3);
    CHECK(XR_SUCCEEDED(gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&create)), 4);

    XrExtensionProperties props[4];
    uint32_t count = 0, found = 0;
    for (int i = 0; i < 4; ++i) props[i] = (XrExtensionProperties){XR_TYPE_EXTENSION_PROPERTIES};
    CHECK(XR_SUCCEEDED(enumerate(NULL, 4, &count, props)), 5);
    for (uint32_t i = 0; i < count; ++i) {
        printf("extension %s v%u\n", props[i].extensionName, props[i].extensionVersion);
        found |= !strcmp(props[i].extensionName, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
    }
    CHECK(found, 6);

    const char *ext = XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME;
    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ci.applicationInfo.applicationName, "in-process time probe");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = &ext;
    XrInstance instance = XR_NULL_HANDLE;
    CHECK(XR_SUCCEEDED(create(&ci, &instance)), 7);

    PFN_xrConvertWin32PerformanceCounterToTimeKHR to_time;
    PFN_xrConvertTimeToWin32PerformanceCounterKHR to_qpc;
    PFN_xrDestroyInstance destroy;
    CHECK(XR_SUCCEEDED(gipa(instance, "xrConvertWin32PerformanceCounterToTimeKHR", (PFN_xrVoidFunction *)&to_time)), 8);
    CHECK(XR_SUCCEEDED(gipa(instance, "xrConvertTimeToWin32PerformanceCounterKHR", (PFN_xrVoidFunction *)&to_qpc)), 9);
    CHECK(XR_SUCCEEDED(gipa(instance, "xrDestroyInstance", (PFN_xrVoidFunction *)&destroy)), 10);

    LARGE_INTEGER freq, a, b, back;
    XrTime ta, tb;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&a);
    CHECK(XR_SUCCEEDED(to_time(instance, &a, &ta)), 11);
    Sleep(100);
    QueryPerformanceCounter(&b);
    CHECK(XR_SUCCEEDED(to_time(instance, &b, &tb)), 12);
    CHECK(XR_SUCCEEDED(to_qpc(instance, ta, &back)), 13);

    double qpc_ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)freq.QuadPart;
    double xr_ms = (double)(tb - ta) / 1e6;
    long long roundtrip_ticks = back.QuadPart - a.QuadPart;
    printf("freq=%lld qpc_delta=%.3fms xr_delta=%.3fms roundtrip_ticks=%lld xr_time=%lld\n", (long long)freq.QuadPart,
           qpc_ms, xr_ms, roundtrip_ticks, (long long)ta);
    CHECK(ta > 0 && tb > ta, 14);
    CHECK(xr_ms - qpc_ms < 1.0 && qpc_ms - xr_ms < 1.0, 15);
    CHECK(roundtrip_ticks > -100 && roundtrip_ticks < 100, 16); // within 10 us
    CHECK(XR_SUCCEEDED(destroy(instance)), 17);
    printf("PASS\n");
    return 0;
}

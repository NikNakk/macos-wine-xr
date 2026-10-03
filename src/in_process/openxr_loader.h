// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#ifndef __cplusplus
#include "wine/debug.h"
#include "wine/list.h"
#endif
#include "winternl.h"
#ifdef __cplusplus
extern "C" {
#endif
#include "wine/unixlib.h"
#ifdef __cplusplus
}
#endif
#include <d3d11.h>
#ifdef __cplusplus
extern "C" {
#endif
#include "wineopenxr.h"
#include "loader_thunks.h"
#ifdef __cplusplus
}
#endif
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
struct openxr_func { const char *name; void *func; };
#ifdef __cplusplus
extern "C" {
#endif
extern void *wine_xr_get_instance_proc_addr(const char *name);
#ifdef __cplusplus
}
#endif
struct is_available_instance_function_openxr_params {
    XrInstance instance;
    const char *name;
    XrResult ret;
};

typedef struct { XrInstance host_instance; void *required_metal_device; uint32_t d3d11_enabled; } wine_XrInstance;
typedef struct { XrSession host_session; wine_XrInstance *instance; void *metal_device;
    void *metal_queue; void *metal_event; uint64_t fence_value; void *graphics;
} wine_XrSession;
// copy_source/copy_target are set only for the duration of a release that
// uses an explicit graphics copy fallback (native MTLTexture objects).
typedef struct { XrSwapchain host_swapchain; wine_XrSession *session; XrSwapchainCreateInfo info;
    void *graphics; void *copy_source, *copy_target;
} wine_XrSwapchain;
static inline wine_XrInstance *wine_instance_from_handle(XrInstance h) { return (wine_XrInstance *)h; }
static inline wine_XrSession *wine_session_from_handle(XrSession h) { return (wine_XrSession *)h; }
static inline wine_XrSwapchain *wine_swapchain_from_handle(XrSwapchain h) { return (wine_XrSwapchain *)h; }
#define UNIX_CALL(name, params) WINE_UNIX_CALL(unix_##name, params)

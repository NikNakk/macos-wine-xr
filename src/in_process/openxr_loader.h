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
#include "wine/debug.h"
#include "wine/list.h"
#include "wine/unixlib.h"
#include "wineopenxr.h"
#include "loader_thunks.h"
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
struct openxr_func { const char *name; void *func; };
extern void *wine_xr_get_instance_proc_addr(const char *name);
struct is_available_instance_function_openxr_params {
    XrInstance instance;
    const char *name;
    XrResult ret;
};

typedef struct { XrInstance host_instance; } wine_XrInstance;
typedef struct { XrSession host_session; } wine_XrSession;
typedef struct { XrSwapchain host_swapchain; } wine_XrSwapchain;
static inline wine_XrInstance *wine_instance_from_handle(XrInstance h) { return (void *)h; }
static inline wine_XrSession *wine_session_from_handle(XrSession h) { return (void *)h; }
static inline wine_XrSwapchain *wine_swapchain_from_handle(XrSwapchain h) { return (void *)h; }
#define UNIX_CALL(name, params) WINE_UNIX_CALL(unix_##name, params)

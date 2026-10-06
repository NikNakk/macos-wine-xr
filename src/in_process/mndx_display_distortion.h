// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// XR_MNDX_display_distortion (Monado's experimental extension, BSL-1.0
// header in src/external/openxr_includes/openxr/XR_MNDX_display_distortion.h),
// mirrored here because it is not in the Khronos registry the thunks are
// generated from, plus the fixed ABI of its one unix call. Pointers are
// borrowed and valid only in this process.
#pragma once
#include <stdint.h>

#ifndef XR_MNDX_display_distortion
#define XR_MNDX_display_distortion 1
#define XR_MNDX_display_distortion_SPEC_VERSION 1
#define XR_MNDX_DISPLAY_DISTORTION_EXTENSION_NAME "XR_MNDX_display_distortion"
#define XR_MNDX_DISPLAY_DISTORTION_MAX_VIEWS 2
#define XR_TYPE_DISPLAY_DISTORTION_PROPERTIES_MNDX ((XrStructureType)0x7fff5060)

typedef struct XrDisplayDistortionViewMNDX {
    XrRect2Di viewport;
    XrFovf fov;
} XrDisplayDistortionViewMNDX;

typedef struct XrDisplayDistortionPropertiesMNDX {
    XrStructureType type;
    void *next;
    XrExtent2Di displaySize;
    float nominalRefreshRate;
    uint32_t viewCount;
    XrDisplayDistortionViewMNDX views[XR_MNDX_DISPLAY_DISTORTION_MAX_VIEWS];
} XrDisplayDistortionPropertiesMNDX;

typedef XrResult(XRAPI_PTR *PFN_xrGetDisplayDistortionPropertiesMNDX)(XrInstance, XrSystemId,
                                                                      XrDisplayDistortionPropertiesMNDX *);
typedef XrResult(XRAPI_PTR *PFN_xrComputeDisplayDistortionMNDX)(XrInstance, XrSystemId, uint32_t, uint32_t,
                                                               const XrVector2f *, XrVector2f *, XrVector2f *,
                                                               XrVector2f *);
#endif

enum mw_display_distortion_op {
    MW_DISPLAY_DISTORTION_PROPERTIES = 1,
    MW_DISPLAY_DISTORTION_COMPUTE,
};

struct mw_display_distortion_params {
    XrInstance instance; // the Windows-side wrapper handle
    XrSystemId system;
    uint32_t op;
    uint32_t view_index;
    uint32_t point_count;
    uint32_t padding;
    XrDisplayDistortionPropertiesMNDX *properties;
    const XrVector2f *points;
    XrVector2f *red, *green, *blue;
    XrResult result;
};

#ifdef WINE_UNIX_LIB
int32_t mw_display_distortion_call(void *args);
#endif

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Experimental opt-in D3D11 companion to Monado's Metal foveation transport.
#pragma once
#include <stdint.h>
#define XR_MWXR_FOVEATION_D3D11_EXTENSION_NAME "XR_MWXR_foveation_d3d11"
#define XR_MWXR_foveation_d3d11_SPEC_VERSION 1
#define XR_TYPE_FOVEATION_D3D11_STATE_MWXR ((XrStructureType)0x7fff5070)
typedef struct XrFoveationD3D11StateMWXR {
    XrStructureType type;
    void *next;
    XrBool32 foveationEnabled;
    uint32_t physicalWidth, physicalHeight, revision;
} XrFoveationD3D11StateMWXR;
// Call after waiting the current image, before recording draws. Initially:
// one outstanding image per non-array swapchain, one colour RT, no depth/MSAA.
// Acquiring both eye swapchains before querying either is supported.
typedef XrResult (XRAPI_PTR *PFN_xrApplyFoveationD3D11MWXR)(
    XrSwapchain, uint32_t viewIndex, XrFoveationD3D11StateMWXR *);

// Native Monado ABI, from XR_MNDX_foveation_metal.h (BSL-1.0, Nick Kennedy).
#define XR_MNDX_FOVEATION_METAL_EXTENSION_NAME "XR_MNDX_foveation_metal"
#define XR_TYPE_FOVEATION_METAL_STATE_MNDX ((XrStructureType)0x7fff5057)
typedef struct XrFoveationMetalStateMNDX {
    XrStructureType type;
    void *next;
    XrBool32 foveationEnabled;
    void *rasterizationRateMap;
    uint32_t physicalWidth, physicalHeight, revision;
} XrFoveationMetalStateMNDX;
typedef XrResult (XRAPI_PTR *PFN_xrGetFoveationMetalStateMNDX)(XrSwapchain, uint32_t, uint32_t,
                                                            XrFoveationMetalStateMNDX *);
// MWXR_GAZE_FOVEATION: once per frame for the projection swapchain. Returns a borrowed
// MTLRasterizationRateMap over the whole packed target (0: render ordinary).
struct mw_gaze_foveation_params {
    XrSwapchain swapchain;
    uint32_t width, height;
    XrRect2Di rects[2];
    XrFovf fovs[2];
    uint64_t map;
    XrResult result;
};
struct mw_foveation_params {
    XrSwapchain swapchain;
    uint32_t view_index, reserved;
    XrFoveationD3D11StateMWXR *state;
    uint64_t map;
    XrResult result;
};

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "openxr_loader.h"

enum mw_vulkan_op { MW_VK_REQUIREMENTS, MW_VK_MATCH_DEVICE, MW_VK_TEXTURE_TYPE };
struct mw_vulkan_native_params {
    wine_XrInstance *instance;
    XrSystemId system;
    uint64_t object;
    uint32_t op, array_texture;
    XrResult result;
};
#ifdef __cplusplus
XrResult mw_vk_create_session(XrInstance, const XrSessionCreateInfo *, XrSession *);
XrResult mw_vk_destroy_session(XrSession);
XrResult mw_vk_create_swapchain(XrSession, const XrSwapchainCreateInfo *, XrSwapchain *);
XrResult mw_vk_destroy_swapchain(XrSwapchain);
XrResult mw_vk_enumerate_images(XrSwapchain, uint32_t, uint32_t *, XrSwapchainImageBaseHeader *);
XrResult mw_vk_acquire(XrSwapchain, const XrSwapchainImageAcquireInfo *, uint32_t *);
XrResult mw_vk_wait(XrSwapchain, const XrSwapchainImageWaitInfo *);
XrResult mw_vk_release(XrSwapchain, const XrSwapchainImageReleaseInfo *);
// The submitted stereo projection layer, for gaze foveation (MWXR_FOVEATION with MWXR_KK_FOVEATION).
void mw_vk_note_projection(XrSession, const XrCompositionLayerProjection *);
#endif

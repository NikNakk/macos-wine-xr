// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <openxr/openxr.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mwxr_native_backend;
struct mwxr_native_swapchain;

enum mwxr_image_strategy {
	MWXR_IMAGE_SHARED_METAL = 1,
	MWXR_IMAGE_GPU_BLIT = 2,
};

struct mwxr_backend_info {
	XrInstanceProperties runtime;
	XrSystemProperties system;
	uint32_t view_count;
	uint32_t recommended_width;
	uint32_t recommended_height;
};

/* Plain metadata only: never pass Objective-C or OpenXR pointers to Wine. */
struct mwxr_image_info {
	uint64_t image_id;
	int64_t metal_format;
	uint32_t width, height, array_size, mip_count, sample_count;
	uint32_t iosurface_backed, shared_handle;
	enum mwxr_image_strategy strategy;
};

struct mwxr_blit_timing {
	uint64_t fence_wait_ns;
	uint64_t copy_submit_ns;
	uint64_t copy_complete_ns;
	uint64_t release_ns;
	double gpu_start_seconds;
	double gpu_end_seconds;
};

/* loader_path=NULL uses MWXR_OPENXR_LOADER or libopenxr_loader.dylib.
 * The Khronos loader owns runtime selection (including XR_RUNTIME_JSON).
 * All calls on an object must be serialized by the native host. */
XrResult mwxr_native_backend_open(const char *loader_path, struct mwxr_native_backend **out_backend);
void mwxr_native_backend_close(struct mwxr_native_backend *backend);
const struct mwxr_backend_info *mwxr_native_backend_info(const struct mwxr_native_backend *backend);
/* Borrowed native-process objects, valid until owner destruction. */
void *mwxr_native_backend_metal_device(struct mwxr_native_backend *backend);
void *mwxr_native_swapchain_metal_texture(struct mwxr_native_swapchain *swapchain, uint32_t index);
XrResult mwxr_native_backend_formats(struct mwxr_native_backend *backend, uint32_t capacity,
                                    uint32_t *count, int64_t *formats);
XrResult mwxr_native_swapchain_create(struct mwxr_native_backend *backend, const XrSwapchainCreateInfo *info,
                                    struct mwxr_native_swapchain **out_swapchain);
void mwxr_native_swapchain_destroy(struct mwxr_native_swapchain *swapchain);
uint32_t mwxr_native_swapchain_image_count(const struct mwxr_native_swapchain *swapchain);
const struct mwxr_image_info *mwxr_native_swapchain_image(const struct mwxr_native_swapchain *swapchain,
                                                        uint32_t index);
XrResult mwxr_native_swapchain_acquire(struct mwxr_native_swapchain *swapchain, uint32_t *index);
XrResult mwxr_native_swapchain_wait(struct mwxr_native_swapchain *swapchain, XrDuration timeout);
XrResult mwxr_native_swapchain_release(struct mwxr_native_swapchain *swapchain);

/* Native-process-only access. The caller owns the returned send right and must
 * mach_port_deallocate it. The texture/swapchain must stay alive until import
 * completes. No process-global bootstrap registrations are created here. */
XrResult mwxr_native_swapchain_export(struct mwxr_native_swapchain *swapchain, uint32_t index,
                                    uint32_t *out_mach_port);
/* Native-only objects, never wire pointers. Source and event are borrowed
 * MTLTexture/MTLSharedEvent objects on the runtime device. Completion precedes
 * OpenXR release. A missing producer fence is explicitly unsupported.
 * Copy all array slices in one command buffer, with no CPU image access. */
XrResult mwxr_native_swapchain_blit_release(struct mwxr_native_swapchain *swapchain, uint32_t index,
                                          void *source_texture, void *producer_event, uint64_t value,
                                          XrDuration timeout, struct mwxr_blit_timing *timing);
XrResult mwxr_native_swapchain_blit(struct mwxr_native_swapchain *swapchain, uint32_t index,
                                  void *source_texture, void *producer_event, uint64_t value,
                                  XrDuration timeout, struct mwxr_blit_timing *timing);

#ifdef __cplusplus
}
#endif

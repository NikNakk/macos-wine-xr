// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "macos_wine_xr/native_openxr_backend.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	if (argc > 2) { fprintf(stderr, "usage: %s [loader.dylib]\n", argv[0]); return 2; }
	struct mwxr_native_backend *backend = NULL;
	struct mwxr_native_swapchain *swapchain = NULL;
	int exit_code = 1;
	XrResult result = mwxr_native_backend_open(argc == 2 ? argv[1] : NULL, &backend);
	if (XR_FAILED(result)) return 1;
	const struct mwxr_backend_info *info = mwxr_native_backend_info(backend);
	printf("runtime=%s\nruntime_version=%u.%u.%u\nsystem=%s\nview_count=%u\nrecommended=%ux%u\n",
	       info->runtime.runtimeName, (unsigned)XR_VERSION_MAJOR(info->runtime.runtimeVersion),
	       (unsigned)XR_VERSION_MINOR(info->runtime.runtimeVersion), (unsigned)XR_VERSION_PATCH(info->runtime.runtimeVersion),
	       info->system.systemName, info->view_count, info->recommended_width, info->recommended_height);
	uint32_t count = 0;
	result = mwxr_native_backend_formats(backend, 0, &count, NULL);
	if (XR_FAILED(result) || !count) goto done;
	int64_t *formats = calloc(count, sizeof(*formats));
	if (!formats) goto done;
	result = mwxr_native_backend_formats(backend, count, &count, formats);
	int64_t selected = 0;
	if (XR_SUCCEEDED(result)) {
		for (uint32_t i = 0; i < count; ++i) {
			printf("metal_format[%u]=%lld\n", i, (long long)formats[i]);
			/* BGRA8/RGBA8, linear or sRGB: single-sample color only. */
			if (!selected && (formats[i] == 80 || formats[i] == 81 || formats[i] == 70 || formats[i] == 71))
				selected = formats[i];
		}
	}
	free(formats);
	if (!selected) { fprintf(stderr, "No supported 8-bit color Metal format\n"); goto done; }
	/* Probe runtime-owned stereo array images, not just single-slice textures. */
	XrSwapchainCreateInfo ci = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
	    .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
	    .format = selected, .sampleCount = 1, .width = info->recommended_width,
	    .height = info->recommended_height, .faceCount = 1, .arraySize = info->view_count, .mipCount = 1};
	result = mwxr_native_swapchain_create(backend, &ci, &swapchain);
	if (XR_FAILED(result)) goto done;
	count = mwxr_native_swapchain_image_count(swapchain);
	printf("selected_metal_format=%lld\nswapchain_image_count=%u\n", (long long)selected, count);
	for (uint32_t i = 0; i < count; ++i) {
		const struct mwxr_image_info *image = mwxr_native_swapchain_image(swapchain, i);
		printf("image=%u id=%llu size=%ux%u array=%u iosurface=%u shared_handle=%u\n", i,
		       (unsigned long long)image->image_id, image->width, image->height,
		       image->array_size, image->iosurface_backed, image->shared_handle);
	}
	const struct mwxr_image_info *first = mwxr_native_swapchain_image(swapchain, 0);
	printf("swapchain_strategy=%s\n", first->strategy == MWXR_IMAGE_SHARED_METAL ? "shared-metal-zero-copy" : "gpu-blit");
	printf("copies_per_submitted_image=%u\n", first->strategy == MWXR_IMAGE_SHARED_METAL ? 0 : 1);
	exit_code = 0;
done:
	mwxr_native_swapchain_destroy(swapchain);
	mwxr_native_backend_close(backend);
	return exit_code;
}

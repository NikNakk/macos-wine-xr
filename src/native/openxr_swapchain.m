// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "native_openxr_internal.h"
#include <mach/mach.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Same native capability used by current DXMT. Keep the SPI at this boundary. */
@interface MTLSharedTextureHandle (MWXRExport)
- (mach_port_t)createMachPort;
@end

XrResult
mwxr_native_swapchain_create(struct mwxr_native_backend *b, const XrSwapchainCreateInfo *info,
                             struct mwxr_native_swapchain **out_swapchain)
{
	if (!b || !info || !out_swapchain || info->next || info->type != XR_TYPE_SWAPCHAIN_CREATE_INFO)
		return XR_ERROR_VALIDATION_FAILURE;
	*out_swapchain = NULL;
	if (info->faceCount != 1 || info->sampleCount != 1 || info->mipCount != 1 ||
	    (info->createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT)) return XR_ERROR_FEATURE_UNSUPPORTED;
	struct mwxr_native_swapchain *s = calloc(1, sizeof(*s));
	if (!s) return XR_ERROR_OUT_OF_MEMORY;
	s->backend = b;
	/* Link immediately: teardown handles every partial-initialization failure. */
	s->next = b->swapchains;
	b->swapchains = s;
	XrResult result;
	CHECK(b->CreateSwapchain(b->session, info, &s->handle));
	CHECK(b->EnumerateSwapchainImages(s->handle, 0, &s->count, NULL));
	if (!s->count) { result = XR_ERROR_RUNTIME_FAILURE; goto fail; }
	s->images = calloc(s->count, sizeof(*s->images));
	s->metadata = calloc(s->count, sizeof(*s->metadata));
	s->shared = calloc(s->count, sizeof(*s->shared));
	s->acquired = calloc(s->count, sizeof(*s->acquired));
	s->waited = calloc(s->count, sizeof(*s->waited));
	if (!s->images || !s->metadata || !s->shared || !s->acquired || !s->waited) {
		result = XR_ERROR_OUT_OF_MEMORY; goto fail;
	}
	for (uint32_t i = 0; i < s->count; ++i) s->images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR;
	uint32_t returned = s->count;
	CHECK(b->EnumerateSwapchainImages(s->handle, s->count, &returned, (XrSwapchainImageBaseHeader *)s->images));
	if (!returned || returned > s->count) { result = XR_ERROR_RUNTIME_FAILURE; goto fail; }
	s->count = returned;
	bool all_shared = true;
	for (uint32_t i = 0; i < s->count; ++i) {
		id<MTLTexture> texture = (id<MTLTexture>)s->images[i].texture;
		if (!texture || texture.device.registryID != b->device.registryID) {
			result = XR_ERROR_GRAPHICS_DEVICE_INVALID; goto fail;
		}
		if (texture.width != info->width || texture.height != info->height ||
		    texture.arrayLength != info->arraySize || texture.pixelFormat != (MTLPixelFormat)info->format ||
		    texture.sampleCount != info->sampleCount || texture.mipmapLevelCount != info->mipCount) {
			result = diagnose(XR_ERROR_RUNTIME_FAILURE, "runtime image descriptor mismatch"); goto fail;
		}
		s->shared[i] = [texture newSharedTextureHandle];
		/* A handle alone is insufficient: also prove it reopens on this device. */
		if (s->shared[i]) {
			id<MTLTexture> reopened = [b->device newSharedTextureWithHandle:s->shared[i]];
			if (!reopened || reopened.width != texture.width || reopened.height != texture.height ||
			    reopened.arrayLength != texture.arrayLength || reopened.pixelFormat != texture.pixelFormat ||
			    reopened.textureType != texture.textureType || reopened.sampleCount != texture.sampleCount ||
			    reopened.mipmapLevelCount != texture.mipmapLevelCount) {
				[s->shared[i] release]; s->shared[i] = nil;
			}
			[reopened release];
		}
		all_shared &= s->shared[i] != nil;
		s->metadata[i] = (struct mwxr_image_info){
		    .image_id = b->next_image_id++, .metal_format = (int64_t)texture.pixelFormat,
		    .width = (uint32_t)texture.width, .height = (uint32_t)texture.height,
		    .array_size = (uint32_t)texture.arrayLength, .mip_count = (uint32_t)texture.mipmapLevelCount,
		    .sample_count = (uint32_t)texture.sampleCount, .iosurface_backed = texture.iosurface != NULL,
		    .shared_handle = s->shared[i] != nil,
		};
	}
	for (uint32_t i = 0; i < s->count; ++i)
		s->metadata[i].strategy = all_shared ? MWXR_IMAGE_SHARED_METAL : MWXR_IMAGE_GPU_BLIT;
	*out_swapchain = s;
	return XR_SUCCESS;
fail:
	mwxr_native_swapchain_destroy(s);
	return result;
}

void
mwxr_native_swapchain_destroy(struct mwxr_native_swapchain *s)
{
	if (!s) return;
	struct mwxr_native_backend *b = s->backend;
	struct mwxr_native_swapchain **link = &b->swapchains;
	while (*link && *link != s) link = &(*link)->next;
	if (*link) *link = s->next;
	for (uint32_t i = 0; s->shared && i < s->count; ++i) [s->shared[i] release];
	if (s->handle) diagnose(b->DestroySwapchain(s->handle), "xrDestroySwapchain");
	free(s->shared); free(s->metadata); free(s->images); free(s->acquired); free(s->waited); free(s);
}

uint32_t mwxr_native_swapchain_image_count(const struct mwxr_native_swapchain *s) { return s ? s->count : 0; }
const struct mwxr_image_info *
mwxr_native_swapchain_image(const struct mwxr_native_swapchain *s, uint32_t i)
{
	return s && i < s->count ? &s->metadata[i] : NULL;
}

XrResult mwxr_native_swapchain_acquire(struct mwxr_native_swapchain *s, uint32_t *index)
{
	if (!s || !index) return XR_ERROR_VALIDATION_FAILURE;
	if (s->acquired_count == s->count) return XR_ERROR_CALL_ORDER_INVALID;
	XrSwapchainImageAcquireInfo info = {.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	XrResult result = diagnose(s->backend->AcquireSwapchainImage(s->handle, &info, index), "xrAcquireSwapchainImage");
	if (XR_SUCCEEDED(result)) {
		if (*index >= s->count) return XR_ERROR_RUNTIME_FAILURE;
		s->acquired[s->acquired_count] = *index;
		s->waited[s->acquired_count++] = false;
	}
	return result;
}
XrResult mwxr_native_swapchain_wait(struct mwxr_native_swapchain *s, XrDuration timeout)
{
	if (!s) return XR_ERROR_VALIDATION_FAILURE;
	if (!s->acquired_count || s->waited[0]) return XR_ERROR_CALL_ORDER_INVALID;
	XrSwapchainImageWaitInfo info = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = timeout};
	XrResult result = diagnose(s->backend->WaitSwapchainImage(s->handle, &info), "xrWaitSwapchainImage");
	if (result == XR_SUCCESS || result == XR_SESSION_LOSS_PENDING) s->waited[0] = true;
	return result;
}
XrResult mwxr_native_swapchain_release(struct mwxr_native_swapchain *s)
{
	if (!s) return XR_ERROR_VALIDATION_FAILURE;
	if (!s->acquired_count || !s->waited[0]) return XR_ERROR_CALL_ORDER_INVALID;
	XrSwapchainImageReleaseInfo info = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	XrResult result = diagnose(s->backend->ReleaseSwapchainImage(s->handle, &info), "xrReleaseSwapchainImage");
	if (XR_SUCCEEDED(result)) {
		--s->acquired_count;
		memmove(s->acquired, s->acquired + 1, s->acquired_count * sizeof(*s->acquired));
		memmove(s->waited, s->waited + 1, s->acquired_count * sizeof(*s->waited));
	}
	return result;
}
XrResult mwxr_native_swapchain_export(struct mwxr_native_swapchain *s, uint32_t i, uint32_t *out_port)
{
	if (!out_port) return XR_ERROR_VALIDATION_FAILURE;
	*out_port = MACH_PORT_NULL;
	if (!s || i >= s->count) return XR_ERROR_VALIDATION_FAILURE;
	if (!s->shared[i] || ![s->shared[i] respondsToSelector:@selector(createMachPort)])
		return XR_ERROR_FEATURE_UNSUPPORTED;
	*out_port = [s->shared[i] createMachPort];
	return *out_port ? XR_SUCCESS : XR_ERROR_RUNTIME_FAILURE;
}

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

XrResult
mwxr_native_swapchain_blit(struct mwxr_native_swapchain *s, uint32_t index,
                                   void *source_texture, void *producer_event, uint64_t value,
                                   XrDuration timeout, struct mwxr_blit_timing *timing)
{
	if (!s || index >= s->count || !source_texture || !timing || timeout < 0)
		return XR_ERROR_VALIDATION_FAILURE;
	memset(timing, 0, sizeof(*timing));
	if (!s->acquired_count || s->acquired[0] != index || !s->waited[0]) return XR_ERROR_CALL_ORDER_INVALID;
	if (!producer_event) return XR_ERROR_FEATURE_UNSUPPORTED;
	id<MTLTexture> src = (id<MTLTexture>)source_texture;
	id<MTLTexture> dst = (id<MTLTexture>)s->images[index].texture;
	id<MTLSharedEvent> event = (id<MTLSharedEvent>)producer_event;
	/* MTLSharedEvent spans devices: its device property may legitimately be nil. */
	if (src.device != s->backend->device ||
	    src.width != dst.width || src.height != dst.height || src.arrayLength != dst.arrayLength ||
	    src.pixelFormat != dst.pixelFormat || src.mipmapLevelCount != 1 || dst.mipmapLevelCount != 1 ||
	    src.sampleCount != 1 || dst.sampleCount != 1 || src.framebufferOnly || dst.framebufferOnly ||
	    (src.textureType != MTLTextureType2D && src.textureType != MTLTextureType2DArray) ||
	    (dst.textureType != MTLTextureType2D && dst.textureType != MTLTextureType2DArray) || src == dst) {
		fprintf(stderr,"native-openxr: unsupported blit descriptors: src=%lux%lu array=%lu type=%lu format=%lu "
		        "dst=%lux%lu array=%lu type=%lu format=%lu\n",(unsigned long)src.width,(unsigned long)src.height,
		        (unsigned long)src.arrayLength,(unsigned long)src.textureType,(unsigned long)src.pixelFormat,
		        (unsigned long)dst.width,(unsigned long)dst.height,(unsigned long)dst.arrayLength,
		        (unsigned long)dst.textureType,(unsigned long)dst.pixelFormat);
		return XR_ERROR_FEATURE_UNSUPPORTED;
	}
	@autoreleasepool {
		uint64_t start = now_ns();
		if (event.signaledValue < value) {
			uint64_t ms = ((uint64_t)timeout + 999999ull) / 1000000ull;
			if (![event waitUntilSignaledValue:value timeoutMS:ms]) return XR_TIMEOUT_EXPIRED;
		}
		timing->fence_wait_ns = now_ns() - start;
		id<MTLCommandBuffer> command = [s->backend->queue commandBuffer];
		id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
		if (!command || !blit) return XR_ERROR_RUNTIME_FAILURE;
		MTLSize size = MTLSizeMake(dst.width, dst.height, 1);
		for (NSUInteger slice = 0; slice < dst.arrayLength; ++slice)
			[blit copyFromTexture:src sourceSlice:slice sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
			           sourceSize:size toTexture:dst destinationSlice:slice destinationLevel:0
			    destinationOrigin:MTLOriginMake(0, 0, 0)];
		[blit endEncoding];
		timing->copy_submit_ns = now_ns();
		[command commit];
		[command waitUntilCompleted];
		timing->copy_complete_ns = now_ns();
		timing->gpu_start_seconds = command.GPUStartTime;
		timing->gpu_end_seconds = command.GPUEndTime;
		if (command.status != MTLCommandBufferStatusCompleted) {
			fprintf(stderr, "native-openxr: Metal blit failed: %s\n", command.error.description.UTF8String);
			return XR_ERROR_RUNTIME_FAILURE;
		}
		return XR_SUCCESS;
	}
}

XrResult
mwxr_native_swapchain_blit_release(struct mwxr_native_swapchain *s, uint32_t index,
                                   void *source, void *event, uint64_t value,
                                   XrDuration timeout, struct mwxr_blit_timing *timing)
{
	XrResult result = mwxr_native_swapchain_blit(s, index, source, event, value, timeout, timing);
	if (result != XR_SUCCESS) return result;
	result = mwxr_native_swapchain_release(s);
	timing->release_ns = now_ns();
	return result;
}

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#define XR_USE_GRAPHICS_API_METAL
#define XR_NO_PROTOTYPES
#include "macos_wine_xr/native_openxr_backend.h"
#include <openxr/openxr_platform.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdbool.h>
#include <stdio.h>
#define MWXR_FUNCTIONS(X) \
	X(DestroyInstance) X(GetInstanceProperties) X(GetSystem) X(GetSystemProperties) \
	X(EnumerateViewConfigurationViews) X(GetMetalGraphicsRequirementsKHR) \
	X(CreateSession) X(DestroySession) X(EnumerateSwapchainFormats) \
	X(CreateSwapchain) X(DestroySwapchain) X(EnumerateSwapchainImages) \
	X(AcquireSwapchainImage) X(WaitSwapchainImage) X(ReleaseSwapchainImage) \
	X(BeginSession) X(EndSession) X(RequestExitSession) X(PollEvent) \
	X(EnumerateEnvironmentBlendModes) X(EnumerateReferenceSpaces) X(GetReferenceSpaceBoundsRect) X(CreateReferenceSpace) X(CreateActionSpace) X(DestroySpace) \
	X(LocateSpace) X(LocateViews) X(WaitFrame) X(BeginFrame) X(EndFrame) \
	X(StringToPath) X(PathToString) X(CreateActionSet) X(DestroyActionSet) X(CreateAction) X(DestroyAction) \
	X(SuggestInteractionProfileBindings) X(AttachSessionActionSets) X(SyncActions) \
	X(GetActionStateBoolean) X(GetActionStateFloat) X(GetActionStateVector2f) X(GetActionStatePose) \
	X(GetCurrentInteractionProfile) X(ApplyHapticFeedback) X(StopHapticFeedback)

struct mwxr_native_backend {
	void *loader;
	PFN_xrGetInstanceProcAddr gipa;
#define DECLARE(name) PFN_xr##name name;
	MWXR_FUNCTIONS(DECLARE)
#undef DECLARE
	XrInstance instance;
	XrSystemId system;
	XrSession session;
	id<MTLDevice> device;
	id<MTLCommandQueue> queue;
	struct mwxr_backend_info info;
	struct mwxr_native_swapchain *swapchains;
	uint64_t next_image_id;
};

struct mwxr_native_swapchain {
	struct mwxr_native_backend *backend;
	struct mwxr_native_swapchain *next;
	XrSwapchain handle;
	uint32_t count;
	XrSwapchainImageMetalKHR *images;
	struct mwxr_image_info *metadata;
	MTLSharedTextureHandle **shared;
	uint32_t *acquired;
	bool *waited;
	uint32_t acquired_count;
};

static inline XrResult
diagnose(XrResult result, const char *operation)
{
	if (XR_FAILED(result)) fprintf(stderr, "native-openxr: %s failed: XrResult=%d\n", operation, result);
	return result;
}

#define CHECK(call) do { result = diagnose((call), #call); if (XR_FAILED(result)) goto fail; } while (0)


XrResult mwxr_native_backend_open_host(const char *loader, const XrApplicationInfo *app, struct mwxr_native_backend **backend);
XrResult mwxr_native_backend_create_session(struct mwxr_native_backend *backend);

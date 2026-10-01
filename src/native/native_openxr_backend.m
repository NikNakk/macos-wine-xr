// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#define XR_USE_GRAPHICS_API_METAL
#define XR_NO_PROTOTYPES
#include "macos_wine_xr/native_openxr_backend.h"
#include <openxr/openxr_platform.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "native_openxr_internal.h"

static XrResult
backend_open(const char *loader_path, const XrApplicationInfo *app, bool session_now, struct mwxr_native_backend **out_backend)
{
	if (!out_backend) return XR_ERROR_VALIDATION_FAILURE;
	*out_backend = NULL;
	struct mwxr_native_backend *b = calloc(1, sizeof(*b));
	if (!b) return XR_ERROR_OUT_OF_MEMORY;
	b->next_image_id = 1;
	XrResult result = XR_ERROR_INITIALIZATION_FAILED;
	XrExtensionProperties *extensions = NULL;
	XrViewConfigurationView *views = NULL;
	if (!loader_path) loader_path = getenv("MWXR_OPENXR_LOADER");
	if (!loader_path) loader_path = "libopenxr_loader.dylib";
	b->loader = dlopen(loader_path, RTLD_NOW | RTLD_LOCAL);
	if (!b->loader) {
		fprintf(stderr, "native-openxr: loader '%s': %s\n", loader_path, dlerror());
		goto fail;
	}
	b->gipa = (PFN_xrGetInstanceProcAddr)dlsym(b->loader, "xrGetInstanceProcAddr");
	if (!b->gipa) goto fail;
	PFN_xrEnumerateInstanceExtensionProperties enumerate = NULL;
	PFN_xrCreateInstance create = NULL;
	CHECK(b->gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction *)&enumerate));
	CHECK(b->gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&create));
	uint32_t count = 0;
	CHECK(enumerate(NULL, 0, &count, NULL));
	extensions = calloc(count, sizeof(*extensions));
	if (count && !extensions) { result = XR_ERROR_OUT_OF_MEMORY; goto fail; }
	for (uint32_t i = 0; i < count; ++i) extensions[i].type = XR_TYPE_EXTENSION_PROPERTIES;
	CHECK(enumerate(NULL, count, &count, extensions));
	bool metal = false;
	for (uint32_t i = 0; i < count; ++i)
		metal |= strcmp(extensions[i].extensionName, XR_KHR_METAL_ENABLE_EXTENSION_NAME) == 0;
	if (!metal) { result = diagnose(XR_ERROR_EXTENSION_NOT_PRESENT, XR_KHR_METAL_ENABLE_EXTENSION_NAME); goto fail; }
	const char *enabled[] = {XR_KHR_METAL_ENABLE_EXTENSION_NAME};
	XrInstanceCreateInfo ci = {.type = XR_TYPE_INSTANCE_CREATE_INFO,
	                         .enabledExtensionCount = 1, .enabledExtensionNames = enabled};
	strcpy(ci.applicationInfo.applicationName, "macos-wine-xr native host");
	ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	if (app) ci.applicationInfo = *app;
	CHECK(create(&ci, &b->instance));
	/* Load destroy first so partial initialization has a valid teardown path. */
#define LOAD(name) CHECK(b->gipa(b->instance, "xr" #name, (PFN_xrVoidFunction *)&b->name));
	MWXR_FUNCTIONS(LOAD)
#undef LOAD
	b->info.runtime.type = XR_TYPE_INSTANCE_PROPERTIES;
	CHECK(b->GetInstanceProperties(b->instance, &b->info.runtime));
	XrSystemGetInfo si = {.type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
	CHECK(b->GetSystem(b->instance, &si, &b->system));
	b->info.system.type = XR_TYPE_SYSTEM_PROPERTIES;
	CHECK(b->GetSystemProperties(b->instance, b->system, &b->info.system));
	CHECK(b->EnumerateViewConfigurationViews(b->instance, b->system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	                                        0, &count, NULL));
	if (!count) { result = XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED; goto fail; }
	views = calloc(count, sizeof(*views));
	if (!views) { result = XR_ERROR_OUT_OF_MEMORY; goto fail; }
	for (uint32_t i = 0; i < count; ++i) views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	CHECK(b->EnumerateViewConfigurationViews(b->instance, b->system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	                                        count, &count, views));
	b->info.view_count = count;
	for (uint32_t i = 0; i < count; ++i) {
		if (views[i].recommendedImageRectWidth > b->info.recommended_width)
			b->info.recommended_width = views[i].recommendedImageRectWidth;
		if (views[i].recommendedImageRectHeight > b->info.recommended_height)
			b->info.recommended_height = views[i].recommendedImageRectHeight;
		fprintf(stderr, "native-openxr: view=%u recommended=%ux%u samples=%u\n", i,
		        views[i].recommendedImageRectWidth, views[i].recommendedImageRectHeight,
		        views[i].recommendedSwapchainSampleCount);
	}
	XrGraphicsRequirementsMetalKHR requirements = {.type = XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR};
	CHECK(b->GetMetalGraphicsRequirementsKHR(b->instance, b->system, &requirements));
	/* Use the runtime-selected device, not MTLCreateSystemDefaultDevice(). */
	b->device = [(id<MTLDevice>)requirements.metalDevice retain];
	if (!b->device) { result = XR_ERROR_GRAPHICS_DEVICE_INVALID; goto fail; }
	b->queue = [b->device newCommandQueue];
	if (!b->queue) { result = XR_ERROR_OUT_OF_MEMORY; goto fail; }
	if (session_now) CHECK(mwxr_native_backend_create_session(b));
	free(extensions);
	free(views);
	*out_backend = b;
	return XR_SUCCESS;
fail:
	free(extensions);
	free(views);
	mwxr_native_backend_close(b);
	return result;
}

XrResult
mwxr_native_backend_open(const char *loader_path, struct mwxr_native_backend **out_backend)
{
	@autoreleasepool { return backend_open(loader_path,NULL,true,out_backend); }
}

void
mwxr_native_backend_close(struct mwxr_native_backend *b)
{
	if (!b) return;
	while (b->swapchains) mwxr_native_swapchain_destroy(b->swapchains);
	if (b->session && b->DestroySession) diagnose(b->DestroySession(b->session), "xrDestroySession");
	[b->queue release];
	[b->device release];
	if (b->instance && b->DestroyInstance) diagnose(b->DestroyInstance(b->instance), "xrDestroyInstance");
	if (b->loader) dlclose(b->loader);
	free(b);
}

const struct mwxr_backend_info *
mwxr_native_backend_info(const struct mwxr_native_backend *b) { return b ? &b->info : NULL; }

void *mwxr_native_backend_metal_device(struct mwxr_native_backend *b) { return b ? b->device : NULL; }
void *mwxr_native_swapchain_metal_texture(struct mwxr_native_swapchain *s, uint32_t i)
{
	return s && i < s->count ? s->images[i].texture : NULL;
}

XrResult
mwxr_native_backend_formats(struct mwxr_native_backend *b, uint32_t capacity, uint32_t *count, int64_t *formats)
{
	if (!b || !count || (capacity && !formats)) return XR_ERROR_VALIDATION_FAILURE;
	return diagnose(b->EnumerateSwapchainFormats(b->session, capacity, count, formats), "xrEnumerateSwapchainFormats");
}


XrResult mwxr_native_backend_open_host(const char *loader, const XrApplicationInfo *app, struct mwxr_native_backend **out)
{
 @autoreleasepool { return backend_open(loader,app,false,out); }
}
XrResult mwxr_native_backend_create_session(struct mwxr_native_backend *b)
{
 if (!b || b->session) return XR_ERROR_LIMIT_REACHED;
 XrGraphicsBindingMetalKHR binding = {.type=XR_TYPE_GRAPHICS_BINDING_METAL_KHR,.commandQueue=b->queue};
 XrSessionCreateInfo ci = {.type=XR_TYPE_SESSION_CREATE_INFO,.next=&binding,.systemId=b->system};
 return b->CreateSession(b->instance,&ci,&b->session);
}

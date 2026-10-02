// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#define XR_NO_PROTOTYPES
#include <cstdio>
#include <cstring>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <windows.h>
#include <d3d11.h>
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr_platform.h>
#include <cmath>
#define CHECK(call)                                                                                                    \
	do {                                                                                                           \
		XrResult x = (call);                                                                                   \
		if (XR_FAILED(x)) {                                                                                    \
			fprintf(stderr, "%s: %d\n", #call, x);                                                         \
			return 1;                                                                                      \
		}                                                                                                      \
	} while (0)
int main(int argc, char **argv) {
	if ((argc != 2 && argc != 3) || (argc == 3 && strcmp(argv[2], "--space-velocity") && strcmp(argv[2], "--swapchains")))
		return 2;
	HMODULE dll = LoadLibraryA(argv[1]);
	if (!dll) {
		fprintf(stderr, "LoadLibrary=%lu\n", GetLastError());
		return 1;
	}
	auto negotiate =
	    (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(dll, "xrNegotiateLoaderRuntimeInterface");
	XrNegotiateLoaderInfo li = {XR_LOADER_INTERFACE_STRUCT_LOADER_INFO,
				    1,
				    sizeof(li),
				    1,
				    1,
				    XR_MAKE_VERSION(1, 0, 0),
				    XR_MAKE_VERSION(1, 0, 0)};
	XrNegotiateRuntimeRequest ri = {XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, 1, sizeof(ri)};
	if (!negotiate)
		return 1;
	CHECK(negotiate(&li, &ri));
	PFN_xrCreateInstance create;
	CHECK(ri.getInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&create));
	XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
	strcpy(ci.applicationInfo.applicationName, "generic RPC smoke");
	ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	const char *extension = XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
	if (argc == 3) {
		ci.enabledExtensionCount = 1;
		ci.enabledExtensionNames = &extension;
	}
	XrInstance instance;
	CHECK(create(&ci, &instance));
#define LOAD(name)                                                                                                     \
	PFN_xr##name name;                                                                                             \
	CHECK(ri.getInstanceProcAddr(instance, "xr" #name, (PFN_xrVoidFunction *)&name))
	LOAD(GetInstanceProperties);
	LOAD(GetSystem);
	LOAD(GetSystemProperties);
	LOAD(EnumerateViewConfigurationViews);
	LOAD(DestroyInstance);
	XrInstanceProperties ip = {XR_TYPE_INSTANCE_PROPERTIES};
	CHECK(GetInstanceProperties(instance, &ip));
	XrSystemGetInfo si = {XR_TYPE_SYSTEM_GET_INFO};
	si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId system;
	CHECK(GetSystem(instance, &si, &system));
	XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
	CHECK(GetSystemProperties(instance, system, &sp));
	XrViewConfigurationView views[4] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW},
					    {XR_TYPE_VIEW_CONFIGURATION_VIEW},
					    {XR_TYPE_VIEW_CONFIGURATION_VIEW},
					    {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
	uint32_t count;
	CHECK(EnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 4, &count,
					      views));
	printf("RPC runtime=%s system=%s views=%u recommended=%ux%u\n", ip.runtimeName, sp.systemName, count,
	       views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight);
	if (argc == 3) {
		LOAD(GetD3D11GraphicsRequirementsKHR);
		LOAD(CreateSession);
		LOAD(DestroySession);
		LOAD(BeginSession);
		LOAD(PollEvent);
		LOAD(CreateReferenceSpace);
		LOAD(DestroySpace);
		LOAD(LocateSpace);
		LOAD(WaitFrame);
		LOAD(BeginFrame);
		LOAD(EndFrame);
		XrGraphicsRequirementsD3D11KHR requirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
		CHECK(GetD3D11GraphicsRequirementsKHR(instance, system, &requirements));
		ID3D11Device *device = nullptr;
		HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
		                               D3D11_SDK_VERSION, &device, nullptr, nullptr);
		if (FAILED(hr))
			return 1;
		XrGraphicsBindingD3D11KHR binding = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR, nullptr, device};
		XrSessionCreateInfo session_info = {XR_TYPE_SESSION_CREATE_INFO, &binding, 0, system};
		XrSession session;
		CHECK(CreateSession(instance, &session_info, &session));
		if (!strcmp(argv[2], "--swapchains")) {
			LOAD(CreateSwapchain);
			LOAD(EnumerateSwapchainImages);
			LOAD(DestroySwapchain);
			LOAD(AcquireSwapchainImage);
			LOAD(WaitSwapchainImage);
			LOAD(ReleaseSwapchainImage);
			for (uint32_t array = 1; array <= 2; array++) {
				XrSwapchainCreateInfo sc = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
				sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
				sc.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
				sc.sampleCount = 1; sc.width = 64; sc.height = 64;
				sc.faceCount = 1; sc.arraySize = array; sc.mipCount = 1;
				XrSwapchain chain;
				CHECK(CreateSwapchain(session, &sc, &chain));
				uint32_t count = 0;
				CHECK(EnumerateSwapchainImages(chain, 0, &count, nullptr));
				if (count == 0 || count > 16) return 1;
				XrSwapchainImageD3D11KHR images[16] = {};
				for (uint32_t i = 0; i < count; i++) images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
				CHECK(EnumerateSwapchainImages(chain, count, &count, (XrSwapchainImageBaseHeader *)images));
				for (uint32_t i = 0; i < count; i++) {
					D3D11_TEXTURE2D_DESC desc;
					images[i].texture->GetDesc(&desc);
					if (desc.ArraySize != array || desc.Width != 64 || desc.Height != 64) return 1;
				}
				XrSwapchainImageAcquireInfo acquire = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
				XrSwapchainImageWaitInfo wait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
				XrSwapchainImageReleaseInfo release = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
				uint32_t index;
				CHECK(AcquireSwapchainImage(chain, &acquire, &index));
				CHECK(WaitSwapchainImage(chain, &wait));
				CHECK(ReleaseSwapchainImage(chain, &release));
				CHECK(DestroySwapchain(chain));
				printf("OpenXR swapchain array=%u images=%u import/acquire/wait/release passed\n", array, count);
			}
			CHECK(DestroySession(session));
			device->Release();
			CHECK(DestroyInstance(instance));
			FreeLibrary(dll);
			return 0;
		}

		bool ready = false;
		for (unsigned i = 0; i < 2000 && !ready; i++) {
			XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
			XrResult result = PollEvent(instance, &event);
			if (result == XR_SUCCESS && event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
				ready = ((XrEventDataSessionStateChanged *)&event)->state == XR_SESSION_STATE_READY;
			else if (XR_FAILED(result))
				return 1;
			if (!ready)
				Sleep(5);
		}
		if (!ready)
			return 1;
		XrSessionBeginInfo begin = {XR_TYPE_SESSION_BEGIN_INFO, nullptr,
		                            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
		CHECK(BeginSession(session, &begin));
		XrReferenceSpaceCreateInfo space_info = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
		space_info.poseInReferenceSpace.orientation.w = 1;
		space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
		XrSpace view, local;
		CHECK(CreateReferenceSpace(session, &space_info, &view));
		space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		CHECK(CreateReferenceSpace(session, &space_info, &local));
		XrFrameWaitInfo wait = {XR_TYPE_FRAME_WAIT_INFO};
		XrFrameState state = {XR_TYPE_FRAME_STATE};
		CHECK(WaitFrame(session, &wait, &state));
		XrFrameBeginInfo frame_begin = {XR_TYPE_FRAME_BEGIN_INFO};
		CHECK(BeginFrame(session, &frame_begin));
		XrSpaceVelocity velocity = {XR_TYPE_SPACE_VELOCITY};
		XrSpaceLocation location = {XR_TYPE_SPACE_LOCATION, &velocity};
		CHECK(LocateSpace(view, local, state.predictedDisplayTime, &location));
		if (!(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
			return 1;
		if (velocity.next || location.next != &velocity)
			return 1;
		if ((velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) &&
		    (!std::isfinite(velocity.linearVelocity.x) || !std::isfinite(velocity.linearVelocity.y) ||
		     !std::isfinite(velocity.linearVelocity.z)))
			return 1;
		if ((velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) &&
		    (!std::isfinite(velocity.angularVelocity.x) || !std::isfinite(velocity.angularVelocity.y) ||
		     !std::isfinite(velocity.angularVelocity.z)))
			return 1;
		printf("space velocity: location_flags=%llu velocity_flags=%llu\n",
		       (unsigned long long)location.locationFlags, (unsigned long long)velocity.velocityFlags);
		XrBaseOutStructure unknown = {XR_TYPE_UNKNOWN, nullptr};
		velocity.next = &unknown;
		if (LocateSpace(view, local, state.predictedDisplayTime, &location) != XR_ERROR_FEATURE_UNSUPPORTED)
			return 1;
		velocity.next = nullptr;
		location.next = nullptr;
		CHECK(LocateSpace(view, local, state.predictedDisplayTime, &location));
		XrFrameEndInfo end = {XR_TYPE_FRAME_END_INFO,           nullptr, state.predictedDisplayTime,
		                      XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 0,       nullptr};
		CHECK(EndFrame(session, &end));
		CHECK(DestroySpace(view));
		CHECK(DestroySpace(local));
		CHECK(DestroySession(session));
		device->Release();
	}
	CHECK(DestroyInstance(instance));
	FreeLibrary(dll);
	return 0;
}

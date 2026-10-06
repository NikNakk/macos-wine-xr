// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "xr_backend.h"
#include "log.h"

#include <cstring>
#include <dxgi.h>

namespace mwxr {

#define XR_CHECK(call)                                                                                                 \
	do {                                                                                                           \
		XrResult result_ = (call);                                                                             \
		if (XR_FAILED(result_)) {                                                                              \
			error = std::string(#call) + " failed: " + std::to_string(result_);                            \
			return false;                                                                                  \
		}                                                                                                      \
	} while (0)

static bool
HasExtension(const std::vector<XrExtensionProperties> &extensions, const char *name)
{
	for (const auto &extension : extensions) {
		if (!strcmp(extension.extensionName, name)) {
			return true;
		}
	}
	return false;
}

bool
XrBackend::Init(std::string &error)
{
	uint32_t count = 0;
	XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr));
	std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
	XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data()));
	const char *required[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME,
	                          XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME};
	for (const char *name : required) {
		if (!HasExtension(extensions, name)) {
			error = std::string("runtime lacks ") + name;
			return false;
		}
	}

	XrInstanceCreateInfo instanceInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
	strcpy(instanceInfo.applicationInfo.applicationName, "SteamVR (macos-wine-xr driver)");
	strcpy(instanceInfo.applicationInfo.engineName, "driver_mwxr");
	instanceInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	instanceInfo.enabledExtensionCount = 2;
	instanceInfo.enabledExtensionNames = required;
	XR_CHECK(xrCreateInstance(&instanceInfo, &instance_));

	XrInstanceProperties instanceProperties = {XR_TYPE_INSTANCE_PROPERTIES};
	XR_CHECK(xrGetInstanceProperties(instance_, &instanceProperties));
	runtimeName = instanceProperties.runtimeName;
	XR_CHECK(xrGetInstanceProcAddr(instance_, "xrConvertWin32PerformanceCounterToTimeKHR",
	                               (PFN_xrVoidFunction *)&qpcToTime_));
	XR_CHECK(xrGetInstanceProcAddr(instance_, "xrConvertTimeToWin32PerformanceCounterKHR",
	                               (PFN_xrVoidFunction *)&timeToQpc_));
	XR_CHECK(xrGetInstanceProcAddr(instance_, "xrGetD3D11GraphicsRequirementsKHR",
	                               (PFN_xrVoidFunction *)&getRequirements_));

	XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XR_CHECK(xrGetSystem(instance_, &systemInfo, &system_));
	XrSystemProperties systemProperties = {XR_TYPE_SYSTEM_PROPERTIES};
	XR_CHECK(xrGetSystemProperties(instance_, system_, &systemProperties));
	systemName = systemProperties.systemName;

	uint32_t viewCount = 0;
	XR_CHECK(xrEnumerateViewConfigurationViews(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
	                                           &viewCount, nullptr));
	if (viewCount != 2) {
		error = "primary stereo view configuration does not have two views";
		return false;
	}
	XrViewConfigurationView views[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
	XR_CHECK(xrEnumerateViewConfigurationViews(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
	                                           &viewCount, views));
	recommendedWidth = views[0].recommendedImageRectWidth;
	recommendedHeight = views[0].recommendedImageRectHeight;

	// D3D11 device on the adapter the runtime requires.
	XrGraphicsRequirementsD3D11KHR requirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
	XR_CHECK(getRequirements_(instance_, system_, &requirements));
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory))) {
		error = "CreateDXGIFactory1 failed";
		return false;
	}
	IDXGIAdapter1 *adapter = nullptr;
	for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
		DXGI_ADAPTER_DESC1 desc;
		adapter->GetDesc1(&desc);
		if (!memcmp(&desc.AdapterLuid, &requirements.adapterLuid, sizeof(LUID))) {
			break;
		}
		adapter->Release();
		adapter = nullptr;
	}
	factory->Release();
	if (!adapter) {
		error = "no DXGI adapter matches the runtime's LUID";
		return false;
	}
	D3D_FEATURE_LEVEL level = requirements.minFeatureLevel > D3D_FEATURE_LEVEL_11_0 ? requirements.minFeatureLevel
	                                                                              : D3D_FEATURE_LEVEL_11_0;
	HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
	                               &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context_);
	adapter->Release();
	if (FAILED(hr)) {
		error = "D3D11CreateDevice failed: " + std::to_string(hr);
		return false;
	}

	XrGraphicsBindingD3D11KHR binding = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
	binding.device = device;
	XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
	sessionInfo.next = &binding;
	sessionInfo.systemId = system_;
	XR_CHECK(xrCreateSession(instance_, &sessionInfo, &session_));

	// STAGE gives SteamVR a floor-level standing universe when available.
	uint32_t spaceCount = 0;
	XR_CHECK(xrEnumerateReferenceSpaces(session_, 0, &spaceCount, nullptr));
	std::vector<XrReferenceSpaceType> spaces(spaceCount);
	XR_CHECK(xrEnumerateReferenceSpaces(session_, spaceCount, &spaceCount, spaces.data()));
	XrReferenceSpaceCreateInfo spaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	for (auto type : spaces) {
		if (type == XR_REFERENCE_SPACE_TYPE_STAGE) {
			spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
		}
	}
	XR_CHECK(xrCreateReferenceSpace(session_, &spaceInfo, &baseSpace_));
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	XR_CHECK(xrCreateReferenceSpace(session_, &spaceInfo, &viewSpace_));

	uint32_t formatCount = 0;
	XR_CHECK(xrEnumerateSwapchainFormats(session_, 0, &formatCount, nullptr));
	formats_.resize(formatCount);
	XR_CHECK(xrEnumerateSwapchainFormats(session_, formatCount, &formatCount, formats_.data()));

	// Monado locates views only in a begun session, and SteamVR needs the FOV
	// and eye poses when the HMD activates: begin as soon as the session is READY.
	for (int attempt = 0; attempt < 500 && !running_; ++attempt) {
		XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
		while (xrPollEvent(instance_, &event) == XR_SUCCESS) {
			if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
				state_ = reinterpret_cast<XrEventDataSessionStateChanged *>(&event)->state;
				if (state_ == XR_SESSION_STATE_READY) {
					BeginSessionLocked();
				}
			}
			event = {XR_TYPE_EVENT_DATA_BUFFER};
		}
		if (!running_) {
			Sleep(10);
		}
	}
	if (!running_) {
		error = "session did not become READY within 5 s";
		return false;
	}

	// Eye poses relative to the head and field of view, from the views now.
	XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
	locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locateInfo.displayTime = NowXrTime();
	locateInfo.space = viewSpace_;
	XrViewState viewState = {XR_TYPE_VIEW_STATE};
	XrView located[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
	XR_CHECK(xrLocateViews(session_, &locateInfo, &viewState, 2, &viewCount, located));
	for (int eye = 0; eye < 2; ++eye) {
		fov[eye] = located[eye].fov;
		eyeInHead[eye] = located[eye].pose;
	}

	Log("OpenXR runtime '%s', system '%s', %ux%u per eye, %s space, %u swapchain formats\n", runtimeName.c_str(),
	    systemName.c_str(), recommendedWidth, recommendedHeight,
	    spaceInfo.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE ? "STAGE" : "LOCAL", formatCount);
	return true;
}

void
XrBackend::Shutdown()
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &swapchain : swapchains_) {
		if (swapchain.handle) {
			xrDestroySwapchain(swapchain.handle);
		}
	}
	swapchains_.clear();
	if (session_) {
		xrDestroySession(session_);
		session_ = XR_NULL_HANDLE;
	}
	if (instance_) {
		xrDestroyInstance(instance_);
		instance_ = XR_NULL_HANDLE;
	}
	if (context_) {
		context_->Release();
		context_ = nullptr;
	}
	if (device) {
		device->Release();
		device = nullptr;
	}
}

XrTime
XrBackend::NowXrTime()
{
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	XrTime time = 0;
	qpcToTime_(instance_, &counter, &time);
	return time;
}

void
XrBackend::PollEvents()
{
	XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
	while (instance_ && xrPollEvent(instance_, &event) == XR_SUCCESS) {
		if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			auto *changed = reinterpret_cast<XrEventDataSessionStateChanged *>(&event);
			std::lock_guard<std::mutex> lock(mutex_);
			state_ = changed->state;
			Log("OpenXR session state %d\n", state_);
			if (state_ == XR_SESSION_STATE_READY) {
				BeginSessionLocked();
			} else if (state_ == XR_SESSION_STATE_STOPPING) {
				xrEndSession(session_);
				running_ = frameBegun_ = false;
			} else if (state_ == XR_SESSION_STATE_EXITING || state_ == XR_SESSION_STATE_LOSS_PENDING) {
				running_ = frameBegun_ = false;
			}
		}
		event = {XR_TYPE_EVENT_DATA_BUFFER};
	}
}

bool
XrBackend::BeginSessionLocked()
{
	XrSessionBeginInfo beginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
	beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	XrResult result = xrBeginSession(session_, &beginInfo);
	running_ = XR_SUCCEEDED(result);
	Log("xrBeginSession: %d\n", result);
	return running_;
}

bool
XrBackend::IsRunning()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return running_;
}

bool
XrBackend::LocateHeadNow(XrPosef &pose, XrVector3f &linearVelocity, XrVector3f &angularVelocity, bool &positionValid)
{
	if (!session_) {
		return false;
	}
	XrSpaceVelocity velocity = {XR_TYPE_SPACE_VELOCITY};
	XrSpaceLocation location = {XR_TYPE_SPACE_LOCATION, &velocity};
	if (XR_FAILED(xrLocateSpace(viewSpace_, baseSpace_, NowXrTime(), &location)) ||
	    !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
		return false;
	}
	pose = location.pose;
	positionValid = (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
	linearVelocity = (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) ? velocity.linearVelocity
	                                                                                 : XrVector3f{};
	angularVelocity = (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) ? velocity.angularVelocity
	                                                                                   : XrVector3f{};
	return true;
}

bool
XrBackend::WaitAndBeginFrame(double &vsyncOffsetSeconds, double &framePeriodSeconds)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!running_ || frameBegun_) {
		return false;
	}
	frameState_ = {XR_TYPE_FRAME_STATE};
	if (XR_FAILED(xrWaitFrame(session_, nullptr, &frameState_))) {
		return false;
	}
	if (XR_FAILED(xrBeginFrame(session_, nullptr))) {
		return false;
	}
	frameBegun_ = true;
	framePeriodSeconds = frameState_.predictedDisplayPeriod / 1e9;
	XrTime vsync = frameState_.predictedDisplayTime - frameState_.predictedDisplayPeriod;
	vsyncOffsetSeconds = (vsync - NowXrTime()) / 1e9;
	return true;
}

int64_t
XrBackend::ChooseFormat(DXGI_FORMAT source)
{
	// Copy-compatible formats only (same typeless family), exact match first.
	std::vector<DXGI_FORMAT> candidates = {source};
	switch (source) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		candidates.insert(candidates.end(), {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM});
		break;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		candidates.insert(candidates.end(), {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM});
		break;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: candidates.push_back(DXGI_FORMAT_R16G16B16A16_FLOAT); break;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: candidates.push_back(DXGI_FORMAT_R10G10B10A2_UNORM); break;
	default: break;
	}
	for (DXGI_FORMAT candidate : candidates) {
		for (int64_t format : formats_) {
			if (format == candidate) {
				return format;
			}
		}
	}
	return 0;
}

XrBackend::EyeSwapchain *
XrBackend::SwapchainFor(size_t layer, int eye, uint32_t width, uint32_t height, DXGI_FORMAT sourceFormat)
{
	size_t index = layer * 2 + eye;
	if (swapchains_.size() <= index) {
		swapchains_.resize(index + 1);
	}
	EyeSwapchain &swapchain = swapchains_[index];
	int64_t format = ChooseFormat(sourceFormat);
	if (!format) {
		static bool logged = false;
		if (!logged) {
			Log("No runtime swapchain format can receive a copy of DXGI format %d\n", sourceFormat);
			logged = true;
		}
		return nullptr;
	}
	if (swapchain.handle && swapchain.width == width && swapchain.height == height && swapchain.format == format) {
		return &swapchain;
	}
	if (swapchain.handle) {
		xrDestroySwapchain(swapchain.handle);
		swapchain = {};
	}
	XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	info.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
	info.format = format;
	info.sampleCount = 1;
	info.width = width;
	info.height = height;
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	if (XR_FAILED(xrCreateSwapchain(session_, &info, &swapchain.handle))) {
		Log("xrCreateSwapchain %ux%u format %lld failed\n", width, height, (long long)format);
		swapchain = {};
		return nullptr;
	}
	uint32_t count = 0;
	xrEnumerateSwapchainImages(swapchain.handle, 0, &count, nullptr);
	swapchain.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
	xrEnumerateSwapchainImages(swapchain.handle, count, &count,
	                           reinterpret_cast<XrSwapchainImageBaseHeader *>(swapchain.images.data()));
	swapchain.width = width;
	swapchain.height = height;
	swapchain.format = format;
	Log("Swapchain layer %zu eye %d: %ux%u format %lld, %u images\n", layer, eye, width, height, (long long)format,
	    count);
	return &swapchain;
}

void
XrBackend::Present(const std::vector<LayerSubmit> &layers)
{
	if (!frameBegun_) {
		double offset, period;
		WaitAndBeginFrame(offset, period);
	}
	std::lock_guard<std::mutex> lock(mutex_);
	if (!running_ || !frameBegun_) {
		return;
	}
	std::vector<XrCompositionLayerProjection> projections;
	std::vector<XrCompositionLayerProjectionView> views(layers.size() * 2);
	projections.reserve(layers.size());
	if (frameState_.shouldRender) {
		for (size_t i = 0; i < layers.size(); ++i) {
			bool complete = true;
			for (int eye = 0; eye < 2 && complete; ++eye) {
				const EyeSubmit &submit = layers[i].eye[eye];
				D3D11_TEXTURE2D_DESC desc;
				submit.texture->GetDesc(&desc);
				uint32_t width = submit.box.right - submit.box.left;
				uint32_t height = submit.box.bottom - submit.box.top;
				EyeSwapchain *swapchain = SwapchainFor(i, eye, width, height, desc.Format);
				uint32_t image = 0;
				XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
				waitInfo.timeout = XR_INFINITE_DURATION;
				if (!swapchain || XR_FAILED(xrAcquireSwapchainImage(swapchain->handle, nullptr, &image)) ||
				    XR_FAILED(xrWaitSwapchainImage(swapchain->handle, &waitInfo))) {
					complete = false;
					break;
				}
				context_->CopySubresourceRegion(swapchain->images[image].texture, 0, 0, 0, 0, submit.texture,
				                                0, &submit.box);
				xrReleaseSwapchainImage(swapchain->handle, nullptr);

				XrCompositionLayerProjectionView &view = views[i * 2 + eye];
				view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
				view.pose = submit.pose;
				view.fov = submit.fov;
				view.subImage.swapchain = swapchain->handle;
				view.subImage.imageRect.extent = {(int32_t)width, (int32_t)height};
			}
			if (!complete) {
				continue;
			}
			XrCompositionLayerProjection projection = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
			projection.space = baseSpace_;
			projection.viewCount = 2;
			projection.views = &views[i * 2];
			if (!projections.empty()) {
				projection.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			}
			projections.push_back(projection);
		}
	}
	std::vector<const XrCompositionLayerBaseHeader *> headers;
	for (auto &projection : projections) {
		headers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection));
	}
	XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
	endInfo.displayTime = frameState_.predictedDisplayTime;
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.layerCount = (uint32_t)headers.size();
	endInfo.layers = headers.data();
	XrResult result = xrEndFrame(session_, &endInfo);
	frameBegun_ = false;
	if (XR_FAILED(result)) {
		static int failures = 0;
		if (failures++ < 5) {
			Log("xrEndFrame failed: %d (%u layers)\n", result, endInfo.layerCount);
		}
	}
}

} // namespace mwxr

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "xr_backend.h"
#include "log.h"

#include <cstring>
#include <dxgi.h>

#if __has_include("dxmt_native_interop.h")
#include "dxmt_native_interop.h"
#define MWXR_HAVE_DXMT_INTEROP 1
#endif
#include <set>

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
	if (!HasExtension(extensions, XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) {
		error = "runtime lacks " XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
		return false;
	}
	// Optional: without it, the current XrTime is estimated from the frame loop.
	const char *required[3] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
	uint32_t enabledCount = 1;
	bool timeConversion = HasExtension(extensions, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
	if (timeConversion) {
		required[enabledCount++] = XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME;
	}
	// Optional: only SteamVR's virtual-display mode needs it.
	hasDisplayDistortion = HasExtension(extensions, XR_MNDX_DISPLAY_DISTORTION_EXTENSION_NAME);
	if (hasDisplayDistortion) {
		required[enabledCount++] = XR_MNDX_DISPLAY_DISTORTION_EXTENSION_NAME;
	}

	XrInstanceCreateInfo instanceInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
	strcpy(instanceInfo.applicationInfo.applicationName, "SteamVR (macos-wine-xr driver)");
	strcpy(instanceInfo.applicationInfo.engineName, "driver_mwxr");
	instanceInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	instanceInfo.enabledExtensionCount = enabledCount;
	instanceInfo.enabledExtensionNames = required;
	XR_CHECK(xrCreateInstance(&instanceInfo, &instance_));

	XrInstanceProperties instanceProperties = {XR_TYPE_INSTANCE_PROPERTIES};
	XR_CHECK(xrGetInstanceProperties(instance_, &instanceProperties));
	runtimeName = instanceProperties.runtimeName;
	if (timeConversion) {
		XR_CHECK(xrGetInstanceProcAddr(instance_, "xrConvertWin32PerformanceCounterToTimeKHR",
		                               (PFN_xrVoidFunction *)&qpcToTime_));
		XR_CHECK(xrGetInstanceProcAddr(instance_, "xrConvertTimeToWin32PerformanceCounterKHR",
		                               (PFN_xrVoidFunction *)&timeToQpc_));
	}
	XR_CHECK(xrGetInstanceProcAddr(instance_, "xrGetD3D11GraphicsRequirementsKHR",
	                               (PFN_xrVoidFunction *)&getRequirements_));

	XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XR_CHECK(xrGetSystem(instance_, &systemInfo, &system_));
	XrSystemProperties systemProperties = {XR_TYPE_SYSTEM_PROPERTIES};
	XR_CHECK(xrGetSystemProperties(instance_, system_, &systemProperties));
	systemName = systemProperties.systemName;
	if (hasDisplayDistortion) {
		PFN_xrGetDisplayDistortionPropertiesMNDX getDisplay = nullptr;
		xrGetInstanceProcAddr(instance_, "xrGetDisplayDistortionPropertiesMNDX", (PFN_xrVoidFunction *)&getDisplay);
		xrGetInstanceProcAddr(instance_, "xrComputeDisplayDistortionMNDX", (PFN_xrVoidFunction *)&computeDistortion_);
		XrResult result = getDisplay && computeDistortion_ ? getDisplay(instance_, system_, &display)
		                                                   : XR_ERROR_FUNCTION_UNSUPPORTED;
		hasDisplayDistortion = XR_SUCCEEDED(result) && display.viewCount == 2;
		Log("Display distortion: %s (%d), display %dx%d, %.2f Hz\n", hasDisplayDistortion ? "available" : "unusable",
		    result, display.displaySize.width, display.displaySize.height, display.nominalRefreshRate);
	}

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
	baseSpaceType_ = spaceInfo.referenceSpaceType;
	if (baseSpaceType_ == XR_REFERENCE_SPACE_TYPE_STAGE) {
		XrExtent2Df bounds = {};
		if (xrGetReferenceSpaceBoundsRect(session_, XR_REFERENCE_SPACE_TYPE_STAGE, &bounds) == XR_SUCCESS) {
			playAreaWidth = bounds.width;
			playAreaDepth = bounds.height;
		}
	}
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	XR_CHECK(xrCreateReferenceSpace(session_, &spaceInfo, &viewSpace_));

	if (!CreateActions(error)) {
		return false;
	}

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

	if (!qpcToTime_) {
		// One empty frame anchors the frame-loop time estimate before any
		// view or pose is located.
		XrFrameState state = {XR_TYPE_FRAME_STATE};
		XR_CHECK(xrWaitFrame(session_, nullptr, &state));
		AnchorTime(state.predictedDisplayTime);
		XR_CHECK(xrBeginFrame(session_, nullptr));
		XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
		endInfo.displayTime = state.predictedDisplayTime;
		endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		XR_CHECK(xrEndFrame(session_, &endInfo));
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

	Log("OpenXR runtime '%s', system '%s', %ux%u per eye, %s space, %u swapchain formats, time %s\n",
	    runtimeName.c_str(), systemName.c_str(), recommendedWidth, recommendedHeight,
	    baseSpaceType_ == XR_REFERENCE_SPACE_TYPE_STAGE ? "STAGE" : "LOCAL", formatCount,
	    qpcToTime_ ? "converted from QueryPerformanceCounter" : "estimated from the frame loop");
	return true;
}

bool
XrBackend::CreateActions(std::string &error)
{
	XR_CHECK(xrStringToPath(instance_, "/user/hand/left", &handPaths_[0]));
	XR_CHECK(xrStringToPath(instance_, "/user/hand/right", &handPaths_[1]));
	XrActionSetCreateInfo setInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
	strcpy(setInfo.actionSetName, "steamvr");
	strcpy(setInfo.localizedActionSetName, "SteamVR");
	XR_CHECK(xrCreateActionSet(instance_, &setInfo, &actionSet_));

	struct ActionDef
	{
		XrAction *action;
		const char *name;
		XrActionType type;
	} defs[] = {
	    {&gripPose_, "grip_pose", XR_ACTION_TYPE_POSE_INPUT},
	    {&trigger_, "trigger", XR_ACTION_TYPE_FLOAT_INPUT},
	    {&triggerTouch_, "trigger_touch", XR_ACTION_TYPE_BOOLEAN_INPUT},
	    {&squeeze_, "squeeze", XR_ACTION_TYPE_FLOAT_INPUT},
	    {&thumbstick_, "thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT},
	    {&thumbstickClick_, "thumbstick_click", XR_ACTION_TYPE_BOOLEAN_INPUT},
	    {&thumbstickTouch_, "thumbstick_touch", XR_ACTION_TYPE_BOOLEAN_INPUT},
	    {&lowerClick_, "lower_click", XR_ACTION_TYPE_BOOLEAN_INPUT},
	    {&lowerTouch_, "lower_touch", XR_ACTION_TYPE_BOOLEAN_INPUT},
	    {&upperClick_, "upper_click", XR_ACTION_TYPE_BOOLEAN_INPUT},
	    {&upperTouch_, "upper_touch", XR_ACTION_TYPE_BOOLEAN_INPUT},
	    {&menuClick_, "menu_click", XR_ACTION_TYPE_BOOLEAN_INPUT},
	    {&thumbrestTouch_, "thumbrest_touch", XR_ACTION_TYPE_BOOLEAN_INPUT},
	    {&haptic_, "haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT},
	};
	for (const auto &def : defs) {
		XrActionCreateInfo info = {XR_TYPE_ACTION_CREATE_INFO};
		strcpy(info.actionName, def.name);
		strcpy(info.localizedActionName, def.name);
		info.actionType = def.type;
		info.countSubactionPaths = 2;
		info.subactionPaths = handPaths_;
		XR_CHECK(xrCreateAction(actionSet_, &info, def.action));
	}

	auto suggest = [&](const char *profile, std::initializer_list<std::pair<XrAction, const char *>> bindings) {
		std::vector<XrActionSuggestedBinding> suggested;
		for (const auto &binding : bindings) {
			XrPath path;
			if (XR_SUCCEEDED(xrStringToPath(instance_, binding.second, &path))) {
				suggested.push_back({binding.first, path});
			}
		}
		XrPath profilePath;
		xrStringToPath(instance_, profile, &profilePath);
		XrInteractionProfileSuggestedBinding info = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
		info.interactionProfile = profilePath;
		info.countSuggestedBindings = (uint32_t)suggested.size();
		info.suggestedBindings = suggested.data();
		XrResult result = xrSuggestInteractionProfileBindings(instance_, &info);
		Log("Suggested bindings for %s: %d\n", profile, result);
	};
	suggest("/interaction_profiles/oculus/touch_controller",
	        {{gripPose_, "/user/hand/left/input/grip/pose"},
	         {gripPose_, "/user/hand/right/input/grip/pose"},
	         {trigger_, "/user/hand/left/input/trigger/value"},
	         {trigger_, "/user/hand/right/input/trigger/value"},
	         {triggerTouch_, "/user/hand/left/input/trigger/touch"},
	         {triggerTouch_, "/user/hand/right/input/trigger/touch"},
	         {squeeze_, "/user/hand/left/input/squeeze/value"},
	         {squeeze_, "/user/hand/right/input/squeeze/value"},
	         {thumbstick_, "/user/hand/left/input/thumbstick"},
	         {thumbstick_, "/user/hand/right/input/thumbstick"},
	         {thumbstickClick_, "/user/hand/left/input/thumbstick/click"},
	         {thumbstickClick_, "/user/hand/right/input/thumbstick/click"},
	         {thumbstickTouch_, "/user/hand/left/input/thumbstick/touch"},
	         {thumbstickTouch_, "/user/hand/right/input/thumbstick/touch"},
	         {lowerClick_, "/user/hand/left/input/x/click"},
	         {lowerClick_, "/user/hand/right/input/a/click"},
	         {lowerTouch_, "/user/hand/left/input/x/touch"},
	         {lowerTouch_, "/user/hand/right/input/a/touch"},
	         {upperClick_, "/user/hand/left/input/y/click"},
	         {upperClick_, "/user/hand/right/input/b/click"},
	         {upperTouch_, "/user/hand/left/input/y/touch"},
	         {upperTouch_, "/user/hand/right/input/b/touch"},
	         {menuClick_, "/user/hand/left/input/menu/click"},
	         {thumbrestTouch_, "/user/hand/left/input/thumbrest/touch"},
	         {thumbrestTouch_, "/user/hand/right/input/thumbrest/touch"},
	         {haptic_, "/user/hand/left/output/haptic"},
	         {haptic_, "/user/hand/right/output/haptic"}});
	suggest("/interaction_profiles/khr/simple_controller",
	        {{gripPose_, "/user/hand/left/input/grip/pose"},
	         {gripPose_, "/user/hand/right/input/grip/pose"},
	         {trigger_, "/user/hand/left/input/select/click"},
	         {trigger_, "/user/hand/right/input/select/click"},
	         {menuClick_, "/user/hand/left/input/menu/click"},
	         {menuClick_, "/user/hand/right/input/menu/click"},
	         {haptic_, "/user/hand/left/output/haptic"},
	         {haptic_, "/user/hand/right/output/haptic"}});

	XrSessionActionSetsAttachInfo attach = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	attach.countActionSets = 1;
	attach.actionSets = &actionSet_;
	XR_CHECK(xrAttachSessionActionSets(session_, &attach));
	for (int hand = 0; hand < 2; ++hand) {
		XrActionSpaceCreateInfo spaceInfo = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
		spaceInfo.action = gripPose_;
		spaceInfo.subactionPath = handPaths_[hand];
		spaceInfo.poseInActionSpace.orientation.w = 1.0f;
		XR_CHECK(xrCreateActionSpace(session_, &spaceInfo, &gripSpaces_[hand]));
	}
	return true;
}

bool
XrBackend::UpdateHands(HandState hands[2])
{
	if (!actionSet_ || state_ != XR_SESSION_STATE_FOCUSED) {
		return false;
	}
	XrActiveActionSet active = {actionSet_, XR_NULL_PATH};
	XrActionsSyncInfo syncInfo = {XR_TYPE_ACTIONS_SYNC_INFO};
	syncInfo.countActiveActionSets = 1;
	syncInfo.activeActionSets = &active;
	if (XR_FAILED(xrSyncActions(session_, &syncInfo))) {
		return false;
	}
	XrTime now = NowXrTime();
	for (int hand = 0; hand < 2; ++hand) {
		HandState &out = hands[hand];
		XrActionStateGetInfo get = {XR_TYPE_ACTION_STATE_GET_INFO};
		get.subactionPath = handPaths_[hand];
		auto boolean = [&](XrAction action) {
			XrActionStateBoolean state = {XR_TYPE_ACTION_STATE_BOOLEAN};
			get.action = action;
			return XR_SUCCEEDED(xrGetActionStateBoolean(session_, &get, &state)) && state.isActive &&
			       state.currentState;
		};
		auto scalar = [&](XrAction action) {
			XrActionStateFloat state = {XR_TYPE_ACTION_STATE_FLOAT};
			get.action = action;
			return XR_SUCCEEDED(xrGetActionStateFloat(session_, &get, &state)) && state.isActive
			           ? state.currentState
			           : 0.0f;
		};
		XrActionStatePose poseState = {XR_TYPE_ACTION_STATE_POSE};
		get.action = gripPose_;
		out.active = XR_SUCCEEDED(xrGetActionStatePose(session_, &get, &poseState)) && poseState.isActive;
		out.trigger = scalar(trigger_);
		out.squeeze = scalar(squeeze_);
		XrActionStateVector2f stick = {XR_TYPE_ACTION_STATE_VECTOR2F};
		get.action = thumbstick_;
		out.thumbstick = XR_SUCCEEDED(xrGetActionStateVector2f(session_, &get, &stick)) && stick.isActive
		                     ? stick.currentState
		                     : XrVector2f{};
		out.triggerTouch = boolean(triggerTouch_);
		out.thumbstickClick = boolean(thumbstickClick_);
		out.thumbstickTouch = boolean(thumbstickTouch_);
		out.lowerClick = boolean(lowerClick_);
		out.lowerTouch = boolean(lowerTouch_);
		out.upperClick = boolean(upperClick_);
		out.upperTouch = boolean(upperTouch_);
		out.menuClick = boolean(menuClick_);
		out.thumbrestTouch = boolean(thumbrestTouch_);

		XrSpaceVelocity velocity = {XR_TYPE_SPACE_VELOCITY};
		XrSpaceLocation location = {XR_TYPE_SPACE_LOCATION, &velocity};
		out.poseValid = out.active && XR_SUCCEEDED(xrLocateSpace(gripSpaces_[hand], baseSpace_, now, &location)) &&
		                (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
		if (out.poseValid) {
			out.pose = location.pose;
			out.positionValid = (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
			out.linearVelocity = (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT)
			                         ? velocity.linearVelocity
			                         : XrVector3f{};
			out.angularVelocity = (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT)
			                          ? velocity.angularVelocity
			                          : XrVector3f{};
		}

		XrInteractionProfileState profile = {XR_TYPE_INTERACTION_PROFILE_STATE};
		if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(session_, handPaths_[hand], &profile)) &&
		    profile.interactionProfile != XR_NULL_PATH) {
			char name[XR_MAX_PATH_LENGTH];
			uint32_t length = 0;
			if (XR_SUCCEEDED(xrPathToString(instance_, profile.interactionProfile, sizeof(name), &length, name)) &&
			    currentProfile[hand] != name) {
				currentProfile[hand] = name;
				Log("%s hand interaction profile: %s\n", hand ? "Right" : "Left", name);
			}
		}
	}
	return true;
}

void
XrBackend::Vibrate(int hand, float durationSeconds, float frequency, float amplitude)
{
	if (!haptic_ || state_ != XR_SESSION_STATE_FOCUSED) {
		return;
	}
	XrHapticVibration vibration = {XR_TYPE_HAPTIC_VIBRATION};
	vibration.duration = durationSeconds > 0 ? (XrDuration)(durationSeconds * 1e9) : XR_MIN_HAPTIC_DURATION;
	vibration.frequency = frequency > 0 ? frequency : XR_FREQUENCY_UNSPECIFIED;
	vibration.amplitude = amplitude;
	XrHapticActionInfo info = {XR_TYPE_HAPTIC_ACTION_INFO};
	info.action = haptic_;
	info.subactionPath = handPaths_[hand];
	xrApplyHapticFeedback(session_, &info, reinterpret_cast<XrHapticBaseHeader *>(&vibration));
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

bool
XrBackend::ComputeDisplayDistortion(uint32_t view, float u, float v, XrVector2f out[3])
{
	XrVector2f point = {u, v};
	return hasDisplayDistortion &&
	       XR_SUCCEEDED(computeDistortion_(instance_, system_, view, 1, &point, &out[0], &out[1], &out[2]));
}

XrTime
XrBackend::NowXrTime()
{
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	if (qpcToTime_) {
		XrTime time = 0;
		qpcToTime_(instance_, &counter, &time);
		return time;
	}
	// Estimate: the latest predicted display time, advanced by the counter
	// since xrWaitFrame returned. It runs ahead by the runtime's display lead
	// (a frame or two); timewarp absorbs that.
	LONGLONG anchorCounter = anchorCounter_.load();
	XrTime anchorTime = anchorTime_.load();
	if (!anchorTime) {
		return 0;
	}
	LARGE_INTEGER frequency;
	QueryPerformanceFrequency(&frequency);
	return anchorTime + (XrTime)((counter.QuadPart - anchorCounter) * (1e9 / frequency.QuadPart));
}

void
XrBackend::AnchorTime(XrTime predictedDisplayTime)
{
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	anchorCounter_ = counter.QuadPart;
	anchorTime_ = predictedDisplayTime;
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
	if (!qpcToTime_) {
		AnchorTime(frameState_.predictedDisplayTime);
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
	char label[32];
	snprintf(label, sizeof(label), "layer %zu eye %d", layer, eye);
	return EnsureSwapchain(swapchains_[index], width, height, sourceFormat,
	                       XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT, label);
}

XrBackend::EyeSwapchain *
XrBackend::EnsureSwapchain(EyeSwapchain &swapchain,
                           uint32_t width,
                           uint32_t height,
                           DXGI_FORMAT sourceFormat,
                           XrSwapchainUsageFlags usage,
                           const char *label)
{
	int64_t format = ChooseFormat(sourceFormat);
	if (!format) {
		static bool logged = false;
		if (!logged) {
			Log("No runtime swapchain format can receive a copy of DXGI format %d\n", sourceFormat);
			logged = true;
		}
		return nullptr;
	}
	if (swapchain.width == width && swapchain.height == height && swapchain.format == format) {
		return swapchain.handle ? &swapchain : nullptr; // no handle: creation failed, already logged
	}
	if (swapchain.handle) {
		xrDestroySwapchain(swapchain.handle);
		swapchain = {};
	}
	XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	info.usageFlags = usage;
	info.format = format;
	info.sampleCount = 1;
	info.width = width;
	info.height = height;
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	if (XR_FAILED(xrCreateSwapchain(session_, &info, &swapchain.handle))) {
		Log("xrCreateSwapchain %s %ux%u format %lld usage 0x%llx failed\n", label, width, height,
		    (long long)format, (unsigned long long)usage);
		swapchain = {};
		swapchain.width = width;
		swapchain.height = height;
		swapchain.format = format;
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
	Log("Swapchain %s: %ux%u format %lld, %u images\n", label, width, height, (long long)format, count);
	return &swapchain;
}

bool
XrBackend::CreateSharedSwapchain(uint32_t width, uint32_t height, DXGI_FORMAT format, XrSwapchain &swapchain,
                                 ID3D11Texture2D *textures[3], HANDLE handles[3])
{
	std::lock_guard<std::mutex> lock(mutex_);
	swapchain = XR_NULL_HANDLE;
#ifndef MWXR_HAVE_DXMT_INTEROP
	return false;
#else
	bool formatSupported = false;
	for (int64_t candidate : formats_) {
		formatSupported |= candidate == format; // the application opens the image with this format
	}
	IDXMTNativeDevice3 *native = nullptr;
	if (!formatSupported || !session_ ||
	    FAILED(device->QueryInterface(DXMT_IID_NATIVE_DEVICE3, (void **)&native))) {
		return false;
	}
	XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	info.format = format;
	info.sampleCount = 1;
	info.width = width;
	info.height = height;
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	uint32_t count = 0;
	bool ok = XR_SUCCEEDED(xrCreateSwapchain(session_, &info, &swapchain)) &&
	          XR_SUCCEEDED(xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr)) && count == 3;
	if (ok) {
		XrSwapchainImageD3D11KHR images[3] = {{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR},
		                                      {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR},
		                                      {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR}};
		ok = XR_SUCCEEDED(xrEnumerateSwapchainImages(swapchain, 3, &count,
		                                             reinterpret_cast<XrSwapchainImageBaseHeader *>(images)));
		for (int i = 0; ok && i < 3; ++i) {
			textures[i] = images[i].texture;
			ok = SUCCEEDED(native->CreateSharedTextureHandle(textures[i], &handles[i])) && handles[i];
		}
	}
	native->Release();
	if (!ok) {
		Log("Zero-copy swapchain %ux%u format %d unavailable (%u images); copying instead\n", width, height,
		    format, count);
		if (swapchain) {
			xrDestroySwapchain(swapchain);
			swapchain = XR_NULL_HANDLE;
		}
		return false;
	}
	return true;
#endif
}

void
XrBackend::DestroySwapchain(XrSwapchain swapchain)
{
	std::lock_guard<std::mutex> lock(mutex_);
	xrDestroySwapchain(swapchain);
}

int
XrBackend::AcquireImage(XrSwapchain swapchain)
{
	uint32_t index = 0;
	XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	waitInfo.timeout = XR_INFINITE_DURATION;
	if (XR_FAILED(xrAcquireSwapchainImage(swapchain, nullptr, &index)) ||
	    XR_FAILED(xrWaitSwapchainImage(swapchain, &waitInfo))) {
		return -1;
	}
	return (int)index;
}

void
XrBackend::Present(const std::vector<LayerSubmit> &layers)
{
	if (!frameBegun_) {
		double offset, period;
		WaitAndBeginFrame(offset, period);
	}
	std::lock_guard<std::mutex> lock(mutex_);
	// Zero-copy images: released after the caller's sync, so the runtime's
	// GPU wait follows the application's rendering.
	std::set<XrSwapchain> released;
	for (const auto &layer : layers) {
		for (const auto &eye : layer.eye) {
			if (eye.swapchain && released.insert(eye.swapchain).second) {
				xrReleaseSwapchainImage(eye.swapchain, nullptr);
			}
		}
	}
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
				uint32_t width = submit.box.right - submit.box.left;
				uint32_t height = submit.box.bottom - submit.box.top;
				XrCompositionLayerProjectionView &view = views[i * 2 + eye];
				view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
				view.pose = submit.pose;
				view.fov = submit.fov;
				view.subImage.imageRect.offset = {0, 0};
				view.subImage.imageRect.extent = {(int32_t)width, (int32_t)height};
				if (submit.swapchain) {
					view.subImage.swapchain = submit.swapchain;
					view.subImage.imageRect.offset = {(int32_t)submit.box.left, (int32_t)submit.box.top};
					continue;
				}
				D3D11_TEXTURE2D_DESC desc;
				submit.texture->GetDesc(&desc);
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
				view.subImage.swapchain = swapchain->handle;
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
	LARGE_INTEGER now, frequency;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&frequency);
	if (now.QuadPart - lastLayerLog_ > 5 * frequency.QuadPart) {
		lastLayerLog_ = now.QuadPart;
		Log("Frame: %zu layers submitted, %zu projected, shouldRender %d\n", layers.size(), projections.size(),
		    frameState_.shouldRender);
		for (size_t i = 0; i < layers.size(); ++i) {
			for (int eye = 0; eye < 2; ++eye) {
				const EyeSubmit &e = layers[i].eye[eye];
				const float r2d = 57.29578f;
				Log("  layer %zu eye %d: box %u,%u-%u,%u pos %.3f %.3f %.3f fov L%.1f R%.1f U%.1f D%.1f\n", i,
				    eye, e.box.left, e.box.top, e.box.right, e.box.bottom, e.pose.position.x, e.pose.position.y,
				    e.pose.position.z, e.fov.angleLeft * r2d, e.fov.angleRight * r2d, e.fov.angleUp * r2d,
				    e.fov.angleDown * r2d);
			}
		}
	}
	std::vector<const XrCompositionLayerBaseHeader *> headers;
	for (auto &projection : projections) {
		headers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection));
	}
	EndFrameLocked(headers);
}

void
XrBackend::PresentDisplayImage(ID3D11Texture2D *source)
{
	if (!frameBegun_) {
		double offset, period;
		WaitAndBeginFrame(offset, period);
	}
	std::lock_guard<std::mutex> lock(mutex_);
	if (!running_ || !frameBegun_) {
		return;
	}
	XrCompositionLayerDisplayImageMNDX marker = {XR_TYPE_COMPOSITION_LAYER_DISPLAY_IMAGE_MNDX};
	XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
	                                             {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
	XrCompositionLayerProjection projection = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	std::vector<const XrCompositionLayerBaseHeader *> headers;
	D3D11_TEXTURE2D_DESC desc;
	source->GetDesc(&desc);
	EyeSwapchain *swapchain = nullptr;
	if (frameState_.shouldRender) {
		// SAMPLED and TRANSFER_SRC let the runtime present or copy the image.
		swapchain = EnsureSwapchain(displaySwapchain_, desc.Width, desc.Height, desc.Format,
		                            XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
		                                XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT,
		                            "display image");
	}
	uint32_t image = 0;
	XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	waitInfo.timeout = XR_INFINITE_DURATION;
	if (swapchain && XR_SUCCEEDED(xrAcquireSwapchainImage(swapchain->handle, nullptr, &image)) &&
	    XR_SUCCEEDED(xrWaitSwapchainImage(swapchain->handle, &waitInfo))) {
		// SteamVR's compositor hands the backbuffer over with key 0.
		IDXGIKeyedMutex *mutex = nullptr;
		source->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&mutex);
		bool locked = mutex && mutex->AcquireSync(0, 100) == S_OK;
		if (!mutex || locked) {
			context_->CopyResource(swapchain->images[image].texture, source);
		}
		if (locked) {
			mutex->ReleaseSync(0);
		}
		if (mutex) {
			mutex->Release();
		}
		xrReleaseSwapchainImage(swapchain->handle, nullptr);
		if (!mutex || locked) {
			for (uint32_t i = 0; i < 2; ++i) {
				// The pose and FOV only describe the image: it is presented as it is.
				views[i].pose = {{0, 0, 0, 1}, {0, 0, 0}};
				views[i].fov = display.views[i].fov;
				views[i].subImage.swapchain = swapchain->handle;
				views[i].subImage.imageRect = display.views[i].viewport;
			}
			projection.next = &marker;
			projection.space = viewSpace_;
			projection.viewCount = 2;
			projection.views = views;
			headers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection));
		} else {
			static int failures = 0;
			if (failures++ < 5) {
				Log("Display image: backbuffer keyed mutex not acquired\n");
			}
		}
	}
	EndFrameLocked(headers);
}

void
XrBackend::EndFrameLocked(const std::vector<const XrCompositionLayerBaseHeader *> &headers)
{
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

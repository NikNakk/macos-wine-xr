// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
// OpenXR session used by the SteamVR driver: the headset as any OpenXR runtime
// reports it, and composited frames submitted as projection layers.
#pragma once

#include <d3d11.h>
#include <mutex>
#include <string>
#include <vector>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace mwxr {

// One eye of one SteamVR layer, already resolved to a D3D11 texture.
struct EyeSubmit
{
	ID3D11Texture2D *texture;
	D3D11_BOX box;      // valid region of texture
	XrPosef pose;       // eye pose in the reference space at render time
	XrFovf fov;
};

struct LayerSubmit
{
	EyeSubmit eye[2];
};

// One hand's controller state, from the Touch (or simple) interaction profile.
struct HandState
{
	bool active = false;       // the runtime has a device bound for this hand
	bool poseValid = false, positionValid = false;
	XrPosef pose = {{0, 0, 0, 1}, {0, 0, 0}}; // grip pose in the reference space
	XrVector3f linearVelocity = {}, angularVelocity = {};
	float trigger = 0, squeeze = 0;
	XrVector2f thumbstick = {};
	bool triggerTouch = false, thumbstickClick = false, thumbstickTouch = false;
	bool lowerClick = false, lowerTouch = false; // A (right) / X (left)
	bool upperClick = false, upperTouch = false; // B (right) / Y (left)
	bool menuClick = false, thumbrestTouch = false;
};

class XrBackend
{
public:
	bool
	Init(std::string &error);
	void
	Shutdown();

	// Static display description, valid after Init.
	uint32_t recommendedWidth = 0, recommendedHeight = 0;
	XrFovf fov[2] = {};
	XrPosef eyeInHead[2] = {};
	std::string systemName, runtimeName;
	float playAreaWidth = 0, playAreaDepth = 0; // STAGE bounds, 0 if unavailable
	ID3D11Device *device = nullptr;

	// Handles session state changes; call regularly from one thread.
	void
	PollEvents();
	bool
	IsRunning();

	// Head pose (VIEW space) in the reference space at the current time.
	bool
	LocateHeadNow(XrPosef &pose, XrVector3f &linearVelocity, XrVector3f &angularVelocity, bool &positionValid);

	// Controllers: syncs actions and locates both grip poses now. Returns false
	// while the session is not focused.
	bool
	UpdateHands(HandState hands[2]);
	void
	Vibrate(int hand, float durationSeconds, float frequency, float amplitude);
	std::string currentProfile[2];

	// Frame loop, all on the compositor's present thread.
	// Present: copies and submits; begins a frame first if none is open.
	void
	Present(const std::vector<LayerSubmit> &layers);
	// PostPresent: waits for and begins the next frame. Returns the seconds
	// from now to the vsync before its predicted display time.
	bool
	WaitAndBeginFrame(double &vsyncOffsetSeconds, double &framePeriodSeconds);

private:
	struct EyeSwapchain
	{
		XrSwapchain handle = XR_NULL_HANDLE;
		uint32_t width = 0, height = 0;
		int64_t format = 0;
		std::vector<XrSwapchainImageD3D11KHR> images;
	};

	bool
	BeginSessionLocked();
	bool
	CreateActions(std::string &error);
	EyeSwapchain *
	SwapchainFor(size_t layer, int eye, uint32_t width, uint32_t height, DXGI_FORMAT sourceFormat);
	int64_t
	ChooseFormat(DXGI_FORMAT source);
	XrTime
	NowXrTime();

	std::mutex mutex_; // session lifecycle and frame calls
	XrInstance instance_ = XR_NULL_HANDLE;
	XrSystemId system_ = XR_NULL_SYSTEM_ID;
	XrSession session_ = XR_NULL_HANDLE;
	XrSpace baseSpace_ = XR_NULL_HANDLE, viewSpace_ = XR_NULL_HANDLE;
	XrSessionState state_ = XR_SESSION_STATE_UNKNOWN;
	XrReferenceSpaceType baseSpaceType_ = XR_REFERENCE_SPACE_TYPE_LOCAL;
	LONGLONG lastLayerLog_ = 0;
	bool running_ = false, frameBegun_ = false;
	XrFrameState frameState_ = {XR_TYPE_FRAME_STATE};
	ID3D11DeviceContext *context_ = nullptr;
	std::vector<int64_t> formats_;
	std::vector<EyeSwapchain> swapchains_; // [layer * 2 + eye]

	XrActionSet actionSet_ = XR_NULL_HANDLE;
	XrPath handPaths_[2] = {};
	XrAction gripPose_ = XR_NULL_HANDLE, trigger_ = XR_NULL_HANDLE, triggerTouch_ = XR_NULL_HANDLE,
	         squeeze_ = XR_NULL_HANDLE, thumbstick_ = XR_NULL_HANDLE, thumbstickClick_ = XR_NULL_HANDLE,
	         thumbstickTouch_ = XR_NULL_HANDLE, lowerClick_ = XR_NULL_HANDLE, lowerTouch_ = XR_NULL_HANDLE,
	         upperClick_ = XR_NULL_HANDLE, upperTouch_ = XR_NULL_HANDLE, menuClick_ = XR_NULL_HANDLE,
	         thumbrestTouch_ = XR_NULL_HANDLE, haptic_ = XR_NULL_HANDLE;
	XrSpace gripSpaces_[2] = {};

	PFN_xrConvertWin32PerformanceCounterToTimeKHR qpcToTime_ = nullptr;
	PFN_xrConvertTimeToWin32PerformanceCounterKHR timeToQpc_ = nullptr;
	PFN_xrGetD3D11GraphicsRequirementsKHR getRequirements_ = nullptr;
};

} // namespace mwxr

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
// OpenVR driver interfaces whose methods return structs by value, declared
// with the MSVC calling layout SteamVR uses.
//
// For a member function returning a struct by value, MSVC passes `this`
// first and the hidden result pointer second, and returns that pointer.
// MinGW GCC passes the result pointer first, so SteamVR would hand it `this`
// as the result. Declaring the result pointer as an explicit parameter and
// returning it gives the MSVC layout under either compiler. The slot order
// must match openvr_driver.h exactly; neither interface has a destructor.
#pragma once

#include "third_party/openvr/openvr_driver.h"

namespace mwxr {

// vr::ITrackedDeviceServerDriver (ITrackedDeviceServerDriver_005)
class TrackedDeviceServerDriverAbi
{
public:
	virtual vr::EVRInitError
	Activate(uint32_t objectId) = 0;
	virtual void
	Deactivate() = 0;
	virtual void
	EnterStandby() = 0;
	virtual void *
	GetComponent(const char *name) = 0;
	virtual void
	DebugRequest(const char *request, char *response, uint32_t size) = 0;
	virtual vr::DriverPose_t *
	GetPose(vr::DriverPose_t *result) = 0;
};

// vr::IVRDisplayComponent (IVRDisplayComponent_003)
class DisplayComponentAbi
{
public:
	virtual void
	GetWindowBounds(int32_t *x, int32_t *y, uint32_t *width, uint32_t *height) = 0;
	virtual bool
	IsDisplayOnDesktop() = 0;
	virtual bool
	IsDisplayRealDisplay() = 0;
	virtual void
	GetRecommendedRenderTargetSize(uint32_t *width, uint32_t *height) = 0;
	virtual void
	GetEyeOutputViewport(vr::EVREye eye, uint32_t *x, uint32_t *y, uint32_t *width, uint32_t *height) = 0;
	virtual void
	GetProjectionRaw(vr::EVREye eye, float *left, float *right, float *top, float *bottom) = 0;
	virtual vr::DistortionCoordinates_t *
	ComputeDistortion(vr::DistortionCoordinates_t *result, vr::EVREye eye, float u, float v) = 0;
	virtual bool
	ComputeInverseDistortion(vr::HmdVector2_t *result, vr::EVREye eye, uint32_t channel, float u, float v) = 0;
};

} // namespace mwxr

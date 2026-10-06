// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
// SteamVR driver presenting an OpenXR runtime's headset to SteamVR.
//
// The compositor renders undistorted eye textures through
// IVRDriverDirectModeComponent (the way SteamVR's Oculus driver feeds
// LibOVR). Each Present copies them into OpenXR swapchains and submits them as
// projection layers, so the OpenXR runtime does distortion and timewarp.
// PostPresent runs xrWaitFrame/xrBeginFrame, which paces the compositor.
#include "log.h"
#include "openvr_abi.h"
#include "xr_backend.h"

#include "third_party/openvr/openvr_driver.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <memory>
#include <thread>

namespace mwxr {

void
Log(const char *format, ...)
{
	char buffer[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	if (vr::VRDriverLog()) {
		vr::VRDriverLog()->Log(buffer);
	}
}

static const char *const kSettingsSection = "driver_mwxr";

static vr::HmdQuaternion_t
ToQuat(const XrQuaternionf &q)
{
	return {q.w, q.x, q.y, q.z};
}

static XrVector3f
Rotate(const XrQuaternionf &q, const XrVector3f &v)
{
	// v + 2w(u x v) + 2u x (u x v), u = (x, y, z)
	XrVector3f u = {q.x, q.y, q.z};
	XrVector3f c1 = {u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
	XrVector3f c2 = {u.y * c1.z - u.z * c1.y, u.z * c1.x - u.x * c1.z, u.x * c1.y - u.y * c1.x};
	return {v.x + 2 * (q.w * c1.x + c2.x), v.y + 2 * (q.w * c1.y + c2.y), v.z + 2 * (q.w * c1.z + c2.z)};
}

static XrQuaternionf
Multiply(const XrQuaternionf &a, const XrQuaternionf &b)
{
	return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
	        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

static XrPosef
Compose(const XrPosef &a, const XrPosef &b)
{
	XrVector3f p = Rotate(a.orientation, b.position);
	return {Multiply(a.orientation, b.orientation), {a.position.x + p.x, a.position.y + p.y, a.position.z + p.z}};
}

static vr::HmdMatrix34_t
ToMatrix(const XrPosef &pose)
{
	const XrQuaternionf &q = pose.orientation;
	vr::HmdMatrix34_t m;
	m.m[0][0] = 1 - 2 * (q.y * q.y + q.z * q.z);
	m.m[0][1] = 2 * (q.x * q.y - q.z * q.w);
	m.m[0][2] = 2 * (q.x * q.z + q.y * q.w);
	m.m[1][0] = 2 * (q.x * q.y + q.z * q.w);
	m.m[1][1] = 1 - 2 * (q.x * q.x + q.z * q.z);
	m.m[1][2] = 2 * (q.y * q.z - q.x * q.w);
	m.m[2][0] = 2 * (q.x * q.z - q.y * q.w);
	m.m[2][1] = 2 * (q.y * q.z + q.x * q.w);
	m.m[2][2] = 1 - 2 * (q.x * q.x + q.y * q.y);
	m.m[0][3] = pose.position.x;
	m.m[1][3] = pose.position.y;
	m.m[2][3] = pose.position.z;
	return m;
}

static XrPosef
ToPose(const vr::HmdMatrix34_t &m)
{
	XrPosef pose;
	float trace = m.m[0][0] + m.m[1][1] + m.m[2][2];
	XrQuaternionf &q = pose.orientation;
	if (trace > 0) {
		float s = 0.5f / sqrtf(trace + 1.0f);
		q = {(m.m[2][1] - m.m[1][2]) * s, (m.m[0][2] - m.m[2][0]) * s, (m.m[1][0] - m.m[0][1]) * s, 0.25f / s};
	} else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
		float s = 2.0f * sqrtf(1.0f + m.m[0][0] - m.m[1][1] - m.m[2][2]);
		q = {0.25f * s, (m.m[0][1] + m.m[1][0]) / s, (m.m[0][2] + m.m[2][0]) / s, (m.m[2][1] - m.m[1][2]) / s};
	} else if (m.m[1][1] > m.m[2][2]) {
		float s = 2.0f * sqrtf(1.0f + m.m[1][1] - m.m[0][0] - m.m[2][2]);
		q = {(m.m[0][1] + m.m[1][0]) / s, 0.25f * s, (m.m[1][2] + m.m[2][1]) / s, (m.m[0][2] - m.m[2][0]) / s};
	} else {
		float s = 2.0f * sqrtf(1.0f + m.m[2][2] - m.m[0][0] - m.m[1][1]);
		q = {(m.m[0][2] + m.m[2][0]) / s, (m.m[1][2] + m.m[2][1]) / s, 0.25f * s, (m.m[1][0] - m.m[0][1]) / s};
	}
	float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	q = {q.x / n, q.y / n, q.z / n, q.w / n};
	pose.position = {m.m[0][3], m.m[1][3], m.m[2][3]};
	return pose;
}

// OpenVR projection matrix (left/right/top/bottom tangents, y down) to an
// OpenXR field of view; see ComposeProjection in the OpenVR samples.
static bool
FovFromProjection(const vr::HmdMatrix44_t &p, XrFovf &fov)
{
	if (p.m[0][0] == 0 || p.m[1][1] == 0) {
		return false;
	}
	float left = (p.m[0][2] - 1) / p.m[0][0], right = (p.m[0][2] + 1) / p.m[0][0];
	float top = (p.m[1][2] - 1) / p.m[1][1], bottom = (p.m[1][2] + 1) / p.m[1][1];
	fov = {atanf(left), atanf(right), atanf(-top), atanf(-bottom)};
	return true;
}

// A hand controller presented to SteamVR as an Oculus Touch controller, which
// matches the OpenXR Touch profile the runtime binds (Valve's own Touch input
// profile and bindings then apply to every game).
class ControllerDevice : public TrackedDeviceServerDriverAbi
{
public:
	explicit ControllerDevice(int hand) : hand_(hand) {}

	vr::EVRInitError
	Activate(uint32_t objectId) override
	{
		objectId_ = objectId;
		auto *props = vr::VRProperties();
		auto container = props->TrackedDeviceToPropertyContainer(objectId);
		bool left = hand_ == 0;
		props->SetStringProperty(container, vr::Prop_TrackingSystemName_String, "mwxr");
		props->SetStringProperty(container, vr::Prop_ModelNumber_String,
		                         left ? "mwxr Touch-compatible (Left)" : "mwxr Touch-compatible (Right)");
		props->SetStringProperty(container, vr::Prop_ControllerType_String, "oculus_touch");
		props->SetStringProperty(container, vr::Prop_InputProfilePath_String, "{oculus}/input/touch_profile.json");
		props->SetStringProperty(container, vr::Prop_RenderModelName_String,
		                         left ? "oculus_quest2_controller_left" : "oculus_quest2_controller_right");
		props->SetInt32Property(container, vr::Prop_ControllerRoleHint_Int32,
		                        left ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
		props->SetInt32Property(container, vr::Prop_DeviceClass_Int32, vr::TrackedDeviceClass_Controller);
		props->SetBoolProperty(container, vr::Prop_DeviceProvidesBatteryStatus_Bool, false);

		auto *input = vr::VRDriverInput();
		auto boolean = [&](const char *path) {
			vr::VRInputComponentHandle_t handle = vr::k_ulInvalidInputComponentHandle;
			input->CreateBooleanComponent(container, path, &handle);
			return handle;
		};
		auto scalar = [&](const char *path, vr::EVRScalarUnits units) {
			vr::VRInputComponentHandle_t handle = vr::k_ulInvalidInputComponentHandle;
			input->CreateScalarComponent(container, path, &handle, vr::VRScalarType_Absolute, units);
			return handle;
		};
		stickX_ = scalar("/input/joystick/x", vr::VRScalarUnits_NormalizedTwoSided);
		stickY_ = scalar("/input/joystick/y", vr::VRScalarUnits_NormalizedTwoSided);
		stickClick_ = boolean("/input/joystick/click");
		stickTouch_ = boolean("/input/joystick/touch");
		trigger_ = scalar("/input/trigger/value", vr::VRScalarUnits_NormalizedOneSided);
		triggerTouch_ = boolean("/input/trigger/touch");
		grip_ = scalar("/input/grip/value", vr::VRScalarUnits_NormalizedOneSided);
		gripTouch_ = boolean("/input/grip/touch");
		lowerClick_ = boolean(left ? "/input/x/click" : "/input/a/click");
		lowerTouch_ = boolean(left ? "/input/x/touch" : "/input/a/touch");
		upperClick_ = boolean(left ? "/input/y/click" : "/input/b/click");
		upperTouch_ = boolean(left ? "/input/y/touch" : "/input/b/touch");
		systemClick_ = boolean("/input/system/click");
		thumbrestTouch_ = boolean("/input/thumbrest/touch");
		input->CreateHapticComponent(container, "/output/haptic", &haptic_);
		Log("%s controller active\n", left ? "Left" : "Right");
		return vr::VRInitError_None;
	}

	void
	Deactivate() override
	{
		objectId_ = vr::k_unTrackedDeviceIndexInvalid;
	}
	void
	EnterStandby() override
	{}
	void *
	GetComponent(const char *) override
	{
		return nullptr;
	}
	void
	DebugRequest(const char *, char *response, uint32_t size) override
	{
		if (size) {
			response[0] = 0;
		}
	}
	vr::DriverPose_t *
	GetPose(vr::DriverPose_t *result) override
	{
		*result = lastPose_;
		return result;
	}

	vr::VRInputComponentHandle_t
	HapticHandle() const
	{
		return haptic_;
	}

	void
	Update(const HandState &state)
	{
		uint32_t id = objectId_;
		if (id == vr::k_unTrackedDeviceIndexInvalid) {
			return;
		}
		vr::DriverPose_t pose = {};
		pose.qWorldFromDriverRotation = {1, 0, 0, 0};
		pose.qDriverFromHeadRotation = {1, 0, 0, 0};
		pose.deviceIsConnected = state.active;
		if (state.poseValid) {
			XrQuaternionf inverse = {-state.pose.orientation.x, -state.pose.orientation.y,
			                         -state.pose.orientation.z, state.pose.orientation.w};
			XrVector3f local = Rotate(inverse, state.angularVelocity);
			pose.qRotation = ToQuat(state.pose.orientation);
			pose.vecPosition[0] = state.pose.position.x;
			pose.vecPosition[1] = state.pose.position.y;
			pose.vecPosition[2] = state.pose.position.z;
			pose.vecVelocity[0] = state.linearVelocity.x;
			pose.vecVelocity[1] = state.linearVelocity.y;
			pose.vecVelocity[2] = state.linearVelocity.z;
			pose.vecAngularVelocity[0] = local.x;
			pose.vecAngularVelocity[1] = local.y;
			pose.vecAngularVelocity[2] = local.z;
			pose.poseIsValid = true;
			pose.result = vr::TrackingResult_Running_OK;
		} else {
			pose.qRotation = {1, 0, 0, 0};
			pose.result = vr::TrackingResult_Running_OutOfRange;
		}
		lastPose_ = pose;
		vr::VRServerDriverHost()->TrackedDevicePoseUpdated(id, pose, sizeof(pose));

		auto *input = vr::VRDriverInput();
		input->UpdateScalarComponent(stickX_, state.thumbstick.x, 0);
		input->UpdateScalarComponent(stickY_, state.thumbstick.y, 0);
		input->UpdateBooleanComponent(stickClick_, state.thumbstickClick, 0);
		input->UpdateBooleanComponent(stickTouch_, state.thumbstickTouch || state.thumbstickClick, 0);
		input->UpdateScalarComponent(trigger_, state.trigger, 0);
		input->UpdateBooleanComponent(triggerTouch_, state.triggerTouch || state.trigger > 0.05f, 0);
		input->UpdateScalarComponent(grip_, state.squeeze, 0);
		input->UpdateBooleanComponent(gripTouch_, state.squeeze > 0.05f, 0);
		input->UpdateBooleanComponent(lowerClick_, state.lowerClick, 0);
		input->UpdateBooleanComponent(lowerTouch_, state.lowerTouch || state.lowerClick, 0);
		input->UpdateBooleanComponent(upperClick_, state.upperClick, 0);
		input->UpdateBooleanComponent(upperTouch_, state.upperTouch || state.upperClick, 0);
		input->UpdateBooleanComponent(systemClick_, state.menuClick, 0);
		input->UpdateBooleanComponent(thumbrestTouch_, state.thumbrestTouch, 0);
	}

private:
	int hand_;
	std::atomic<uint32_t> objectId_ = vr::k_unTrackedDeviceIndexInvalid;
	vr::DriverPose_t lastPose_ = {};
	vr::VRInputComponentHandle_t stickX_ = 0, stickY_ = 0, stickClick_ = 0, stickTouch_ = 0, trigger_ = 0,
	                             triggerTouch_ = 0, grip_ = 0, gripTouch_ = 0, lowerClick_ = 0, lowerTouch_ = 0,
	                             upperClick_ = 0, upperTouch_ = 0, systemClick_ = 0, thumbrestTouch_ = 0,
	                             haptic_ = vr::k_ulInvalidInputComponentHandle;
};

class HmdDevice : public TrackedDeviceServerDriverAbi,
                  public DisplayComponentAbi,
                  public vr::IVRDriverDirectModeComponent
{
public:
	HmdDevice(XrBackend &xr, ControllerDevice *left, ControllerDevice *right) : xr_(xr), controllers_{left, right} {}

	// ITrackedDeviceServerDriver
	vr::EVRInitError
	Activate(uint32_t objectId) override
	{
		objectId_ = objectId;
		auto container = vr::VRProperties()->TrackedDeviceToPropertyContainer(objectId);
		auto *props = vr::VRProperties();
		frequency_ = vr::VRSettings()->GetFloat(kSettingsSection, "displayFrequency");
		if (frequency_ <= 0) {
			frequency_ = 90.0f;
		}
		XrVector3f l = xr_.eyeInHead[0].position, r = xr_.eyeInHead[1].position;
		float ipd = sqrtf((r.x - l.x) * (r.x - l.x) + (r.y - l.y) * (r.y - l.y) + (r.z - l.z) * (r.z - l.z));
		props->SetStringProperty(container, vr::Prop_TrackingSystemName_String, "mwxr");
		props->SetStringProperty(container, vr::Prop_ModelNumber_String, xr_.systemName.c_str());
		props->SetStringProperty(container, vr::Prop_ManufacturerName_String, xr_.runtimeName.c_str());
		props->SetStringProperty(container, vr::Prop_RenderModelName_String, "generic_hmd");
		props->SetFloatProperty(container, vr::Prop_UserIpdMeters_Float, ipd);
		props->SetFloatProperty(container, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.0f);
		props->SetFloatProperty(container, vr::Prop_DisplayFrequency_Float, frequency_);
		props->SetFloatProperty(container, vr::Prop_SecondsFromVsyncToPhotons_Float, 1.0f / frequency_);
		props->SetBoolProperty(container, vr::Prop_IsOnDesktop_Bool, false);
		props->SetBoolProperty(container, vr::Prop_DriverDirectModeSendsVsyncEvents_Bool, true);
		props->SetBoolProperty(container, vr::Prop_ContainsProximitySensor_Bool, false);
		props->SetBoolProperty(container, vr::Prop_DeviceProvidesBatteryStatus_Bool, false);
		props->SetBoolProperty(container, vr::Prop_HasCamera_Bool, false);
		props->SetBoolProperty(container, vr::Prop_DisplayDebugMode_Bool, false);
		SetPlayArea(container);
		vr::VRServerDriverHost()->SetDisplayEyeToHead(objectId, ToMatrix(xr_.eyeInHead[0]),
		                                             ToMatrix(xr_.eyeInHead[1]));
		Log("HMD '%s' active: IPD %.1f mm, %.0f Hz\n", xr_.systemName.c_str(), ipd * 1000, frequency_);

		poseRateHz_ = vr::VRSettings()->GetInt32(kSettingsSection, "poseRateHz");
		if (poseRateHz_ <= 0) {
			poseRateHz_ = 500;
		}
		running_ = true;
		poseThread_ = std::thread([this] { PoseLoop(); });
		return vr::VRInitError_None;
	}

	void
	Deactivate() override
	{
		running_ = false;
		if (poseThread_.joinable()) {
			poseThread_.join();
		}
		objectId_ = vr::k_unTrackedDeviceIndexInvalid;
	}

	void
	EnterStandby() override
	{}

	void *
	GetComponent(const char *name) override
	{
		if (!strcmp(name, vr::IVRDisplayComponent_Version)) {
			return static_cast<DisplayComponentAbi *>(this);
		}
		if (!strcmp(name, vr::IVRDriverDirectModeComponent_Version)) {
			return static_cast<vr::IVRDriverDirectModeComponent *>(this);
		}
		return nullptr;
	}

	void
	DebugRequest(const char *, char *response, uint32_t size) override
	{
		if (size) {
			response[0] = 0;
		}
	}

	vr::DriverPose_t *
	GetPose(vr::DriverPose_t *result) override
	{
		*result = lastPose_;
		return result;
	}

	// IVRDisplayComponent: the runtime distorts, so SteamVR's distortion is identity.
	void
	GetWindowBounds(int32_t *x, int32_t *y, uint32_t *width, uint32_t *height) override
	{
		*x = *y = 0;
		*width = xr_.recommendedWidth * 2;
		*height = xr_.recommendedHeight;
	}
	bool
	IsDisplayOnDesktop() override
	{
		return false;
	}
	bool
	IsDisplayRealDisplay() override
	{
		return false;
	}
	void
	GetRecommendedRenderTargetSize(uint32_t *width, uint32_t *height) override
	{
		*width = xr_.recommendedWidth;
		*height = xr_.recommendedHeight;
	}
	void
	GetEyeOutputViewport(vr::EVREye eye, uint32_t *x, uint32_t *y, uint32_t *width, uint32_t *height) override
	{
		*x = eye == vr::Eye_Left ? 0 : xr_.recommendedWidth;
		*y = 0;
		*width = xr_.recommendedWidth;
		*height = xr_.recommendedHeight;
	}
	void
	GetProjectionRaw(vr::EVREye eye, float *left, float *right, float *top, float *bottom) override
	{
		const XrFovf &fov = xr_.fov[eye];
		*left = tanf(fov.angleLeft);
		*right = tanf(fov.angleRight);
		*top = tanf(-fov.angleUp);
		*bottom = tanf(-fov.angleDown);
	}
	vr::DistortionCoordinates_t *
	ComputeDistortion(vr::DistortionCoordinates_t *result, vr::EVREye, float u, float v) override
	{
		*result = {{u, v}, {u, v}, {u, v}};
		return result;
	}
	bool
	ComputeInverseDistortion(vr::HmdVector2_t *, vr::EVREye, uint32_t, float, float) override
	{
		// false: SteamVR inverts ComputeDistortion numerically (as Monado's
		// own driver does); returning an identity result fails its check.
		return false;
	}

	// IVRDriverDirectModeComponent
	void
	CreateSwapTextureSet(uint32_t pid, const SwapTextureSetDesc_t *desc, SwapTextureSet_t *out) override
	{
		std::lock_guard<std::mutex> lock(texturesMutex_);
		auto set = std::make_shared<TextureSet>();
		set->pid = pid;
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = desc->nWidth;
		td.Height = desc->nHeight;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = (DXGI_FORMAT)desc->nFormat;
		td.SampleDesc.Count = desc->nSampleCount ? desc->nSampleCount : 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
		out->unTextureFlags = 0;
		for (int i = 0; i < 3; ++i) {
			IDXGIResource *resource = nullptr;
			HANDLE handle = nullptr;
			if (FAILED(xr_.device->CreateTexture2D(&td, nullptr, &set->textures[i])) ||
			    FAILED(set->textures[i]->QueryInterface(__uuidof(IDXGIResource), (void **)&resource)) ||
			    FAILED(resource->GetSharedHandle(&handle))) {
				Log("CreateSwapTextureSet %ux%u format %u failed\n", td.Width, td.Height, desc->nFormat);
			}
			if (resource) {
				resource->Release();
			}
			set->handles[i] = (vr::SharedTextureHandle_t)handle;
			out->rSharedTextureHandles[i] = set->handles[i];
			if (handle) {
				textures_[set->handles[i]] = {set, i};
			}
		}
		if (td.SampleDesc.Count > 1) {
			Log("Swap texture set has %u samples; multisampled copies are not supported\n",
			    td.SampleDesc.Count);
		}
		Log("Swap texture set for pid %u: %ux%u format %u\n", pid, td.Width, td.Height, desc->nFormat);
	}

	void
	DestroySwapTextureSet(vr::SharedTextureHandle_t handle) override
	{
		std::lock_guard<std::mutex> lock(texturesMutex_);
		auto it = textures_.find(handle);
		if (it != textures_.end()) {
			EraseSetLocked(it->second.set);
		}
	}

	void
	DestroyAllSwapTextureSets(uint32_t pid) override
	{
		std::lock_guard<std::mutex> lock(texturesMutex_);
		std::vector<std::shared_ptr<TextureSet>> sets;
		for (auto &entry : textures_) {
			if (entry.second.set->pid == pid) {
				sets.push_back(entry.second.set);
			}
		}
		for (auto &set : sets) {
			EraseSetLocked(set);
		}
	}

	void
	GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t handles[2], uint32_t (*indices)[2]) override
	{
		std::lock_guard<std::mutex> lock(texturesMutex_);
		TextureSet *advanced = nullptr;
		for (int eye = 0; eye < 2; ++eye) {
			auto it = textures_.find(handles[eye]);
			if (it == textures_.end()) {
				(*indices)[eye] = 0;
				continue;
			}
			TextureSet *set = it->second.set.get();
			if (set != advanced) { // both eyes may share one set
				set->next = (set->next + 1) % 3;
				advanced = set;
			}
			(*indices)[eye] = set->next;
		}
	}

	void
	SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) override
	{
		std::lock_guard<std::mutex> lock(texturesMutex_);
		LayerSubmit layer = {};
		for (int eye = 0; eye < 2; ++eye) {
			const SubmitLayerPerEye_t &in = perEye[eye];
			auto it = textures_.find(in.hTexture);
			if (it == textures_.end()) {
				return;
			}
			ID3D11Texture2D *texture = it->second.set->textures[it->second.index];
			D3D11_TEXTURE2D_DESC desc;
			texture->GetDesc(&desc);
			float u0 = std::min(in.bounds.uMin, in.bounds.uMax), u1 = std::max(in.bounds.uMin, in.bounds.uMax);
			float v0 = std::min(in.bounds.vMin, in.bounds.vMax), v1 = std::max(in.bounds.vMin, in.bounds.vMax);
			EyeSubmit &out = layer.eye[eye];
			out.texture = texture;
			out.box = {(UINT)lroundf(u0 * desc.Width), (UINT)lroundf(v0 * desc.Height), 0,
			           (UINT)lroundf(u1 * desc.Width), (UINT)lroundf(v1 * desc.Height), 1};
			out.pose = Compose(ToPose(in.mHmdPose), xr_.eyeInHead[eye]);
			if (!FovFromProjection(in.mProjection, out.fov)) {
				out.fov = xr_.fov[eye];
			}
		}
		pending_.push_back(layer);
		if (pending_.size() == 1) {
			lastPrediction_ = perEye[0].flHmdPosePredictionTimeInSecondsFromNow;
			lastRenderHead_ = ToPose(perEye[0].mHmdPose).orientation;
		}
	}

	void
	Present(vr::SharedTextureHandle_t syncTexture) override
	{
		std::vector<LayerSubmit> layers;
		{
			std::lock_guard<std::mutex> lock(texturesMutex_);
			layers.swap(pending_);
		}
		IDXGIKeyedMutex *mutex = SyncMutex(syncTexture);
		bool locked = mutex && mutex->AcquireSync(0, 100) == S_OK;
		if (mutex && !locked) {
			static int failures = 0;
			if (failures++ < 5) {
				Log("Sync texture AcquireSync failed; submitting no layers this frame\n");
			}
			layers.clear();
		}
		auto now = std::chrono::steady_clock::now();
		if (!layers.empty() && now - lastPresentLog_ > std::chrono::seconds(5)) {
			lastPresentLog_ = now;
			XrPosef head;
			XrVector3f linear, angular;
			bool positionValid;
			if (xr_.LocateHeadNow(head, linear, angular, positionValid)) {
				const XrQuaternionf &a = lastRenderHead_, &b = head.orientation;
				float dot = fabsf(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
				float degrees = 2.0f * acosf(std::min(dot, 1.0f)) * 57.29578f;
				Log("Present: SteamVR predicted %.1f ms ahead; render vs current head %.2f deg; angular "
				    "velocity %.2f %.2f %.2f rad/s\n",
				    lastPrediction_ * 1000, degrees, angular.x, angular.y, angular.z);
			}
		}
		xr_.Present(layers);
		if (locked) {
			mutex->ReleaseSync(0);
		}
	}

	void
	PostPresent(const Throttling_t *) override
	{
		double vsyncOffset = 0, period = 0;
		if (!xr_.WaitAndBeginFrame(vsyncOffset, period)) {
			return;
		}
		// predictedDisplayPeriod becomes a multiple of the refresh when frames
		// are late, so only ever adopt a shorter period than seen so far.
		if (period > 0 && (minPeriod_ == 0 || period < minPeriod_ * 0.95)) {
			minPeriod_ = period;
		} else {
			period = 0;
		}
		if (period > 0 && fabs(1.0 / period - frequency_) > 1.0 &&
		    objectId_ != vr::k_unTrackedDeviceIndexInvalid) {
			frequency_ = (float)(1.0 / period);
			auto container = vr::VRProperties()->TrackedDeviceToPropertyContainer(objectId_);
			vr::VRProperties()->SetFloatProperty(container, vr::Prop_DisplayFrequency_Float, frequency_);
			vr::VRProperties()->SetFloatProperty(container, vr::Prop_SecondsFromVsyncToPhotons_Float,
			                                      (float)period);
			Log("Runtime frame period %.3f ms: display frequency now %.2f Hz\n", period * 1000, frequency_);
		}
		vr::VRServerDriverHost()->VsyncEvent(vsyncOffset);
	}

private:
	// A standing universe at the reference space origin, so SteamVR does not
	// require room setup. The runtime's STAGE is already floor-level.
	void
	SetPlayArea(vr::PropertyContainerHandle_t container)
	{
		float width = xr_.playAreaWidth > 0 ? xr_.playAreaWidth : 2.0f;
		float depth = xr_.playAreaDepth > 0 ? xr_.playAreaDepth : 2.0f;
		float x = width / 2, z = depth / 2, h = 2.43f;
		const uint64_t universe = 0x6d77787200000001ull; // "mwxr", 1
		char json[2048];
		snprintf(json, sizeof(json),
		         "{\"jsonid\":\"chaperone_info\",\"version\":5,\"universes\":[{\"universeID\":\"%llu\","
		         "\"play_area\":[%.3f,%.3f],"
		         "\"collision_bounds\":["
		         "[[%.3f,0,%.3f],[%.3f,%.2f,%.3f],[%.3f,%.2f,%.3f],[%.3f,0,%.3f]],"
		         "[[%.3f,0,%.3f],[%.3f,%.2f,%.3f],[%.3f,%.2f,%.3f],[%.3f,0,%.3f]],"
		         "[[%.3f,0,%.3f],[%.3f,%.2f,%.3f],[%.3f,%.2f,%.3f],[%.3f,0,%.3f]],"
		         "[[%.3f,0,%.3f],[%.3f,%.2f,%.3f],[%.3f,%.2f,%.3f],[%.3f,0,%.3f]]],"
		         "\"standing\":{\"translation\":[0,0,0],\"yaw\":0},"
		         "\"seated\":{\"translation\":[0,0,0],\"yaw\":0}}]}",
		         (unsigned long long)universe, width, depth, -x, -z, -x, h, -z, x, h, -z, x, -z, x, -z, x, h, -z, x,
		         h, z, x, z, x, z, x, h, z, -x, h, z, -x, z, -x, z, -x, h, z, -x, h, -z, -x, -z);
		auto *props = vr::VRProperties();
		props->SetUint64Property(container, vr::Prop_CurrentUniverseId_Uint64, universe);
		props->SetStringProperty(container, vr::Prop_DriverProvidedChaperoneJson_String, json);
		props->SetBoolProperty(container, vr::Prop_DriverProvidedChaperoneVisibility_Bool, true);
		Log("Play area %.2f x %.2f m (%s)\n", width, depth, xr_.playAreaWidth > 0 ? "runtime" : "default");
	}

	struct TextureSet
	{
		uint32_t pid = 0;
		ID3D11Texture2D *textures[3] = {};
		vr::SharedTextureHandle_t handles[3] = {};
		uint32_t next = 0;
		~TextureSet()
		{
			for (auto *texture : textures) {
				if (texture) {
					texture->Release();
				}
			}
		}
	};
	struct TextureRef
	{
		std::shared_ptr<TextureSet> set;
		int index;
	};

	void
	EraseSetLocked(std::shared_ptr<TextureSet> set)
	{
		for (auto handle : set->handles) {
			textures_.erase(handle);
		}
	}

	IDXGIKeyedMutex *
	SyncMutex(vr::SharedTextureHandle_t handle)
	{
		if (!handle) {
			return nullptr;
		}
		if (handle != syncHandle_) {
			if (syncMutex_) {
				syncMutex_->Release();
				syncMutex_ = nullptr;
			}
			ID3D11Texture2D *texture = nullptr;
			if (SUCCEEDED(xr_.device->OpenSharedResource((HANDLE)handle, __uuidof(ID3D11Texture2D),
			                                             (void **)&texture))) {
				texture->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&syncMutex_);
				texture->Release();
			} else {
				Log("OpenSharedResource on sync texture %p failed\n", (void *)handle);
			}
			syncHandle_ = handle;
		}
		return syncMutex_;
	}

	void
	PoseLoop()
	{
		auto interval = std::chrono::microseconds(1000000 / poseRateHz_);
		while (running_) {
			auto next = std::chrono::steady_clock::now() + interval;
			UpdatePose();
			HandState hands[2];
			if (xr_.UpdateHands(hands)) {
				controllers_[0]->Update(hands[0]);
				controllers_[1]->Update(hands[1]);
				auto now = std::chrono::steady_clock::now();
				if (now - lastInputLog_ > std::chrono::seconds(5)) {
					lastInputLog_ = now;
					for (int hand = 0; hand < 2; ++hand) {
						const HandState &h = hands[hand];
						Log("%s hand: active %d pose %d trigger %.2f squeeze %.2f stick %.2f %.2f lower %d "
						    "upper %d menu %d\n",
						    hand ? "Right" : "Left", h.active, h.poseValid, h.trigger, h.squeeze,
						    h.thumbstick.x, h.thumbstick.y, h.lowerClick, h.upperClick, h.menuClick);
					}
				}
			}
			std::this_thread::sleep_until(next);
		}
	}

	void
	UpdatePose()
	{
		XrPosef pose;
		XrVector3f linear, angular;
		bool positionValid = false;
		vr::DriverPose_t out = {};
		out.qWorldFromDriverRotation = {1, 0, 0, 0};
		out.qDriverFromHeadRotation = {1, 0, 0, 0};
		out.deviceIsConnected = true;
		if (xr_.LocateHeadNow(pose, linear, angular, positionValid)) {
			// OpenVR expects angular velocity in the device's local frame.
			XrQuaternionf inverse = {-pose.orientation.x, -pose.orientation.y, -pose.orientation.z,
			                         pose.orientation.w};
			XrVector3f local = Rotate(inverse, angular);
			out.qRotation = ToQuat(pose.orientation);
			out.vecPosition[0] = pose.position.x;
			out.vecPosition[1] = pose.position.y;
			out.vecPosition[2] = pose.position.z;
			out.vecVelocity[0] = linear.x;
			out.vecVelocity[1] = linear.y;
			out.vecVelocity[2] = linear.z;
			out.vecAngularVelocity[0] = local.x;
			out.vecAngularVelocity[1] = local.y;
			out.vecAngularVelocity[2] = local.z;
			out.poseIsValid = true;
			out.result = vr::TrackingResult_Running_OK;
			out.willDriftInYaw = !positionValid;
			out.shouldApplyHeadModel = !positionValid;
		} else {
			out.qRotation = {1, 0, 0, 0};
			out.poseIsValid = false;
			out.result = vr::TrackingResult_Running_OutOfRange;
		}
		lastPose_ = out;
		auto now = std::chrono::steady_clock::now();
		if (now - lastPoseLog_ > std::chrono::seconds(5)) {
			lastPoseLog_ = now;
			Log("Head: valid %d position %.3f %.3f %.3f orientation %.3f %.3f %.3f %.3f\n", out.poseIsValid,
			    out.vecPosition[0], out.vecPosition[1], out.vecPosition[2], out.qRotation.w, out.qRotation.x,
			    out.qRotation.y, out.qRotation.z);
		}
		if (objectId_ != vr::k_unTrackedDeviceIndexInvalid) {
			vr::VRServerDriverHost()->TrackedDevicePoseUpdated(objectId_, out, sizeof(out));
		}
	}

	XrBackend &xr_;
	ControllerDevice *controllers_[2];
	std::atomic<uint32_t> objectId_ = vr::k_unTrackedDeviceIndexInvalid;
	float frequency_ = 90.0f;
	double minPeriod_ = 0;
	int poseRateHz_ = 500;
	std::atomic<bool> running_ = false;
	std::thread poseThread_;
	vr::DriverPose_t lastPose_ = {};
	std::chrono::steady_clock::time_point lastPoseLog_, lastInputLog_;

	std::mutex texturesMutex_;
	std::map<vr::SharedTextureHandle_t, TextureRef> textures_;
	std::vector<LayerSubmit> pending_;

	float lastPrediction_ = 0;
	XrQuaternionf lastRenderHead_ = {0, 0, 0, 1};
	std::chrono::steady_clock::time_point lastPresentLog_;

	vr::SharedTextureHandle_t syncHandle_ = 0;
	IDXGIKeyedMutex *syncMutex_ = nullptr;
};

class ServerProvider : public vr::IServerTrackedDeviceProvider
{
public:
	vr::EVRInitError
	Init(vr::IVRDriverContext *context) override
	{
		VR_INIT_SERVER_DRIVER_CONTEXT(context);
		if (!vr::VRSettings()->GetBool(kSettingsSection, "enable")) {
			return vr::VRInitError_Driver_NotLoaded;
		}
		std::string error;
		if (!xr_.Init(error)) {
			Log("OpenXR initialisation failed: %s\n", error.c_str());
			xr_.Shutdown();
			return vr::VRInitError_Driver_Failed;
		}
		left_ = new ControllerDevice(0);
		right_ = new ControllerDevice(1);
		hmd_ = new HmdDevice(xr_, left_, right_);
		auto *driver = reinterpret_cast<vr::ITrackedDeviceServerDriver *>(
		    static_cast<TrackedDeviceServerDriverAbi *>(hmd_));
		if (!vr::VRServerDriverHost()->TrackedDeviceAdded("mwxr-hmd", vr::TrackedDeviceClass_HMD, driver)) {
			Log("TrackedDeviceAdded failed\n");
			return vr::VRInitError_Driver_Failed;
		}
		const char *serials[2] = {"mwxr-left", "mwxr-right"};
		ControllerDevice *controllers[2] = {left_, right_};
		for (int hand = 0; hand < 2; ++hand) {
			auto *controller = reinterpret_cast<vr::ITrackedDeviceServerDriver *>(
			    static_cast<TrackedDeviceServerDriverAbi *>(controllers[hand]));
			vr::VRServerDriverHost()->TrackedDeviceAdded(serials[hand], vr::TrackedDeviceClass_Controller,
			                                             controller);
		}
		return vr::VRInitError_None;
	}

	void
	Cleanup() override
	{
		delete hmd_;
		delete left_;
		delete right_;
		hmd_ = nullptr;
		left_ = right_ = nullptr;
		xr_.Shutdown();
		VR_CLEANUP_SERVER_DRIVER_CONTEXT();
	}

	const char *const *
	GetInterfaceVersions() override
	{
		return vr::k_InterfaceVersions;
	}

	void
	RunFrame() override
	{
		xr_.PollEvents();
		vr::VREvent_t event;
		while (vr::VRServerDriverHost()->PollNextEvent(&event, sizeof(event))) {
			if (event.eventType != vr::VREvent_Input_HapticVibration) {
				continue;
			}
			const auto &haptic = event.data.hapticVibration;
			for (int hand = 0; hand < 2; ++hand) {
				ControllerDevice *controller = hand ? right_ : left_;
				if (controller && haptic.componentHandle == controller->HapticHandle()) {
					xr_.Vibrate(hand, haptic.fDurationSeconds, haptic.fFrequency, haptic.fAmplitude);
				}
			}
		}
	}

	bool
	ShouldBlockStandbyMode() override
	{
		return false;
	}
	void
	EnterStandby() override
	{}
	void
	LeaveStandby() override
	{}

private:
	XrBackend xr_;
	HmdDevice *hmd_ = nullptr;
	ControllerDevice *left_ = nullptr, *right_ = nullptr;
};

static ServerProvider g_provider;

} // namespace mwxr

extern "C" __declspec(dllexport) void *
HmdDriverFactory(const char *interfaceName, int *returnCode)
{
	if (!strcmp(interfaceName, vr::IServerTrackedDeviceProvider_Version)) {
		return &mwxr::g_provider;
	}
	if (returnCode) {
		*returnCode = vr::VRInitError_Init_InterfaceNotFound;
	}
	return nullptr;
}

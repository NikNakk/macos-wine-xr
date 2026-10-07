// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
// SteamVR driver presenting an OpenXR runtime's headset to SteamVR.
//
// The compositor renders undistorted eye textures through
// IVRDriverDirectModeComponent (the way SteamVR's Oculus driver feeds
// LibOVR). Each Present copies them into OpenXR swapchains and submits them as
// projection layers, so the OpenXR runtime does distortion and timewarp.
// PostPresent runs xrWaitFrame/xrBeginFrame, which paces the compositor.
#include "hand_skeleton.h"
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

static XrPosef
Inverse(const XrPosef &a)
{
	XrQuaternionf q = {-a.orientation.x, -a.orientation.y, -a.orientation.z, a.orientation.w};
	XrVector3f p = Rotate(q, a.position);
	return {q, {-p.x, -p.y, -p.z}};
}

// Recentring: SteamVR's world from the runtime's space, applied to every device
// pose through DriverPose_t's world-from-driver transform. Set by holding the
// right controller's Options button (as on a PS5), and identity until then.
static std::mutex g_recentreMutex;
static XrPosef g_worldFromDriver = {{0, 0, 0, 1}, {0, 0, 0}};

static XrPosef
WorldFromDriver()
{
	std::lock_guard<std::mutex> lock(g_recentreMutex);
	return g_worldFromDriver;
}

static void
ApplyWorldFromDriver(vr::DriverPose_t &pose)
{
	XrPosef world = WorldFromDriver();
	pose.qWorldFromDriverRotation = ToQuat(world.orientation);
	pose.vecWorldFromDriverTranslation[0] = world.position.x;
	pose.vecWorldFromDriverTranslation[1] = world.position.y;
	pose.vecWorldFromDriverTranslation[2] = world.position.z;
}

// Make the head's current horizontal position the origin and its heading
// forward (-Z), keeping height and the floor.
static void
RecentreOn(const XrPosef &head)
{
	XrVector3f forward = Rotate(head.orientation, {0, 0, -1});
	float yaw = atan2f(-forward.x, -forward.z); // 0 when facing -Z
	XrQuaternionf rotation = {0, sinf(-yaw / 2), 0, cosf(-yaw / 2)};
	XrVector3f position = Rotate(rotation, {head.position.x, 0, head.position.z});
	std::lock_guard<std::mutex> lock(g_recentreMutex);
	g_worldFromDriver = {rotation, {-position.x, 0, -position.z}};
	Log("Recentred: heading %.1f deg, position %.2f %.2f\n", yaw * 57.29578f, head.position.x, head.position.z);
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
		// Home draws its hands and pointer from the skeleton; without one its
		// bindings fail ("invalid device path for skeleton output").
		vr::EVRInputError skeletonError = input->CreateSkeletonComponent(
		    container, left ? "/input/skeleton/left" : "/input/skeleton/right",
		    left ? "/skeleton/hand/left" : "/skeleton/hand/right", "/pose/raw", vr::VRSkeletalTracking_Estimated,
		    nullptr, 0, &skeleton_);
		vr::EVRSettingsError settingError = vr::VRSettingsError_None;
		fingerCurl_ = vr::VRSettings()->GetBool(kSettingsSection, "fingerCurl", &settingError);
		if (settingError != vr::VRSettingsError_None) {
			fingerCurl_ = true;
		}
		Log("%s hand skeleton: finger curl %s\n", left ? "Left" : "Right", fingerCurl_ ? "on" : "off");
		if (skeletonError != vr::VRInputError_None) {
			skeleton_ = vr::k_ulInvalidInputComponentHandle;
			Log("%s hand skeleton: error %d\n", left ? "Left" : "Right", skeletonError);
		}
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
		ApplyWorldFromDriver(pose);
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
		if (skeleton_ != vr::k_ulInvalidInputComponentHandle) {
			// Curl as Touch controllers estimate it: the index finger from the
			// trigger (slightly bent when only touching it), the other fingers
			// from the grip, and the thumb when it rests on a control.
			// driver_mwxr.fingerCurl false sends the open hand unchanged.
			HandCurl curl = {};
			if (fingerCurl_) {
				curl.index = std::max(state.trigger, state.triggerTouch ? 0.25f : 0.0f);
				curl.middle = curl.ring = curl.pinky = state.squeeze;
				bool thumbDown = state.thumbstickTouch || state.thumbstickClick || state.lowerTouch ||
				                 state.lowerClick || state.upperTouch || state.upperClick ||
				                 state.thumbrestTouch;
				curl.thumb = thumbDown ? 0.5f : 0.0f;
			}
			vr::VRBoneTransform_t bones[kHandBoneCount];
			CurlHand(hand_ == 0 ? kLeftOpenHand : kRightOpenHand, curl, bones);
			vr::EVRInputError with = input->UpdateSkeletonComponent(
			    skeleton_, vr::VRSkeletalMotionRange_WithController, bones, kHandBoneCount);
			vr::EVRInputError without = input->UpdateSkeletonComponent(
			    skeleton_, vr::VRSkeletalMotionRange_WithoutController, bones, kHandBoneCount);
			if ((with != vr::VRInputError_None || without != vr::VRInputError_None) && !loggedSkeletonError_) {
				Log("%s hand skeleton update rejected: %d %d\n", hand_ == 0 ? "Left" : "Right", with, without);
				loggedSkeletonError_ = true;
			}
			if (++skeletonUpdates_ % 2500 == 0) { // about every 5 s at the pose rate
				Log("%s hand skeleton: %llu updates, curl thumb %.2f index %.2f grip %.2f\n",
				    hand_ == 0 ? "Left" : "Right", (unsigned long long)skeletonUpdates_, curl.thumb, curl.index,
				    curl.middle);
			}
		}
	}

private:
	int hand_;
	std::atomic<uint32_t> objectId_ = vr::k_unTrackedDeviceIndexInvalid;
	vr::DriverPose_t lastPose_ = {};
	vr::VRInputComponentHandle_t stickX_ = 0, stickY_ = 0, stickClick_ = 0, stickTouch_ = 0, trigger_ = 0,
	                             triggerTouch_ = 0, grip_ = 0, gripTouch_ = 0, lowerClick_ = 0, lowerTouch_ = 0,
	                             upperClick_ = 0, upperTouch_ = 0, systemClick_ = 0, thumbrestTouch_ = 0,
	                             haptic_ = vr::k_ulInvalidInputComponentHandle,
	                             skeleton_ = vr::k_ulInvalidInputComponentHandle;
	bool loggedSkeletonError_ = false;
	bool fingerCurl_ = true;
	uint64_t skeletonUpdates_ = 0;
};

// SteamVR's compositor always composites into resolve textures of its own and
// submits only those; applications' texture sets are read by the compositor
// and never reach the driver's layers.
static bool
IsCompositorProcess(uint32_t pid)
{
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!process) {
		return false;
	}
	char path[MAX_PATH];
	DWORD size = sizeof(path);
	bool compositor = false;
	if (QueryFullProcessImageNameA(process, 0, path, &size)) {
		const char *name = strrchr(path, '\\');
		compositor = !_stricmp(name ? name + 1 : path, "vrcompositor.exe");
	}
	CloseHandle(process);
	return compositor;
}

class HmdDevice : public TrackedDeviceServerDriverAbi,
                  public DisplayComponentAbi,
                  public vr::IVRDriverDirectModeComponent,
                  public vr::IVRVirtualDisplay
{
public:
	HmdDevice(XrBackend &xr, ControllerDevice *left, ControllerDevice *right) : xr_(xr), controllers_{left, right}
	{
		// "virtual": SteamVR's compositor distorts with the runtime's distortion
		// and presents through IVRVirtualDisplay (XR_MNDX_display_distortion).
		char mode[32] = {};
		vr::VRSettings()->GetString(kSettingsSection, "displayMode", mode, sizeof(mode));
		virtualDisplay_ = !strcmp(mode, "virtual") && xr_.hasDisplayDistortion;
		vr::VRSettings()->GetString(kSettingsSection, "virtualDisplayDumpDir", dumpDir_, sizeof(dumpDir_));
		dumpEvery_ = vr::VRSettings()->GetInt32(kSettingsSection, "virtualDisplayDumpEvery");
		Log("Display mode: %s%s\n", virtualDisplay_ ? "virtual display" : "direct",
		    !strcmp(mode, "virtual") && !virtualDisplay_ ? " (virtual requested, runtime lacks display distortion)"
		                                                : "");
		if (virtualDisplay_) {
			// What SteamVR will be given: the display layout and a few points of
			// each eye's distortion (green channel), to check against the panel.
			for (uint32_t eye = 0; eye < 2; eye++) {
				const auto &view = xr_.display.views[eye];
				Log("Display view %u: viewport %d,%d %dx%d, fov L%.1f R%.1f U%.1f D%.1f\n", eye,
				    view.viewport.offset.x, view.viewport.offset.y, view.viewport.extent.width,
				    view.viewport.extent.height, view.fov.angleLeft * 57.2958f, view.fov.angleRight * 57.2958f,
				    view.fov.angleUp * 57.2958f, view.fov.angleDown * 57.2958f);
				const float points[][2] = {{0.5f, 0.5f}, {0.0f, 0.5f}, {1.0f, 0.5f}, {0.5f, 0.0f}, {0.0f, 0.0f}};
				for (const auto &point : points) {
					XrVector2f rgb[3];
					if (xr_.ComputeDisplayDistortion(eye, point[0], point[1], rgb)) {
						Log("  distortion eye %u (%.1f, %.1f) -> (%.3f, %.3f)\n", eye, point[0], point[1],
						    rgb[1].x, rgb[1].y);
					} else {
						Log("  distortion eye %u (%.1f, %.1f) failed\n", eye, point[0], point[1]);
					}
				}
			}
		}
	}

	// ITrackedDeviceServerDriver
	vr::EVRInitError
	Activate(uint32_t objectId) override
	{
		objectId_ = objectId;
		auto container = vr::VRProperties()->TrackedDeviceToPropertyContainer(objectId);
		auto *props = vr::VRProperties();
		frequency_ = vr::VRSettings()->GetFloat(kSettingsSection, "displayFrequency");
		if (virtualDisplay_ && xr_.display.nominalRefreshRate > 0) {
			frequency_ = xr_.display.nominalRefreshRate; // the runtime's display, not a guess
		}
		if (frequency_ <= 0) {
			frequency_ = 90.0f;
		}
		XrVector3f l = xr_.eyeInHead[0].position, r = xr_.eyeInHead[1].position;
		float ipd = sqrtf((r.x - l.x) * (r.x - l.x) + (r.y - l.y) * (r.y - l.y) + (r.z - l.z) * (r.z - l.z));
		props->SetStringProperty(container, vr::Prop_TrackingSystemName_String, "mwxr");
		props->SetStringProperty(container, vr::Prop_ModelNumber_String, xr_.systemName.c_str());
		props->SetStringProperty(container, vr::Prop_ManufacturerName_String, xr_.runtimeName.c_str());
		props->SetStringProperty(container, vr::Prop_RenderModelName_String, "generic_hmd");
		// Home spawns the avatar's hands for the controllers the headset expects. Unset, it falls back to a
		// gamepad and, if our controllers attach later (PS Sense tracking takes a while to lock), never
		// spawns hands for them.
		props->SetStringProperty(container, vr::Prop_ExpectedControllerType_String, "oculus_touch");
		props->SetFloatProperty(container, vr::Prop_UserIpdMeters_Float, ipd);
		props->SetFloatProperty(container, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.0f);
		props->SetFloatProperty(container, vr::Prop_DisplayFrequency_Float, frequency_);
		props->SetFloatProperty(container, vr::Prop_SecondsFromVsyncToPhotons_Float, 1.0f / frequency_);
		props->SetBoolProperty(container, vr::Prop_IsOnDesktop_Bool, false);
		props->SetBoolProperty(container, vr::Prop_DriverDirectModeSendsVsyncEvents_Bool, true);
		// SteamVR waits for the headset to be worn (for example "Put on your
		// headset" and the tutorial). The runtime's user presence is not used
		// yet: the headset is reported as worn while the session runs.
		props->SetBoolProperty(container, vr::Prop_ContainsProximitySensor_Bool, true);
		props->SetBoolProperty(container, vr::Prop_DeviceProvidesBatteryStatus_Bool, false);
		props->SetBoolProperty(container, vr::Prop_HasCamera_Bool, false);
		props->SetBoolProperty(container, vr::Prop_DisplayDebugMode_Bool, false);
		SetPlayArea(container);
		vr::VRDriverInput()->CreateBooleanComponent(container, "/proximity", &proximity_);
		vr::VRDriverInput()->UpdateBooleanComponent(proximity_, true, 0);
		vr::VRServerDriverHost()->SetDisplayEyeToHead(objectId, ToMatrix(xr_.eyeInHead[0]),
		                                             ToMatrix(xr_.eyeInHead[1]));
		Log("HMD '%s' active: IPD %.1f mm, %.0f Hz\n", xr_.systemName.c_str(), ipd * 1000, frequency_);

		zeroCopy_ = vr::VRSettings()->GetBool(kSettingsSection, "zeroCopy");
		virtualPhotonRefreshes_ = vr::VRSettings()->GetFloat(kSettingsSection, "virtualPhotonRefreshes");
		if (virtualPhotonRefreshes_ < 0) {
			virtualPhotonRefreshes_ = 1.0f;
		}
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
		if (!virtualDisplay_ && !strcmp(name, vr::IVRDriverDirectModeComponent_Version)) {
			return static_cast<vr::IVRDriverDirectModeComponent *>(this);
		}
		if (virtualDisplay_ && !strcmp(name, vr::IVRVirtualDisplay_Version)) {
			return static_cast<vr::IVRVirtualDisplay *>(this);
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
		if (virtualDisplay_) {
			*width = xr_.display.displaySize.width;
			*height = xr_.display.displaySize.height;
			return;
		}
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
		if (virtualDisplay_) {
			const XrRect2Di &viewport = xr_.display.views[eye].viewport;
			*x = viewport.offset.x;
			*y = viewport.offset.y;
			*width = viewport.extent.width;
			*height = viewport.extent.height;
			return;
		}
		*x = eye == vr::Eye_Left ? 0 : xr_.recommendedWidth;
		*y = 0;
		*width = xr_.recommendedWidth;
		*height = xr_.recommendedHeight;
	}
	void
	GetProjectionRaw(vr::EVREye eye, float *left, float *right, float *top, float *bottom) override
	{
		// In virtual-display mode the distortion samples an image rendered with its own FOV.
		const XrFovf &fov = virtualDisplay_ ? xr_.display.views[eye].fov : xr_.fov[eye];
		*left = tanf(fov.angleLeft);
		*right = tanf(fov.angleRight);
		*top = tanf(-fov.angleUp);
		*bottom = tanf(-fov.angleDown);
	}
	vr::DistortionCoordinates_t *
	ComputeDistortion(vr::DistortionCoordinates_t *result, vr::EVREye eye, float u, float v) override
	{
		XrVector2f rgb[3];
		if (virtualDisplay_ && xr_.ComputeDisplayDistortion(eye, u, v, rgb)) {
			*result = {{rgb[0].x, rgb[0].y}, {rgb[1].x, rgb[1].y}, {rgb[2].x, rgb[2].y}};
		} else {
			if (virtualDisplay_ && !loggedDistortionFailure_) {
				Log("ComputeDistortion: the runtime's distortion failed; SteamVR gets none\n");
				loggedDistortionFailure_ = true;
			}
			*result = {{u, v}, {u, v}, {u, v}};
		}
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
		set->xr = &xr_;
		out->unTextureFlags = 0;
		if (zeroCopy_ && desc->nSampleCount <= 1 && IsCompositorProcess(pid) &&
		    xr_.CreateSharedSwapchain(desc->nWidth, desc->nHeight, (DXGI_FORMAT)desc->nFormat, set->swapchain,
		                              set->textures, set->sharedHandles)) {
			for (int i = 0; i < 3; ++i) {
				set->handles[i] = (vr::SharedTextureHandle_t)set->sharedHandles[i];
				out->rSharedTextureHandles[i] = set->handles[i];
				textures_[set->handles[i]] = {set, i};
			}
			// The application renders into the image it is given first.
			set->acquired = xr_.AcquireImage(set->swapchain);
			set->next = set->acquired >= 0 ? set->acquired : 0;
			Log("Swap texture set for pid %u: %ux%u format %u, zero-copy (first image %d)\n", pid, desc->nWidth,
			    desc->nHeight, desc->nFormat, set->acquired);
			return;
		}
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
				advanced = set;
				if (set->swapchain) {
					// The runtime decides the order; keep an image still unsubmitted.
					if (set->acquired < 0) {
						set->acquired = xr_.AcquireImage(set->swapchain);
					}
					if (set->acquired >= 0) {
						set->next = set->acquired;
					}
				} else {
					set->next = (set->next + 1) % 3;
				}
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
			TextureSet *set = it->second.set.get();
			ID3D11Texture2D *texture = set->textures[it->second.index];
			D3D11_TEXTURE2D_DESC desc;
			texture->GetDesc(&desc);
			if (set->swapchain) {
				if (it->second.index != set->acquired) {
					static int mismatches = 0;
					if (mismatches++ < 5) {
						Log("Zero-copy layer uses image %d but image %d is acquired\n", it->second.index,
						    set->acquired);
					}
					return;
				}
				layer.eye[eye].swapchain = set->swapchain;
				submittedSets_.push_back(it->second.set);
			}
			float u0 = std::min(in.bounds.uMin, in.bounds.uMax), u1 = std::max(in.bounds.uMin, in.bounds.uMax);
			float v0 = std::min(in.bounds.vMin, in.bounds.vMax), v1 = std::max(in.bounds.vMin, in.bounds.vMax);
			EyeSubmit &out = layer.eye[eye];
			out.texture = texture;
			out.box = {(UINT)lroundf(u0 * desc.Width), (UINT)lroundf(v0 * desc.Height), 0,
			           (UINT)lroundf(u1 * desc.Width), (UINT)lroundf(v1 * desc.Height), 1};
			// SteamVR renders in its world; the runtime composites in its own space.
			out.pose = Compose(Compose(Inverse(WorldFromDriver()), ToPose(in.mHmdPose)), xr_.eyeInHead[eye]);
			if (!FovFromProjection(in.mProjection, out.fov)) {
				out.fov = xr_.fov[eye];
			}
		}
		pending_.push_back(layer);
		if (pending_.size() == 1) {
			lastPrediction_ = perEye[0].flHmdPosePredictionTimeInSecondsFromNow;
			lastRenderHead_ = Compose(Inverse(WorldFromDriver()), ToPose(perEye[0].mHmdPose)).orientation;
		}
	}

	void
	Present(vr::SharedTextureHandle_t syncTexture) override
	{
		std::vector<LayerSubmit> layers;
		std::vector<std::shared_ptr<TextureSet>> submitted;
		{
			std::lock_guard<std::mutex> lock(texturesMutex_);
			layers.swap(pending_);
			submitted.swap(submittedSets_);
		}
		IDXGIKeyedMutex *mutex = SyncMutex(syncTexture);
		bool locked = mutex && mutex->AcquireSync(0, 100) == S_OK;
		if (mutex && !locked) {
			static int failures = 0;
			if (failures++ < 5) {
				Log("Sync texture AcquireSync failed; submitting no layers this frame\n");
			}
			layers.clear(); // zero-copy images stay acquired for the next frame
			submitted.clear();
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
		xr_.Present(layers); // releases the submitted zero-copy images
		{
			std::lock_guard<std::mutex> lock(texturesMutex_);
			for (auto &set : submitted) {
				set->acquired = -1;
			}
		}
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
		AdoptPeriod(period);
		// The vsync that has just occurred, as an offset from now (<= 0).
		vr::VRServerDriverHost()->VsyncEvent(-UpdateVsyncTiming(vsyncOffset, period));
	}

	// predictedDisplayPeriod becomes a multiple of the refresh when frames are
	// late, so only ever adopt a shorter period than seen so far. Returns true
	// when SteamVR's display frequency changed.
	bool
	AdoptPeriod(double period)
	{
		if (period <= 0 || (minPeriod_ != 0 && period >= minPeriod_ * 0.95)) {
			return false;
		}
		minPeriod_ = period;
		if (fabs(1.0 / period - frequency_) <= 1.0 || objectId_ == vr::k_unTrackedDeviceIndexInvalid) {
			return false;
		}
		frequency_ = (float)(1.0 / period);
		auto container = vr::VRProperties()->TrackedDeviceToPropertyContainer(objectId_);
		vr::VRProperties()->SetFloatProperty(container, vr::Prop_DisplayFrequency_Float, frequency_);
		Log("Runtime frame period %.3f ms: display frequency now %.2f Hz\n", period * 1000, frequency_);
		return true;
	}

	// IVRVirtualDisplay: SteamVR's compositor has distorted the frame with the
	// runtime's distortion, so its backbuffer is submitted as the display image
	// and presented as it is. Present ends the OpenXR frame; WaitForPresent
	// waits for and begins the next one.
	void
	Present(const vr::PresentInfo_t *info, uint32_t size) override
	{
		ID3D11Texture2D *backbuffer = nullptr;
		if (info && size >= sizeof(vr::PresentInfo_t)) {
			++presentCount_;
			backbuffer = Backbuffer(info->backbufferTextureHandle);
		}
		if (!backbuffer) {
			xr_.Present({});
			return;
		}
		// Frames 300 and 1200, and with virtualDisplayDumpEvery every that many frames (at most 20 dumps).
		if (dumpDir_[0] && (presentCount_ == 300 || presentCount_ == 1200 ||
		                    (dumpEvery_ > 0 && presentCount_ % dumpEvery_ == 0 && presentCount_ / dumpEvery_ <= 20))) {
			DumpBackbuffer(backbuffer, info->nFrameId);
		}
		xr_.PresentDisplayImage(backbuffer);
	}

	void
	WaitForPresent() override
	{
		double vsyncOffset = 0, period = 0;
		if (xr_.WaitAndBeginFrame(vsyncOffset, period)) {
			AdoptPeriod(period);
			UpdateVsyncTiming(vsyncOffset, period);
		}
	}

	// SteamVR paces from the last vsync and expects a frame it starts after
	// vsync V on screen at V + period + SecondsFromVsyncToPhotons. The runtime
	// gives the display time of the frame just begun, which can be several
	// refreshes ahead, so report the latest vsync that has already happened
	// (display time minus whole periods) and put the rest of the pipeline into
	// SecondsFromVsyncToPhotons. Returns the seconds since that vsync.
	double
	UpdateVsyncTiming(double vsyncOffset, double period)
	{
		double refresh = 1.0 / frequency_;
		double toDisplay = vsyncOffset + period; // seconds from now to the predicted display time
		double periods = ceil(toDisplay / refresh - 1e-3);
		if (periods < 1) {
			periods = 1;
		}
		double sinceVsync = periods * refresh - toDisplay; // >= 0
		double photons = (periods - 1) * refresh;
		LARGE_INTEGER now, frequency;
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&frequency);
		lastVsync_ = now.QuadPart - (LONGLONG)(sinceVsync * frequency.QuadPart);
		// Vsyncs since the first one reported, starting at 1, so skipped
		// refreshes still advance the counter.
		if (!firstVsync_) {
			firstVsync_ = lastVsync_;
		}
		vsyncCount_ = 1 + (uint64_t)llround((double)(lastVsync_ - firstVsync_) / ((double)frequency.QuadPart * refresh));
		// In virtual mode the runtime only presents SteamVR's finished image,
		// at the next vsync, so the display time it predicts (which leaves room
		// for its own compositor, and moves further ahead when frames are late)
		// does not measure the photon latency. Report a fixed one instead.
		if (virtualDisplay_) {
			SetVsyncToPhotons(virtualPhotonRefreshes_ * refresh, refresh);
			return sinceVsync;
		}
		// The prediction moves by a refresh from frame to frame, and each change
		// of SteamVR's property shifts its pose prediction by a refresh. Check
		// every half second, and only change it when no frame in that time
		// agreed with the current value; then use the median.
		photonSamples_[photonSampleCount_++ % kPhotonSamples] = photons;
		if (photonSampleCount_ % kPhotonSamples != 0 && vsyncToPhotons_ >= 0) {
			return sinceVsync;
		}
		size_t count = photonSampleCount_ < kPhotonSamples ? photonSampleCount_ : kPhotonSamples;
		double sorted[kPhotonSamples];
		std::copy(photonSamples_, photonSamples_ + count, sorted);
		std::sort(sorted, sorted + count);
		double tolerance = 0.25 * refresh;
		if (vsyncToPhotons_ >= sorted[0] - tolerance && vsyncToPhotons_ <= sorted[count - 1] + tolerance) {
			return sinceVsync;
		}
		SetVsyncToPhotons(sorted[count / 2], refresh);
		return sinceVsync;
	}

	void
	SetVsyncToPhotons(double photons, double refresh)
	{
		if (fabs(photons - vsyncToPhotons_) <= 0.25 * refresh || objectId_ == vr::k_unTrackedDeviceIndexInvalid) {
			return;
		}
		vsyncToPhotons_ = photons;
		auto container = vr::VRProperties()->TrackedDeviceToPropertyContainer(objectId_);
		vr::VRProperties()->SetFloatProperty(container, vr::Prop_SecondsFromVsyncToPhotons_Float, (float)photons);
		Log("Display timing: vsync to photons now %.2f ms (%.0f refreshes ahead)\n", photons * 1000,
		    photons / refresh + 1);
	}

	bool
	GetTimeSinceLastVsync(float *secondsSinceLastVsync, uint64_t *frameCounter) override
	{
		if (!lastVsync_) {
			return false;
		}
		LARGE_INTEGER now, frequency;
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&frequency);
		*secondsSinceLastVsync = (float)(now.QuadPart - lastVsync_) / (float)frequency.QuadPart;
		*frameCounter = vsyncCount_;
		return true;
	}

private:
	ID3D11Texture2D *
	Backbuffer(vr::SharedTextureHandle_t handle)
	{
		for (auto &entry : backbuffers_) {
			if (entry.first == handle) {
				return entry.second;
			}
		}
		ID3D11Texture2D *texture = nullptr;
		if (FAILED(xr_.device->OpenSharedResource((HANDLE)handle, __uuidof(ID3D11Texture2D), (void **)&texture))) {
			Log("Virtual display: OpenSharedResource(%p) failed\n", (void *)handle);
			return nullptr;
		}
		D3D11_TEXTURE2D_DESC desc;
		texture->GetDesc(&desc);
		Log("Virtual display backbuffer %zu: %ux%u format %d samples %u bind 0x%x misc 0x%x\n", backbuffers_.size(),
		    desc.Width, desc.Height, desc.Format, desc.SampleDesc.Count, desc.BindFlags, desc.MiscFlags);
		backbuffers_.push_back({handle, texture});
		return texture;
	}

	// Writes the backbuffer as a binary PPM (RGB, top row first).
	void
	DumpBackbuffer(ID3D11Texture2D *backbuffer, uint64_t frameId)
	{
		D3D11_TEXTURE2D_DESC desc;
		backbuffer->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.MiscFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ID3D11Texture2D *staging = nullptr;
		ID3D11DeviceContext *context = nullptr;
		xr_.device->GetImmediateContext(&context);
		IDXGIKeyedMutex *mutex = nullptr;
		backbuffer->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&mutex);
		bool locked = mutex && mutex->AcquireSync(0, 100) == S_OK;
		if (SUCCEEDED(xr_.device->CreateTexture2D(&desc, nullptr, &staging))) {
			context->CopyResource(staging, backbuffer);
			D3D11_MAPPED_SUBRESOURCE map;
			if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
				char path[600];
				snprintf(path, sizeof(path), "%s\\virtual-display-%llu.ppm", dumpDir_, (unsigned long long)frameId);
				FILE *f = fopen(path, "wb");
				bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
				            desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
				if (f) {
					fprintf(f, "P6\n%u %u\n255\n", desc.Width, desc.Height);
					std::vector<unsigned char> row(desc.Width * 3);
					for (uint32_t y = 0; y < desc.Height; ++y) {
						const unsigned char *src = (const unsigned char *)map.pData + (size_t)y * map.RowPitch;
						for (uint32_t x = 0; x < desc.Width; ++x) {
							row[x * 3 + 0] = src[x * 4 + (bgra ? 2 : 0)];
							row[x * 3 + 1] = src[x * 4 + 1];
							row[x * 3 + 2] = src[x * 4 + (bgra ? 0 : 2)];
						}
						fwrite(row.data(), 1, row.size(), f);
					}
					fclose(f);
					Log("Virtual display: dumped frame %llu (format %d, keyed mutex %d) to %s\n",
					    (unsigned long long)frameId, desc.Format, locked, path);
				}
				context->Unmap(staging, 0);
			}
			staging->Release();
		}
		if (locked) {
			mutex->ReleaseSync(0);
		}
		if (mutex) {
			mutex->Release();
		}
		context->Release();
	}

	// A standing universe at the reference space origin, so SteamVR does not
	// require room setup. The runtime's STAGE is already floor-level.
	void
	SetPlayArea(vr::PropertyContainerHandle_t container)
	{
		// driver_mwxr.playAreaSize (metres, square) overrides the runtime's
		// STAGE bounds; without either, 2 x 2 m.
		float size = vr::VRSettings()->GetFloat(kSettingsSection, "playAreaSize");
		bool overridden = size > 0;
		float width = overridden ? size : xr_.playAreaWidth > 0 ? xr_.playAreaWidth : 2.0f;
		float depth = overridden ? size : xr_.playAreaDepth > 0 ? xr_.playAreaDepth : 2.0f;
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
		// SteamVR decides when to show the boundary: the driver never does.
		props->SetBoolProperty(container, vr::Prop_DriverProvidedChaperoneVisibility_Bool, false);
		Log("Play area %.2f x %.2f m (%s)\n", width, depth,
		    overridden ? "playAreaSize" : xr_.playAreaWidth > 0 ? "runtime" : "default");
	}

	struct TextureSet
	{
		uint32_t pid = 0;
		ID3D11Texture2D *textures[3] = {};
		vr::SharedTextureHandle_t handles[3] = {};
		uint32_t next = 0;
		// Zero-copy: textures are the swapchain's images, owned by the runtime.
		XrBackend *xr = nullptr;
		XrSwapchain swapchain = XR_NULL_HANDLE;
		HANDLE sharedHandles[3] = {};
		int acquired = -1;
		~TextureSet()
		{
			if (swapchain) {
				xr->DestroySwapchain(swapchain);
				return;
			}
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
				// Hold the right Options button for a second to recentre.
				auto pressed = std::chrono::steady_clock::now();
				if (!hands[1].optionsClick) {
					optionsSince_ = {};
					recentredThisPress_ = false;
				} else if (optionsSince_ == std::chrono::steady_clock::time_point{}) {
					optionsSince_ = pressed;
				} else if (!recentredThisPress_ && pressed - optionsSince_ >= std::chrono::seconds(1)) {
					XrPosef head;
					XrVector3f linear, angular;
					bool positionValid;
					if (xr_.LocateHeadNow(head, linear, angular, positionValid)) {
						RecentreOn(head);
					}
					recentredThisPress_ = true;
				}
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
		ApplyWorldFromDriver(out);
		lastPose_ = out;
		// Worn while the session runs; repeated because SteamVR may not be
		// listening yet when the HMD activates.
		if (proximity_ != vr::k_ulInvalidInputComponentHandle) {
			vr::VRDriverInput()->UpdateBooleanComponent(proximity_, true, 0);
		}
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
	std::chrono::steady_clock::time_point lastPoseLog_, lastInputLog_, optionsSince_;
	bool recentredThisPress_ = false;

	std::mutex texturesMutex_;
	std::map<vr::SharedTextureHandle_t, TextureRef> textures_;
	std::vector<LayerSubmit> pending_;
	std::vector<std::shared_ptr<TextureSet>> submittedSets_;
	bool zeroCopy_ = true;

	bool virtualDisplay_ = false;
	bool loggedDistortionFailure_ = false;
	char dumpDir_[512] = {};
	int32_t dumpEvery_ = 0;
	std::vector<std::pair<vr::SharedTextureHandle_t, ID3D11Texture2D *>> backbuffers_;
	uint64_t presentCount_ = 0, vsyncCount_ = 0;
	LONGLONG lastVsync_ = 0;
	double vsyncToPhotons_ = -1;
	float virtualPhotonRefreshes_ = 1.0f;
	LONGLONG firstVsync_ = 0;
	vr::VRInputComponentHandle_t proximity_ = vr::k_ulInvalidInputComponentHandle;
	static constexpr size_t kPhotonSamples = 60;
	double photonSamples_[kPhotonSamples] = {};
	size_t photonSampleCount_ = 0;

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

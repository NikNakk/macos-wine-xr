// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
// A stand-in openvr_api.dll for OpenVR applications under Wine.
//
// Under Wine, SteamVR's compositor returns the HMD's render pose from
// IVRCompositor::WaitGetPoses (and GetLastPoses) with live values but
// bPoseIsValid = false and TrackingResult_Uninitialized, with any driver.
// Applications that respect the flag, such as Source 2's SteamVR Home, then
// stop following the head. See docs/steamvr-home.md.
//
// Every export forwards to Valve's DLL, renamed openvr_api_valve.dll beside
// this one (openvr_api.def), except VR_GetGenericInterface. For
// IVRCompositor_022 to _025 it patches the returned interface's
// WaitGetPoses, GetLastPoses and GetLastPoseForTrackedDeviceIndex (vtable
// slots 2 to 4) and Submit (slot 5; later versions insert GetSubmitTexture
// before it), and logs the application's frame loop every 5 s to
// MWXR_OPENVR_SHIM_LOG.
//
// With MWXR_OPENVR_SHIM_FIX_POSE=1 it also marks an HMD pose that is
// "uninitialised" but has a proper rotation valid and running. That did not
// change SteamVR Home, so it is off by default.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "third_party/openvr/openvr.h"

// Lets the launcher tell this DLL from Valve's.
extern "C" __attribute__((used)) const char mwxr_openvr_shim_marker[] = "mwxr-openvr-shim";

namespace {

using GetGenericInterfaceFn = void *(*)(const char *, vr::EVRInitError *);
using PosesFn = vr::EVRCompositorError (*)(void *, vr::TrackedDevicePose_t *, uint32_t, vr::TrackedDevicePose_t *,
                                           uint32_t);
using PoseForDeviceFn = vr::EVRCompositorError (*)(void *, vr::TrackedDeviceIndex_t, vr::TrackedDevicePose_t *,
                                                   vr::TrackedDevicePose_t *);
using SubmitFn = vr::EVRCompositorError (*)(void *, vr::EVREye, const vr::Texture_t *, const vr::VRTextureBounds_t *,
                                            vr::EVRSubmitFlags);

constexpr int kWaitGetPosesSlot = 2;
constexpr int kGetLastPosesSlot = 3;
constexpr int kGetLastPoseForDeviceSlot = 4;
constexpr int kSubmitSlot = 5;

// One entry per patched vtable: interface versions have their own vtables.
struct Patched
{
	void **vtable;
	PosesFn waitGetPoses;
	PosesFn getLastPoses;
	PoseForDeviceFn getLastPoseForDevice;
	SubmitFn submit;
};

SRWLOCK g_lock = SRWLOCK_INIT;
Patched g_patched[16];
int g_patchedCount = 0;
GetGenericInterfaceFn g_getGenericInterface = nullptr;
volatile LONG g_fixes = 0;

// WaitGetPoses statistics, logged every 5 s: the application's own view of
// its frame loop.
struct Stats
{
	SRWLOCK lock = SRWLOCK_INIT;
	LONGLONG windowStart = 0;
	uint32_t calls = 0, errors = 0, lastError = 0, invalidHmd = 0, fixed = 0;
	uint32_t submits[2] = {}, submitErrors = 0, lastSubmitError = 0, lastType = 0, lastColorSpace = 0,
	         lastFlags = 0, distinctHandles = 0;
	void *handles[8] = {};
	double waitTotalMs = 0, waitMaxMs = 0, intervalMaxMs = 0;
	LONGLONG lastReturn = 0;
} g_stats;

double
QpcMs(LONGLONG ticks)
{
	static LARGE_INTEGER frequency = {};
	if (!frequency.QuadPart) {
		QueryPerformanceFrequency(&frequency);
	}
	return (double)ticks * 1000.0 / (double)frequency.QuadPart;
}

void
Log(const char *format, ...)
{
	const char *path = getenv("MWXR_OPENVR_SHIM_LOG");
	if (!path || !path[0]) {
		return;
	}
	FILE *f = fopen(path, "a");
	if (!f) {
		return;
	}
	char exe[MAX_PATH] = {};
	GetModuleFileNameA(nullptr, exe, MAX_PATH);
	const char *name = strrchr(exe, '\\');
	fprintf(f, "[%s %lu] ", name ? name + 1 : exe, GetCurrentProcessId());
	va_list args;
	va_start(args, format);
	vfprintf(f, format, args);
	va_end(args);
	fclose(f);
}

const Patched *
Find(void *self)
{
	void **vtable = *reinterpret_cast<void ***>(self);
	AcquireSRWLockShared(&g_lock);
	const Patched *found = nullptr;
	for (int i = 0; i < g_patchedCount; ++i) {
		if (g_patched[i].vtable == vtable) {
			found = &g_patched[i];
		}
	}
	ReleaseSRWLockShared(&g_lock);
	return found;
}

// A rotation whose first column has unit length: values SteamVR filled in.
bool
Plausible(const vr::HmdMatrix34_t &m)
{
	double norm = m.m[0][0] * m.m[0][0] + m.m[1][0] * m.m[1][0] + m.m[2][0] * m.m[2][0];
	return std::isfinite(norm) && fabs(norm - 1.0) < 0.05;
}

// Returns true when it marked the pose valid.
bool
FixHmd(vr::TrackedDevicePose_t *pose)
{
	static const bool enabled = [] {
		const char *value = getenv("MWXR_OPENVR_SHIM_FIX_POSE");
		return value && value[0] == '1';
	}();
	if (!enabled || !pose || pose->bPoseIsValid || pose->eTrackingResult != vr::TrackingResult_Uninitialized ||
	    !Plausible(pose->mDeviceToAbsoluteTracking)) {
		return false;
	}
	pose->bPoseIsValid = true;
	pose->eTrackingResult = vr::TrackingResult_Running_OK;
	LONG fixes = InterlockedIncrement(&g_fixes);
	if (fixes == 1) {
		Log("marked the HMD render pose valid (first time)\n");
	}
	return true;
}

void
Record(LONGLONG begin, LONGLONG end, vr::EVRCompositorError error, const vr::TrackedDevicePose_t *hmdBefore,
       bool fixed)
{
	AcquireSRWLockExclusive(&g_stats.lock);
	Stats &s = g_stats;
	if (!s.windowStart) {
		s.windowStart = begin;
	}
	++s.calls;
	double waitMs = QpcMs(end - begin);
	s.waitTotalMs += waitMs;
	s.waitMaxMs = waitMs > s.waitMaxMs ? waitMs : s.waitMaxMs;
	if (s.lastReturn) {
		double intervalMs = QpcMs(end - s.lastReturn);
		s.intervalMaxMs = intervalMs > s.intervalMaxMs ? intervalMs : s.intervalMaxMs;
	}
	s.lastReturn = end;
	if (error != vr::VRCompositorError_None) {
		++s.errors;
		s.lastError = error;
	}
	if (hmdBefore && !hmdBefore->bPoseIsValid) {
		++s.invalidHmd;
	}
	if (fixed) {
		++s.fixed;
	}
	double windowMs = QpcMs(end - s.windowStart);
	if (windowMs >= 5000.0) {
		Log("WaitGetPoses: %.1f calls/s, wait avg %.2f max %.2f ms, longest gap %.1f ms, errors %u (last %u), "
		    "HMD invalid %u, marked valid %u\n",
		    s.calls * 1000.0 / windowMs, s.waitTotalMs / s.calls, s.waitMaxMs, s.intervalMaxMs, s.errors,
		    s.lastError, s.invalidHmd, s.fixed);
		Log("Submit: left %u right %u, errors %u (last %u), texture type %u colour space %u flags 0x%x, "
		    "distinct handles %u\n",
		    s.submits[0], s.submits[1], s.submitErrors, s.lastSubmitError, s.lastType, s.lastColorSpace,
		    s.lastFlags, s.distinctHandles);
		s.windowStart = end;
		s.calls = s.errors = s.lastError = s.invalidHmd = s.fixed = 0;
		s.submits[0] = s.submits[1] = s.submitErrors = s.lastSubmitError = s.distinctHandles = 0;
		memset(s.handles, 0, sizeof(s.handles));
		s.waitTotalMs = s.waitMaxMs = s.intervalMaxMs = 0;
	}
	ReleaseSRWLockExclusive(&g_stats.lock);
}

vr::EVRCompositorError
WaitGetPosesHook(void *self, vr::TrackedDevicePose_t *render, uint32_t renderCount, vr::TrackedDevicePose_t *game,
                 uint32_t gameCount)
{
	const Patched *p = Find(self);
	LARGE_INTEGER begin, end;
	QueryPerformanceCounter(&begin);
	vr::EVRCompositorError error = p->waitGetPoses(self, render, renderCount, game, gameCount);
	QueryPerformanceCounter(&end);
	bool haveHmd = render && renderCount > vr::k_unTrackedDeviceIndex_Hmd;
	vr::TrackedDevicePose_t before = haveHmd ? render[vr::k_unTrackedDeviceIndex_Hmd] : vr::TrackedDevicePose_t{};
	bool fixed = haveHmd && FixHmd(&render[vr::k_unTrackedDeviceIndex_Hmd]);
	if (game && gameCount > vr::k_unTrackedDeviceIndex_Hmd) {
		FixHmd(&game[vr::k_unTrackedDeviceIndex_Hmd]);
	}
	Record(begin.QuadPart, end.QuadPart, error, haveHmd ? &before : nullptr, fixed);
	return error;
}

vr::EVRCompositorError
GetLastPosesHook(void *self, vr::TrackedDevicePose_t *render, uint32_t renderCount, vr::TrackedDevicePose_t *game,
                 uint32_t gameCount)
{
	const Patched *p = Find(self);
	vr::EVRCompositorError error = p->getLastPoses(self, render, renderCount, game, gameCount);
	if (render && renderCount > vr::k_unTrackedDeviceIndex_Hmd) {
		FixHmd(&render[vr::k_unTrackedDeviceIndex_Hmd]);
	}
	if (game && gameCount > vr::k_unTrackedDeviceIndex_Hmd) {
		FixHmd(&game[vr::k_unTrackedDeviceIndex_Hmd]);
	}
	return error;
}

vr::EVRCompositorError
GetLastPoseForDeviceHook(void *self, vr::TrackedDeviceIndex_t device, vr::TrackedDevicePose_t *render,
                         vr::TrackedDevicePose_t *game)
{
	const Patched *p = Find(self);
	vr::EVRCompositorError error = p->getLastPoseForDevice(self, device, render, game);
	if (device == vr::k_unTrackedDeviceIndex_Hmd) {
		FixHmd(render);
		FixHmd(game);
	}
	return error;
}

// Arguments pass through unchanged, so a wrong slot would only log nonsense.
vr::EVRCompositorError
SubmitHook(void *self, vr::EVREye eye, const vr::Texture_t *texture, const vr::VRTextureBounds_t *bounds,
           vr::EVRSubmitFlags flags)
{
	const Patched *p = Find(self);
	vr::EVRCompositorError error = p->submit(self, eye, texture, bounds, flags);
	AcquireSRWLockExclusive(&g_stats.lock);
	Stats &s = g_stats;
	if (eye == vr::Eye_Left || eye == vr::Eye_Right) {
		++s.submits[eye];
	}
	if (error != vr::VRCompositorError_None) {
		++s.submitErrors;
		s.lastSubmitError = error;
	}
	if (texture) {
		s.lastType = texture->eType;
		s.lastColorSpace = texture->eColorSpace;
		bool seen = false;
		for (uint32_t i = 0; i < s.distinctHandles && i < 8; ++i) {
			seen = seen || s.handles[i] == texture->handle;
		}
		if (!seen) {
			if (s.distinctHandles < 8) {
				s.handles[s.distinctHandles] = texture->handle;
			}
			++s.distinctHandles;
		}
	}
	s.lastFlags = flags;
	ReleaseSRWLockExclusive(&g_stats.lock);
	return error;
}

void
Patch(void *compositor, const char *version)
{
	void **vtable = *reinterpret_cast<void ***>(compositor);
	AcquireSRWLockExclusive(&g_lock);
	for (int i = 0; i < g_patchedCount; ++i) {
		if (g_patched[i].vtable == vtable) {
			ReleaseSRWLockExclusive(&g_lock);
			return;
		}
	}
	if (g_patchedCount == (int)(sizeof(g_patched) / sizeof(g_patched[0]))) {
		ReleaseSRWLockExclusive(&g_lock);
		return;
	}
	Patched &p = g_patched[g_patchedCount];
	p.vtable = vtable;
	p.waitGetPoses = reinterpret_cast<PosesFn>(vtable[kWaitGetPosesSlot]);
	p.getLastPoses = reinterpret_cast<PosesFn>(vtable[kGetLastPosesSlot]);
	p.getLastPoseForDevice = reinterpret_cast<PoseForDeviceFn>(vtable[kGetLastPoseForDeviceSlot]);
	p.submit = reinterpret_cast<SubmitFn>(vtable[kSubmitSlot]);
	DWORD old = 0;
	if (VirtualProtect(&vtable[kWaitGetPosesSlot], 4 * sizeof(void *), PAGE_READWRITE, &old)) {
		vtable[kWaitGetPosesSlot] = reinterpret_cast<void *>(&WaitGetPosesHook);
		vtable[kGetLastPosesSlot] = reinterpret_cast<void *>(&GetLastPosesHook);
		vtable[kGetLastPoseForDeviceSlot] = reinterpret_cast<void *>(&GetLastPoseForDeviceHook);
		vtable[kSubmitSlot] = reinterpret_cast<void *>(&SubmitHook);
		VirtualProtect(&vtable[kWaitGetPosesSlot], 4 * sizeof(void *), old, &old);
		++g_patchedCount;
		Log("patched %s (vtable %p)\n", version, (void *)vtable);
	} else {
		Log("could not patch %s: VirtualProtect error %lu\n", version, GetLastError());
	}
	ReleaseSRWLockExclusive(&g_lock);
}

} // namespace

extern "C" void *
VR_GetGenericInterface(const char *version, vr::EVRInitError *error)
{
	if (!g_getGenericInterface) {
		HMODULE real = GetModuleHandleA("openvr_api_valve.dll");
		if (!real) {
			real = LoadLibraryA("openvr_api_valve.dll");
		}
		if (real) {
			g_getGenericInterface =
			    reinterpret_cast<GetGenericInterfaceFn>(GetProcAddress(real, "VR_GetGenericInterface"));
		}
		if (!g_getGenericInterface) {
			if (error) {
				*error = vr::VRInitError_Init_InterfaceNotFound;
			}
			return nullptr;
		}
	}
	void *result = g_getGenericInterface(version, error);
	// Plain C++ interfaces only: "FnTable:" tables have a different layout.
	if (result && version && !strncmp(version, "IVRCompositor_", 14) && atoi(version + 14) >= 22 &&
	    atoi(version + 14) <= 25) {
		Patch(result, version);
	}
	return result;
}

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
// IVRCompositor_* it patches the returned interface's WaitGetPoses,
// GetLastPoses and GetLastPoseForTrackedDeviceIndex (vtable slots 2 to 4
// from IVRCompositor_022 on). After Valve's call, an HMD pose that is
// "uninitialised" but has a proper rotation is marked valid and running.
// Nothing else changes.
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

constexpr int kWaitGetPosesSlot = 2;
constexpr int kGetLastPosesSlot = 3;
constexpr int kGetLastPoseForDeviceSlot = 4;

// One entry per patched vtable: interface versions have their own vtables.
struct Patched
{
	void **vtable;
	PosesFn waitGetPoses;
	PosesFn getLastPoses;
	PoseForDeviceFn getLastPoseForDevice;
};

SRWLOCK g_lock = SRWLOCK_INIT;
Patched g_patched[16];
int g_patchedCount = 0;
GetGenericInterfaceFn g_getGenericInterface = nullptr;
volatile LONG g_fixes = 0;

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

void
FixHmd(vr::TrackedDevicePose_t *pose)
{
	if (!pose || pose->bPoseIsValid || pose->eTrackingResult != vr::TrackingResult_Uninitialized ||
	    !Plausible(pose->mDeviceToAbsoluteTracking)) {
		return;
	}
	pose->bPoseIsValid = true;
	pose->eTrackingResult = vr::TrackingResult_Running_OK;
	LONG fixes = InterlockedIncrement(&g_fixes);
	if (fixes == 1 || fixes % 10000 == 0) {
		Log("marked the HMD render pose valid (%ld times so far)\n", fixes);
	}
}

vr::EVRCompositorError
WaitGetPosesHook(void *self, vr::TrackedDevicePose_t *render, uint32_t renderCount, vr::TrackedDevicePose_t *game,
                 uint32_t gameCount)
{
	const Patched *p = Find(self);
	vr::EVRCompositorError error = p->waitGetPoses(self, render, renderCount, game, gameCount);
	if (render && renderCount > vr::k_unTrackedDeviceIndex_Hmd) {
		FixHmd(&render[vr::k_unTrackedDeviceIndex_Hmd]);
	}
	if (game && gameCount > vr::k_unTrackedDeviceIndex_Hmd) {
		FixHmd(&game[vr::k_unTrackedDeviceIndex_Hmd]);
	}
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
	DWORD old = 0;
	if (VirtualProtect(&vtable[kWaitGetPosesSlot], 3 * sizeof(void *), PAGE_READWRITE, &old)) {
		vtable[kWaitGetPosesSlot] = reinterpret_cast<void *>(&WaitGetPosesHook);
		vtable[kGetLastPosesSlot] = reinterpret_cast<void *>(&GetLastPosesHook);
		vtable[kGetLastPoseForDeviceSlot] = reinterpret_cast<void *>(&GetLastPoseForDeviceHook);
		VirtualProtect(&vtable[kWaitGetPosesSlot], 3 * sizeof(void *), old, &old);
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
	if (result && version && !strncmp(version, "IVRCompositor_", 14) &&
	    atoi(version + 14) >= 22) {
		Patch(result, version);
	}
	return result;
}

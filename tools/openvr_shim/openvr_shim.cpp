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
// IVRCompositor_022 to _027 (Half-Life: Alyx uses 026) it patches the returned interface's
// WaitGetPoses, GetLastPoses and GetLastPoseForTrackedDeviceIndex (vtable
// slots 2 to 4) and Submit (slot 5; later versions insert GetSubmitTexture
// before it), and logs the application's frame loop every 5 s to
// MWXR_OPENVR_SHIM_LOG.
//
// It also hooks PostPresentHandoff (slot 7) to trace it. The application keeps
// SteamVR's sync texture from its last Submit until it calls either that or
// its next WaitGetPoses, and the compositor waits for it meanwhile. With
// MWXR_OPENVR_SHIM_HANDOFF=1 the shim calls PostPresentHandoff itself, on the
// submitting thread, once both eyes of a frame are submitted, unless the
// application has already called it for that frame.
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
using HandoffFn = void (*)(void *);
using FrameTimingFn = bool (*)(void *, vr::Compositor_FrameTiming *, uint32_t);

constexpr int kWaitGetPosesSlot = 2;
constexpr int kGetLastPosesSlot = 3;
constexpr int kGetLastPoseForDeviceSlot = 4;
constexpr int kSubmitSlot = 5;
constexpr int kPostPresentHandoffSlot = 7; // after ClearLastSubmittedFrame
constexpr int kGetFrameTimingSlot = 8;      // called, not patched

// One entry per patched vtable: interface versions have their own vtables.
struct Patched
{
	void **vtable;
	PosesFn waitGetPoses;
	PosesFn getLastPoses;
	PoseForDeviceFn getLastPoseForDevice;
	SubmitFn submit;
	HandoffFn postPresentHandoff;
	FrameTimingFn getFrameTiming;
};

SRWLOCK g_lock = SRWLOCK_INIT;
Patched g_patched[16];
int g_patchedCount = 0;
GetGenericInterfaceFn g_getGenericInterface = nullptr;
volatile LONG g_fixes = 0;

// The current frame's submitted eyes (bits 0 and 1) and whether it has been
// handed off (bit 2). WaitGetPoses starts a new frame.
constexpr LONG kHandedOff = 4;
volatile LONG g_frame = 0;

bool
InjectHandoff()
{
	static const bool enabled = [] {
		const char *value = getenv("MWXR_OPENVR_SHIM_HANDOFF");
		return value && !strcmp(value, "1");
	}();
	return enabled;
}

// WaitGetPoses statistics, logged every 5 s: the application's own view of
// its frame loop.
struct Stats
{
	SRWLOCK lock = SRWLOCK_INIT;
	LONGLONG windowStart = 0;
	uint32_t calls = 0, errors = 0, lastError = 0, invalidHmd = 0, fixed = 0;
	uint32_t submits[2] = {}, submitErrors = 0, lastSubmitError = 0, lastType = 0, lastColorSpace = 0,
	         lastFlags = 0, distinctHandles = 0;
	uint32_t appHandoffs = 0, injectedHandoffs = 0;
	void *handles[8] = {};
	double submitTotalMs = 0, submitMaxMs = 0, submitGapMaxMs = 0;
	LONGLONG lastSubmit = 0;
	double waitTotalMs = 0, waitMaxMs = 0, intervalMaxMs = 0;
	LONGLONG lastReturn = 0;
} g_stats;

double
QpcMs(LONGLONG ticks)
{
	static const LONGLONG frequency = [] {
		LARGE_INTEGER value;
		QueryPerformanceFrequency(&value);
		return value.QuadPart;
	}();
	return (double)ticks * 1000.0 / (double)frequency;
}

void
Log(const char *format, ...)
{
	// Without MWXR_OPENVR_SHIM_LOG (an app started by a Steam that was not
	// launched with it), log beside this DLL.
	char fallback[MAX_PATH] = {};
	const char *path = getenv("MWXR_OPENVR_SHIM_LOG");
	if (!path || !path[0]) {
		HMODULE self = nullptr;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                   reinterpret_cast<LPCSTR>(&Log), &self);
		DWORD length = GetModuleFileNameA(self, fallback, MAX_PATH);
		char *slash = strrchr(fallback, '\\');
		if (!length || !slash || (size_t)(slash - fallback) + sizeof("\\openvr-shim.log") > MAX_PATH) {
			return;
		}
		strcpy(slash, "\\openvr-shim.log");
		path = fallback;
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

// Opt-in complete call intervals, buffered separately from aggregate logs.
// On this CrossOver macOS build QPC and server history use the same
// mach_continuous_time epoch. Keep frequency conversion explicit.
void
TraceCall(const char *call, LONGLONG begin, LONGLONG end, vr::EVRCompositorError error,
          int eye = -1, const void *texture = nullptr)
{
	static const bool enabled = [] {
		const char *value = getenv("MWXR_OPENVR_SHIM_TRACE");
		return value && !strcmp(value, "1");
	}();
	if (!enabled) return;
	static SRWLOCK lock = SRWLOCK_INIT;
	static FILE *file = nullptr;
	static bool attempted = false;
	static LONGLONG lastFlush = 0;
	AcquireSRWLockExclusive(&lock);
	if (!attempted) {
		attempted = true;
		const char *path = getenv("MWXR_OPENVR_SHIM_LOG");
		if (path && *path) {
			char tracePath[32768];
			if (snprintf(tracePath, sizeof(tracePath), "%s.trace", path) < (int)sizeof(tracePath)) {
				file = fopen(tracePath, "a");
				if (file) {
					setvbuf(file, nullptr, _IOFBF, 65536);
					LARGE_INTEGER frequency;
					QueryPerformanceFrequency(&frequency);
					fprintf(file, "trace_active pid=%lu frequency=%lld tick_ms=%llu qpc_us=%.0f\n",
					        GetCurrentProcessId(), frequency.QuadPart, GetTickCount64(), QpcMs(begin) * 1000.0);
				}
			}
		}
	}
	if (file) {
		fprintf(file, "call=%s pid=%lu tid=%lu begin_us=%.0f end_us=%.0f duration_ms=%.6f eye=%d texture=%p error=%u\n",
		        call, GetCurrentProcessId(), GetCurrentThreadId(), QpcMs(begin) * 1000.0,
		        QpcMs(end) * 1000.0, QpcMs(end - begin), eye, texture, (unsigned)error);
		if (!lastFlush || QpcMs(end - lastFlush) >= 1000.0) {
			LARGE_INTEGER flushBegin, flushEnd;
			QueryPerformanceCounter(&flushBegin);
			fflush(file);
			QueryPerformanceCounter(&flushEnd);
			fprintf(file, "trace_flush pid=%lu begin_us=%.0f end_us=%.0f duration_ms=%.6f\n",
			        GetCurrentProcessId(), QpcMs(flushBegin.QuadPart) * 1000.0,
			        QpcMs(flushEnd.QuadPart) * 1000.0, QpcMs(flushEnd.QuadPart - flushBegin.QuadPart));
			lastFlush = end;
		}
	}
	ReleaseSRWLockExclusive(&lock);
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
		uint32_t submits = s.submits[0] + s.submits[1];
		Log("Submit: left %u right %u, in-call avg %.2f max %.2f ms, longest gap %.1f ms, errors %u (last %u), "
		    "texture type %u colour space %u flags 0x%x, distinct handles %u\n",
		    s.submits[0], s.submits[1], submits ? s.submitTotalMs / submits : 0.0, s.submitMaxMs,
		    s.submitGapMaxMs, s.submitErrors, s.lastSubmitError, s.lastType, s.lastColorSpace, s.lastFlags,
		    s.distinctHandles);
		Log("PostPresentHandoff: application %u, injected %u%s\n", s.appHandoffs, s.injectedHandoffs,
		    InjectHandoff() ? "" : " (injection off)");
		s.appHandoffs = s.injectedHandoffs = 0;
		s.windowStart = end;
		s.calls = s.errors = s.lastError = s.invalidHmd = s.fixed = 0;
		s.submits[0] = s.submits[1] = s.submitErrors = s.lastSubmitError = s.distinctHandles = 0;
		s.submitTotalMs = s.submitMaxMs = s.submitGapMaxMs = 0;
		memset(s.handles, 0, sizeof(s.handles));
		s.waitTotalMs = s.waitMaxMs = s.intervalMaxMs = 0;
	}
	ReleaseSRWLockExclusive(&g_stats.lock);
}

// SteamVR's own timing of the application's last frame, about once a second
// (MWXR_OPENVR_SHIM_FRAME_TIMING=1). Source 2's auto-fidelity is suspected to
// read these GPU times.
void
LogFrameTiming(const Patched *p, void *self, LONGLONG now)
{
	static const bool enabled = [] {
		const char *value = getenv("MWXR_OPENVR_SHIM_FRAME_TIMING");
		return value && !strcmp(value, "1");
	}();
	static LONGLONG last = 0;
	if (!enabled || !p->getFrameTiming || (last && QpcMs(now - last) < 1000.0)) {
		return;
	}
	last = now;
	vr::Compositor_FrameTiming t = {};
	t.m_nSize = sizeof(t);
	if (!p->getFrameTiming(self, &t, 0)) {
		Log("GetFrameTiming failed\n");
		return;
	}
	Log("FrameTiming %u: pre-submit GPU %.2f post-submit GPU %.2f total render GPU %.2f compositor GPU %.2f "
	    "client interval %.2f ms, presents %u dropped %u\n",
	    t.m_nFrameIndex, t.m_flPreSubmitGpuMs, t.m_flPostSubmitGpuMs, t.m_flTotalRenderGpuMs,
	    t.m_flCompositorRenderGpuMs, t.m_flClientFrameIntervalMs, t.m_nNumFramePresents, t.m_nNumDroppedFrames);
}

vr::EVRCompositorError
WaitGetPosesHook(void *self, vr::TrackedDevicePose_t *render, uint32_t renderCount, vr::TrackedDevicePose_t *game,
                 uint32_t gameCount)
{
	const Patched *p = Find(self);
	InterlockedExchange(&g_frame, 0);
	LARGE_INTEGER begin, end;
	QueryPerformanceCounter(&begin);
	vr::EVRCompositorError error = p->waitGetPoses(self, render, renderCount, game, gameCount);
	QueryPerformanceCounter(&end);
	TraceCall("WaitGetPoses", begin.QuadPart, end.QuadPart, error);
	bool haveHmd = render && renderCount > vr::k_unTrackedDeviceIndex_Hmd;
	vr::TrackedDevicePose_t before = haveHmd ? render[vr::k_unTrackedDeviceIndex_Hmd] : vr::TrackedDevicePose_t{};
	bool fixed = haveHmd && FixHmd(&render[vr::k_unTrackedDeviceIndex_Hmd]);
	if (game && gameCount > vr::k_unTrackedDeviceIndex_Hmd) {
		FixHmd(&game[vr::k_unTrackedDeviceIndex_Hmd]);
	}
	Record(begin.QuadPart, end.QuadPart, error, haveHmd ? &before : nullptr, fixed);
	LogFrameTiming(p, self, end.QuadPart);
	return error;
}

vr::EVRCompositorError
GetLastPosesHook(void *self, vr::TrackedDevicePose_t *render, uint32_t renderCount, vr::TrackedDevicePose_t *game,
                 uint32_t gameCount)
{
	const Patched *p = Find(self);
	LARGE_INTEGER begin, end;
	QueryPerformanceCounter(&begin);
	vr::EVRCompositorError error = p->getLastPoses(self, render, renderCount, game, gameCount);
	QueryPerformanceCounter(&end);
	TraceCall("GetLastPoses", begin.QuadPart, end.QuadPart, error);
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
	LARGE_INTEGER begin, end;
	QueryPerformanceCounter(&begin);
	vr::EVRCompositorError error = p->getLastPoseForDevice(self, device, render, game);
	QueryPerformanceCounter(&end);
	TraceCall("GetLastPoseForDevice", begin.QuadPart, end.QuadPart, error);
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
	LARGE_INTEGER begin, end;
	QueryPerformanceCounter(&begin);
	vr::EVRCompositorError error = p->submit(self, eye, texture, bounds, flags);
	QueryPerformanceCounter(&end);
	TraceCall("Submit", begin.QuadPart, end.QuadPart, error, (int)eye, texture ? texture->handle : nullptr);
	AcquireSRWLockExclusive(&g_stats.lock);
	Stats &s = g_stats;
	double inCallMs = QpcMs(end.QuadPart - begin.QuadPart);
	s.submitTotalMs += inCallMs;
	s.submitMaxMs = inCallMs > s.submitMaxMs ? inCallMs : s.submitMaxMs;
	if (s.lastSubmit) {
		double gapMs = QpcMs(end.QuadPart - s.lastSubmit);
		s.submitGapMaxMs = gapMs > s.submitGapMaxMs ? gapMs : s.submitGapMaxMs;
	}
	s.lastSubmit = end.QuadPart;
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
	if (error == vr::VRCompositorError_None && (eye == vr::Eye_Left || eye == vr::Eye_Right) && InjectHandoff() &&
	    p->postPresentHandoff) {
		LONG bit = 1 << eye;
		LONG frame = InterlockedOr(&g_frame, bit) | bit;
		if ((frame & 3) == 3 && !(InterlockedOr(&g_frame, kHandedOff) & kHandedOff)) {
			LARGE_INTEGER handoffBegin, handoffEnd;
			QueryPerformanceCounter(&handoffBegin);
			p->postPresentHandoff(self);
			QueryPerformanceCounter(&handoffEnd);
			TraceCall("InjectedPostPresentHandoff", handoffBegin.QuadPart, handoffEnd.QuadPart,
			          vr::VRCompositorError_None);
			AcquireSRWLockExclusive(&g_stats.lock);
			++g_stats.injectedHandoffs;
			ReleaseSRWLockExclusive(&g_stats.lock);
		}
	}
	return error;
}

void
PostPresentHandoffHook(void *self)
{
	const Patched *p = Find(self);
	InterlockedOr(&g_frame, kHandedOff);
	LARGE_INTEGER begin, end;
	QueryPerformanceCounter(&begin);
	p->postPresentHandoff(self);
	QueryPerformanceCounter(&end);
	TraceCall("PostPresentHandoff", begin.QuadPart, end.QuadPart, vr::VRCompositorError_None);
	AcquireSRWLockExclusive(&g_stats.lock);
	++g_stats.appHandoffs;
	ReleaseSRWLockExclusive(&g_stats.lock);
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
	p.postPresentHandoff = reinterpret_cast<HandoffFn>(vtable[kPostPresentHandoffSlot]);
	p.getFrameTiming = reinterpret_cast<FrameTimingFn>(vtable[kGetFrameTimingSlot]);
	constexpr SIZE_T kPatchedBytes = (kPostPresentHandoffSlot - kWaitGetPosesSlot + 1) * sizeof(void *);
	DWORD old = 0;
	if (VirtualProtect(&vtable[kWaitGetPosesSlot], kPatchedBytes, PAGE_READWRITE, &old)) {
		vtable[kWaitGetPosesSlot] = reinterpret_cast<void *>(&WaitGetPosesHook);
		vtable[kGetLastPosesSlot] = reinterpret_cast<void *>(&GetLastPosesHook);
		vtable[kGetLastPoseForDeviceSlot] = reinterpret_cast<void *>(&GetLastPoseForDeviceHook);
		vtable[kSubmitSlot] = reinterpret_cast<void *>(&SubmitHook);
		vtable[kPostPresentHandoffSlot] = reinterpret_cast<void *>(&PostPresentHandoffHook);
		VirtualProtect(&vtable[kWaitGetPosesSlot], kPatchedBytes, old, &old);
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
	    atoi(version + 14) <= 27) {
		Patch(result, version);
	}
	return result;
}

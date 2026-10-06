// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
// A minimal OpenVR scene application for checking how SteamVR treats an
// application through a driver: it draws a world-fixed sky and floor grid
// with Direct3D 11 and logs, once a second, what the application sees.
//
//   openvr_probe.exe [--seconds N] [--explicit-timing] [--log FILE]
//
// Grid: sky lines every 15 degrees (cyan), the -Z "forward" meridian and the
// horizon in red, and a 1 m floor grid at y = 0 (green) with the x axis red
// and the z axis blue. A correct pose keeps all of it fixed in the room.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "third_party/openvr/openvr.h"

namespace {

const char kShader[] = R"(
cbuffer Eye : register(b0)
{
	float4 tangents; // left, right, top, bottom: OpenVR raw projection, y down
	float4 row0;     // eye-to-world 3x4: rotation columns and position in w
	float4 row1;
	float4 row2;
};
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID)
{
	V o;
	o.uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return o;
}
float grid(float x, float width)
{
	float d = abs(frac(x + 0.5) - 0.5);
	return 1 - smoothstep(0, width, d);
}
float4 ps(V i) : SV_Target
{
	float3 d = normalize(float3(lerp(tangents.x, tangents.y, i.uv.x), -lerp(tangents.z, tangents.w, i.uv.y), -1));
	float3 w = normalize(float3(dot(row0.xyz, d), dot(row1.xyz, d), dot(row2.xyz, d)));
	float3 eye = float3(row0.w, row1.w, row2.w);
	float az = degrees(atan2(w.x, -w.z));
	float el = degrees(asin(clamp(w.y, -1, 1)));
	float3 c = float3(0.02, 0.03, 0.08) + 0.1 * saturate(w.y);
	c = lerp(c, float3(0, 0.8, 0.9), 0.8 * max(grid(az / 15, 0.02), grid(el / 15, 0.02)));
	float red = max(1 - smoothstep(0.15, 0.3, abs(el)), 1 - smoothstep(0.15, 0.3, abs(az)));
	c = lerp(c, float3(1, 0.15, 0.15), red);
	if (w.y < -1e-3) {
		float t = -eye.y / w.y;
		float3 p = eye + t * w;
		float3 ink = float3(0.1, 0.9, 0.2);
		if (abs(p.z) < 0.03) ink = float3(1, 0.15, 0.15);
		if (abs(p.x) < 0.03) ink = float3(0.2, 0.4, 1);
		float lines = max(grid(p.x, 0.02), grid(p.z, 0.02)) * saturate(6 / t);
		c = lerp(float3(0.07, 0.06, 0.05), ink, lines);
	}
	return float4(c, 1);
}
)";

struct EyeConstants
{
	float tangents[4];
	float rows[3][4];
};

FILE *g_log = nullptr;

void
Log(const char *format, ...)
{
	char line[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	fputs(line, stdout);
	fflush(stdout);
	if (g_log) {
		fputs(line, g_log);
		fflush(g_log);
	}
}

// IVRSystem::GetEyeToHeadTransform returns a struct by value: MSVC passes
// `this`, then the result pointer; MinGW would swap them. Call the vtable
// slot with the MSVC layout (slot 5 of IVRSystem_026).
vr::HmdMatrix34_t
EyeToHead(vr::IVRSystem *system, vr::EVREye eye)
{
	using Fn = vr::HmdMatrix34_t *(*)(vr::IVRSystem *, vr::HmdMatrix34_t *, vr::EVREye);
	Fn fn = reinterpret_cast<Fn *>(*reinterpret_cast<void ***>(system))[5];
	vr::HmdMatrix34_t result = {};
	fn(system, &result, eye);
	return result;
}

vr::HmdMatrix34_t
Multiply(const vr::HmdMatrix34_t &a, const vr::HmdMatrix34_t &b)
{
	vr::HmdMatrix34_t r = {};
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 4; ++j) {
			r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + (j == 3 ? a.m[i][3] : 0);
		}
	}
	return r;
}

double
YawDegrees(const vr::HmdMatrix34_t &m)
{
	return atan2(-m.m[0][2], m.m[2][2]) * 57.29578;
}

double
PitchDegrees(const vr::HmdMatrix34_t &m)
{
	return asin(fmax(-1.0, fmin(1.0, -m.m[1][2]))) * 57.29578;
}

// Angle of the rotation between two poses, in degrees.
double
AngleBetween(const vr::HmdMatrix34_t &a, const vr::HmdMatrix34_t &b)
{
	double trace = 0;
	for (int i = 0; i < 3; ++i) {
		for (int k = 0; k < 3; ++k) {
			trace += a.m[k][i] * b.m[k][i];
		}
	}
	return acos(fmax(-1.0, fmin(1.0, (trace - 1) / 2))) * 57.29578;
}

double
Distance(const vr::HmdMatrix34_t &a, const vr::HmdMatrix34_t &b)
{
	double dx = a.m[0][3] - b.m[0][3], dy = a.m[1][3] - b.m[1][3], dz = a.m[2][3] - b.m[2][3];
	return sqrt(dx * dx + dy * dy + dz * dz);
}

const char *
EventName(uint32_t type)
{
	switch (type) {
	case vr::VREvent_InputFocusCaptured: return "InputFocusCaptured";
	case vr::VREvent_InputFocusReleased: return "InputFocusReleased";
	case vr::VREvent_SceneApplicationChanged: return "SceneApplicationChanged";
	case vr::VREvent_SceneApplicationStateChanged: return "SceneApplicationStateChanged";
	case vr::VREvent_TrackedDeviceUserInteractionStarted: return "UserInteractionStarted";
	case vr::VREvent_TrackedDeviceUserInteractionEnded: return "UserInteractionEnded";
	case vr::VREvent_TrackedDeviceActivated: return "TrackedDeviceActivated";
	case vr::VREvent_TrackedDeviceUpdated: return "TrackedDeviceUpdated";
	case vr::VREvent_ButtonPress: return "ButtonPress";
	case vr::VREvent_ButtonUnpress: return "ButtonUnpress";
	case vr::VREvent_DashboardActivated: return "DashboardActivated";
	case vr::VREvent_DashboardDeactivated: return "DashboardDeactivated";
	case vr::VREvent_ChaperoneUniverseHasChanged: return "ChaperoneUniverseHasChanged";
	case vr::VREvent_StandingZeroPoseReset: return "StandingZeroPoseReset";
	case vr::VREvent_SeatedZeroPoseReset: return "SeatedZeroPoseReset";
	case vr::VREvent_Quit: return "Quit";
	default: return nullptr;
	}
}

} // namespace

int
main(int argc, char **argv)
{
	double seconds = 120;
	bool explicitTiming = false;
	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
			seconds = atof(argv[++i]);
		} else if (!strcmp(argv[i], "--explicit-timing")) {
			explicitTiming = true;
		} else if (!strcmp(argv[i], "--log") && i + 1 < argc) {
			g_log = fopen(argv[++i], "w");
		}
	}

	vr::EVRInitError initError = vr::VRInitError_None;
	vr::IVRSystem *system = vr::VR_Init(&initError, vr::VRApplication_Scene);
	if (!system) {
		Log("VR_Init failed: %d %s\n", initError, vr::VR_GetVRInitErrorAsEnglishDescription(initError));
		return 1;
	}
	vr::IVRCompositor *compositor = vr::VRCompositor();
	if (!compositor) {
		Log("No IVRCompositor\n");
		vr::VR_Shutdown();
		return 1;
	}

	uint32_t width = 0, height = 0;
	system->GetRecommendedRenderTargetSize(&width, &height);
	EyeConstants eyes[2] = {};
	vr::HmdMatrix34_t eyeToHead[2];
	for (int e = 0; e < 2; ++e) {
		float l, r, t, b;
		system->GetProjectionRaw((vr::EVREye)e, &l, &r, &t, &b);
		eyes[e].tangents[0] = l;
		eyes[e].tangents[1] = r;
		eyes[e].tangents[2] = t;
		eyes[e].tangents[3] = b;
		eyeToHead[e] = EyeToHead(system, (vr::EVREye)e);
		Log("Eye %d: raw projection L %.3f R %.3f T %.3f B %.3f, eye-to-head offset %.4f %.4f %.4f\n", e, l, r, t, b,
		    eyeToHead[e].m[0][3], eyeToHead[e].m[1][3], eyeToHead[e].m[2][3]);
	}
	float frequency = system->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float);
	Log("Render target %ux%u per eye, display %.1f Hz, timing %s\n", width, height, frequency,
	    explicitTiming ? "explicit" : "implicit");

	ID3D11Device *device = nullptr;
	ID3D11DeviceContext *context = nullptr;
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
	                             nullptr, &context))) {
		Log("D3D11CreateDevice failed\n");
		vr::VR_Shutdown();
		return 1;
	}
	ID3DBlob *vsBlob = nullptr, *psBlob = nullptr, *errors = nullptr;
	if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "probe", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsBlob,
	                      &errors)) ||
	    FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "probe", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psBlob,
	                      &errors))) {
		Log("Shader compile failed: %s\n", errors ? (const char *)errors->GetBufferPointer() : "?");
		vr::VR_Shutdown();
		return 1;
	}
	ID3D11VertexShader *vs = nullptr;
	ID3D11PixelShader *ps = nullptr;
	device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
	device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);
	D3D11_BUFFER_DESC cbDesc = {sizeof(EyeConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
	ID3D11Buffer *constants = nullptr;
	device->CreateBuffer(&cbDesc, nullptr, &constants);
	ID3D11Texture2D *textures[2] = {};
	ID3D11RenderTargetView *targets[2] = {};
	for (int e = 0; e < 2; ++e) {
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		device->CreateTexture2D(&desc, nullptr, &textures[e]);
		device->CreateRenderTargetView(textures[e], nullptr, &targets[e]);
	}
	if (explicitTiming) {
		compositor->SetExplicitTimingMode(vr::VRCompositorTimingMode_Explicit_RuntimePerformsPostPresentHandoff);
	}

	using Clock = std::chrono::steady_clock;
	auto start = Clock::now(), windowStart = start;
	uint32_t frames = 0, waitErrors = 0, submitErrors = 0, invalidHead = 0, lastWaitError = 0, lastSubmitError = 0;
	double waitMsTotal = 0, waitMsMax = 0;
	bool quit = false;
	vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
	while (!quit && std::chrono::duration<double>(Clock::now() - start).count() < seconds) {
		vr::VREvent_t event;
		while (system->PollNextEvent(&event, sizeof(event))) {
			const char *name = EventName(event.eventType);
			if (name) {
				Log("Event %s (device %u)\n", name, event.trackedDeviceIndex);
			}
			if (event.eventType == vr::VREvent_Quit) {
				system->AcknowledgeQuit_Exiting();
				quit = true;
			}
		}

		auto waitBegin = Clock::now();
		vr::EVRCompositorError error = compositor->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
		double waitMs = std::chrono::duration<double, std::milli>(Clock::now() - waitBegin).count();
		waitMsTotal += waitMs;
		waitMsMax = fmax(waitMsMax, waitMs);
		if (error != vr::VRCompositorError_None) {
			++waitErrors;
			lastWaitError = error;
		}
		const vr::TrackedDevicePose_t &head = poses[vr::k_unTrackedDeviceIndex_Hmd];
		if (!head.bPoseIsValid) {
			++invalidHead;
		}
		if (explicitTiming) {
			compositor->SubmitExplicitTimingData();
		}
		for (int e = 0; e < 2; ++e) {
			vr::HmdMatrix34_t eyeToWorld = Multiply(head.mDeviceToAbsoluteTracking, eyeToHead[e]);
			memcpy(eyes[e].rows, eyeToWorld.m, sizeof(eyes[e].rows));
			context->UpdateSubresource(constants, 0, nullptr, &eyes[e], 0, 0);
			D3D11_VIEWPORT viewport = {0, 0, (float)width, (float)height, 0, 1};
			context->OMSetRenderTargets(1, &targets[e], nullptr);
			context->RSSetViewports(1, &viewport);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context->IASetInputLayout(nullptr);
			context->VSSetShader(vs, nullptr, 0);
			context->PSSetShader(ps, nullptr, 0);
			context->PSSetConstantBuffers(0, 1, &constants);
			context->Draw(3, 0);
			vr::Texture_t texture = {textures[e], vr::TextureType_DirectX, vr::ColorSpace_Gamma};
			vr::EVRCompositorError submit = compositor->Submit((vr::EVREye)e, &texture);
			if (submit != vr::VRCompositorError_None) {
				++submitErrors;
				lastSubmitError = submit;
			}
		}
		++frames;

		double elapsed = std::chrono::duration<double>(Clock::now() - windowStart).count();
		if (elapsed < 1.0) {
			continue;
		}
		// Once a second: what an engine predicting from the driver's vsync
		// timing would get, compared with the compositor's render pose.
		float sinceVsync = 0;
		uint64_t vsyncCounter = 0;
		bool haveVsync = system->GetTimeSinceLastVsync(&sinceVsync, &vsyncCounter);
		float hz = system->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float);
		float toPhotons = system->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
		                                                        vr::Prop_SecondsFromVsyncToPhotons_Float);
		float predict = hz > 0 ? 1.0f / hz - sinceVsync + toPhotons : 0;
		vr::TrackedDevicePose_t predicted = {};
		system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, predict, &predicted, 1);
		vr::TrackedDevicePose_t now = {};
		system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &now, 1);
		vr::Compositor_FrameTiming timing = {};
		timing.m_nSize = sizeof(timing);
		bool haveTiming = compositor->GetFrameTiming(&timing, 0);
		const vr::HmdMatrix34_t &render = head.mDeviceToAbsoluteTracking;
		Log("t %5.1f s: %3.0f fps, wait avg %.2f max %.2f ms, wait errors %u (last %u), submit errors %u (last %u), "
		    "invalid head %u\n",
		    std::chrono::duration<double>(Clock::now() - start).count(), frames / elapsed, waitMsTotal / frames,
		    waitMsMax, waitErrors, lastWaitError, submitErrors, lastSubmitError, invalidHead);
		Log("  head: pos %.3f %.3f %.3f yaw %.1f pitch %.1f valid %d result %d; input %d pause %d canRender %d\n",
		    render.m[0][3], render.m[1][3], render.m[2][3], YawDegrees(render), PitchDegrees(render), head.bPoseIsValid,
		    head.eTrackingResult, system->IsInputAvailable(), system->ShouldApplicationPause(),
		    compositor->CanRenderScene());
		Log("  vsync: %s since %.2f ms, counter %llu, %.1f Hz, to photons %.2f ms -> predict %.2f ms; "
		    "render vs predicted %.1f deg %.3f m, render vs now %.1f deg %.3f m\n",
		    haveVsync ? "ok" : "unavailable", sinceVsync * 1000, (unsigned long long)vsyncCounter, hz,
		    toPhotons * 1000, predict * 1000, AngleBetween(render, predicted.mDeviceToAbsoluteTracking),
		    Distance(render, predicted.mDeviceToAbsoluteTracking), AngleBetween(render, now.mDeviceToAbsoluteTracking),
		    Distance(render, now.mDeviceToAbsoluteTracking));
		// Which tracking spaces SteamVR considers valid, and why (once a second).
		vr::TrackedDevicePose_t spaces[3][3] = {};
		const vr::ETrackingUniverseOrigin origins[3] = {vr::TrackingUniverseStanding, vr::TrackingUniverseSeated,
		                                                vr::TrackingUniverseRawAndUncalibrated};
		for (int o = 0; o < 3; ++o) {
			system->GetDeviceToAbsoluteTrackingPose(origins[o], 0, spaces[o], 3);
		}
		vr::IVRChaperone *chaperone = vr::VRChaperone();
		Log("  validity (valid/result) standing hmd %d/%d left %d/%d right %d/%d; seated hmd %d/%d; raw hmd %d/%d; "
		    "chaperone calibration %d\n",
		    spaces[0][0].bPoseIsValid, spaces[0][0].eTrackingResult, spaces[0][1].bPoseIsValid,
		    spaces[0][1].eTrackingResult, spaces[0][2].bPoseIsValid, spaces[0][2].eTrackingResult,
		    spaces[1][0].bPoseIsValid, spaces[1][0].eTrackingResult, spaces[2][0].bPoseIsValid,
		    spaces[2][0].eTrackingResult, chaperone ? (int)chaperone->GetCalibrationState() : -1);
		if (haveTiming) {
			Log("  frame %u: presents %u mispresented %u dropped %u reprojection 0x%x, gpu %.2f ms, "
			    "compositor %.2f ms, interval %.2f ms\n",
			    timing.m_nFrameIndex, timing.m_nNumFramePresents, timing.m_nNumMisPresented,
			    timing.m_nNumDroppedFrames, timing.m_nReprojectionFlags, timing.m_flTotalRenderGpuMs,
			    timing.m_flCompositorRenderGpuMs, timing.m_flClientFrameIntervalMs);
		}
		windowStart = Clock::now();
		frames = waitErrors = submitErrors = invalidHead = 0;
		waitMsTotal = waitMsMax = 0;
	}

	Log("Exiting\n");
	vr::VR_Shutdown();
	return 0;
}

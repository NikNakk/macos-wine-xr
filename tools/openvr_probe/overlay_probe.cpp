// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
// OpenVR overlay probe: which ways of giving the compositor an overlay image
// work under Wine. Shows three solid overlays side by side, 1.5 m in front of
// the headset (head-locked), each fed differently:
//   red   SetOverlayRaw: pixels sent over SteamVR's IPC
//   green SetOverlayTexture: a D3D11 texture shared from this process (as the
//         SteamVR and Steam web helpers do)
//   blue  SetOverlayFromFile: a BMP the compositor loads itself
// With --dashboard it also asks SteamVR to show its dashboard after 10 s.
//
//   overlay_probe.exe [--seconds N] [--dashboard] [--log FILE]
#include <windows.h>

#include <d3d11.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "third_party/openvr/openvr.h"

namespace {

FILE *g_log = nullptr;

void
Log(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
	if (g_log) {
		va_start(args, format);
		vfprintf(g_log, format, args);
		va_end(args);
		fflush(g_log);
	}
	fflush(stdout);
}

const char *
OverlayError(vr::EVROverlayError error)
{
	return vr::VROverlay() ? vr::VROverlay()->GetOverlayErrorNameFromEnum(error) : "?";
}

// A 64x64 24-bit BMP of one colour, for SetOverlayFromFile.
bool
WriteBmp(const char *path, uint8_t r, uint8_t g, uint8_t b)
{
	const int size = 64, row = size * 3, image = row * size;
	BITMAPFILEHEADER file = {};
	BITMAPINFOHEADER info = {};
	file.bfType = 0x4d42;
	file.bfOffBits = sizeof(file) + sizeof(info);
	file.bfSize = file.bfOffBits + image;
	info.biSize = sizeof(info);
	info.biWidth = size;
	info.biHeight = size;
	info.biPlanes = 1;
	info.biBitCount = 24;
	info.biCompression = BI_RGB;
	FILE *f = fopen(path, "wb");
	if (!f) {
		return false;
	}
	fwrite(&file, sizeof(file), 1, f);
	fwrite(&info, sizeof(info), 1, f);
	std::vector<uint8_t> pixels(image);
	for (int i = 0; i < size * size; ++i) {
		pixels[i * 3 + 0] = b;
		pixels[i * 3 + 1] = g;
		pixels[i * 3 + 2] = r;
	}
	fwrite(pixels.data(), 1, pixels.size(), f);
	fclose(f);
	return true;
}

vr::VROverlayHandle_t
MakeOverlay(const char *key, const char *name, float x)
{
	vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
	vr::EVROverlayError error = vr::VROverlay()->CreateOverlay(key, name, &handle);
	Log("CreateOverlay %s: %s\n", key, OverlayError(error));
	if (error != vr::VROverlayError_None) {
		return vr::k_ulOverlayHandleInvalid;
	}
	vr::VROverlay()->SetOverlayWidthInMeters(handle, 0.4f);
	vr::HmdMatrix34_t transform = {{{1, 0, 0, x}, {0, 1, 0, 0}, {0, 0, 1, -1.5f}}};
	vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(handle, vr::k_unTrackedDeviceIndex_Hmd, &transform);
	return handle;
}

} // namespace

int
main(int argc, char **argv)
{
	double seconds = 60;
	bool dashboard = false;
	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
			seconds = atof(argv[++i]);
		} else if (!strcmp(argv[i], "--dashboard")) {
			dashboard = true;
		} else if (!strcmp(argv[i], "--log") && i + 1 < argc) {
			g_log = fopen(argv[++i], "w");
		}
	}

	vr::EVRInitError initError = vr::VRInitError_None;
	vr::VR_Init(&initError, vr::VRApplication_Overlay);
	if (initError != vr::VRInitError_None) {
		Log("VR_Init failed: %d %s\n", initError, vr::VR_GetVRInitErrorAsEnglishDescription(initError));
		return 1;
	}
	vr::IVROverlay *overlay = vr::VROverlay();

	vr::VROverlayHandle_t raw = MakeOverlay("mwxr.probe.raw", "probe raw (red)", -0.5f);
	vr::VROverlayHandle_t texture = MakeOverlay("mwxr.probe.texture", "probe texture (green)", 0.0f);
	vr::VROverlayHandle_t file = MakeOverlay("mwxr.probe.file", "probe file (blue)", 0.5f);

	// Red: raw RGBA pixels.
	std::vector<uint8_t> pixels(64 * 64 * 4);
	for (size_t i = 0; i < pixels.size(); i += 4) {
		pixels[i + 0] = 255;
		pixels[i + 3] = 255;
	}
	if (raw != vr::k_ulOverlayHandleInvalid) {
		Log("SetOverlayRaw: %s\n", OverlayError(overlay->SetOverlayRaw(raw, pixels.data(), 64, 64, 4)));
		Log("ShowOverlay raw: %s\n", OverlayError(overlay->ShowOverlay(raw)));
	}

	// Magenta: raw pixels, placed in the room (standing space) rather than on the
	// headset, so it does not depend on the compositor's HMD pose.
	vr::VROverlayHandle_t absolute = vr::k_ulOverlayHandleInvalid;
	if (overlay->CreateOverlay("mwxr.probe.absolute", "probe absolute (magenta)", &absolute) ==
	    vr::VROverlayError_None) {
		std::vector<uint8_t> magenta(64 * 64 * 4);
		for (size_t i = 0; i < magenta.size(); i += 4) {
			magenta[i + 0] = magenta[i + 2] = magenta[i + 3] = 255;
		}
		overlay->SetOverlayWidthInMeters(absolute, 1.0f);
		vr::HmdMatrix34_t place = {{{1, 0, 0, 0}, {0, 1, 0, 1.5f}, {0, 0, 1, -1.5f}}};
		overlay->SetOverlayTransformAbsolute(absolute, vr::TrackingUniverseStanding, &place);
		Log("SetOverlayRaw absolute: %s\n", OverlayError(overlay->SetOverlayRaw(absolute, magenta.data(), 64, 64, 4)));
		Log("ShowOverlay absolute: %s\n", OverlayError(overlay->ShowOverlay(absolute)));
	}

	// Green: a shared D3D11 texture.
	ID3D11Device *device = nullptr;
	ID3D11DeviceContext *context = nullptr;
	ID3D11Texture2D *green = nullptr;
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
	                             &device, nullptr, &context))) {
		Log("D3D11CreateDevice failed\n");
	} else {
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = desc.Height = 64;
		desc.MipLevels = desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
		std::vector<uint8_t> greenPixels(64 * 64 * 4);
		for (size_t i = 0; i < greenPixels.size(); i += 4) {
			greenPixels[i + 1] = 255;
			greenPixels[i + 3] = 255;
		}
		D3D11_SUBRESOURCE_DATA data = {greenPixels.data(), 64 * 4, 0};
		HRESULT hr = device->CreateTexture2D(&desc, &data, &green);
		Log("CreateTexture2D (shared): 0x%08lx\n", (unsigned long)hr);
		if (SUCCEEDED(hr) && texture != vr::k_ulOverlayHandleInvalid) {
			context->Flush();
			vr::Texture_t t = {green, vr::TextureType_DirectX, vr::ColorSpace_Auto};
			Log("SetOverlayTexture: %s\n", OverlayError(overlay->SetOverlayTexture(texture, &t)));
			Log("ShowOverlay texture: %s\n", OverlayError(overlay->ShowOverlay(texture)));
		}
	}

	// Blue: a file the compositor loads.
	char path[MAX_PATH];
	GetTempPathA(sizeof(path), path);
	strncat(path, "mwxr_overlay_probe_blue.bmp", sizeof(path) - strlen(path) - 1);
	if (file != vr::k_ulOverlayHandleInvalid && WriteBmp(path, 0, 0, 255)) {
		Log("SetOverlayFromFile %s: %s\n", path, OverlayError(overlay->SetOverlayFromFile(file, path)));
		Log("ShowOverlay file: %s\n", OverlayError(overlay->ShowOverlay(file)));
	}

	using Clock = std::chrono::steady_clock;
	auto start = Clock::now(), lastReport = start;
	bool askedDashboard = false;
	while (std::chrono::duration<double>(Clock::now() - start).count() < seconds) {
		vr::VREvent_t event;
		while (vr::VRSystem()->PollNextEvent(&event, sizeof(event))) {
			if (event.eventType == vr::VREvent_Quit) {
				seconds = 0;
			}
		}
		double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
		if (dashboard && !askedDashboard && elapsed > 10) {
			askedDashboard = true;
			overlay->ShowDashboard(nullptr);
			Log("ShowDashboard requested; dashboard visible: %d\n", overlay->IsDashboardVisible());
		}
		if (std::chrono::duration<double>(Clock::now() - lastReport).count() >= 5) {
			lastReport = Clock::now();
			Log("t %.0f s: visible raw %d texture %d file %d, dashboard %d\n", elapsed,
			    raw != vr::k_ulOverlayHandleInvalid && overlay->IsOverlayVisible(raw),
			    texture != vr::k_ulOverlayHandleInvalid && overlay->IsOverlayVisible(texture),
			    file != vr::k_ulOverlayHandleInvalid && overlay->IsOverlayVisible(file),
			    overlay->IsDashboardVisible());
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	Log("Exiting\n");
	vr::VR_Shutdown();
	return 0;
}

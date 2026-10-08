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
// With --cef it adds yellow overlays below, fed as SteamVR's web helper feeds
// CEF's frames: a texture shared by NT handle, opened on a second device with
// OpenSharedResource1, then given to SetOverlayTexture.
//
// --app-type N connects as that EVRApplicationType (SteamVR's web helper is
// VRApplication_WebHelper, 8) instead of VRApplication_Overlay.
//
// --overlay-029 gives the --cef textures to IVROverlay_029's SetOverlayTexture
// (vtable slot 61, as the web helper calls it) instead of IVROverlay_028's.
//
//   overlay_probe.exe [--seconds N] [--dashboard] [--cef] [--app-type N] [--overlay-029] [--log FILE]
#include <windows.h>

#include <d3d10.h>
#include <d3d11_1.h>

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
	bool cef = false;
	int appType = vr::VRApplication_Overlay;
	bool overlay029 = false;
	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
			seconds = atof(argv[++i]);
		} else if (!strcmp(argv[i], "--dashboard")) {
			dashboard = true;
		} else if (!strcmp(argv[i], "--cef")) {
			cef = true;
		} else if (!strcmp(argv[i], "--overlay-029")) {
			overlay029 = true;
		} else if (!strcmp(argv[i], "--app-type") && i + 1 < argc) {
			appType = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--log") && i + 1 < argc) {
			g_log = fopen(argv[++i], "w");
		}
	}

	vr::EVRInitError initError = vr::VRInitError_None;
	vr::VR_Init(&initError, (vr::EVRApplicationType)appType);
	if (initError != vr::VRInitError_None) {
		Log("VR_Init failed: %d %s\n", initError, vr::VR_GetVRInitErrorAsEnglishDescription(initError));
		return 1;
	}
	vr::IVROverlay *overlay = vr::VROverlay();
	if (!overlay) {
		Log("No IVROverlay (is the compositor running?)\n");
		vr::VR_Shutdown();
		return 1;
	}

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

	// Yellow: CEF-style frames (see --cef above), one overlay per sharing flag set.
	struct CefVariant
	{
		const char *name;
		UINT miscFlags;
		bool reopen;
		UINT width, height; // CEF's dashboard frames are 1860x2048
	};
	const CefVariant cefVariants[] = {
	    {"nt+keyedmutex reopened", D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX, true, 64, 64},
	    {"nt+shared reopened", D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED, true, 64, 64},
	    {"nt+shared direct", D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED, false, 64, 64},
	    {"nt+shared reopened 1860x2048", D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED, true, 1860, 2048},
	};
	ID3D11Device1 *consumer = nullptr;
	if (cef && device) {
		ID3D11Device *base = nullptr;
		if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
		                                D3D11_SDK_VERSION, &base, nullptr, nullptr))) {
			base->QueryInterface(__uuidof(ID3D11Device1), (void **)&consumer);
			base->Release();
		}
		Log("Consumer ID3D11Device1: %p\n", (void *)consumer);
	}
	for (size_t v = 0; cef && device && consumer && v < sizeof(cefVariants) / sizeof(cefVariants[0]); ++v) {
		const CefVariant &variant = cefVariants[v];
		char key[64];
		snprintf(key, sizeof(key), "mwxr.probe.cef%zu", v);
		vr::VROverlayHandle_t handle = MakeOverlay(key, variant.name, -0.5f + 0.5f * v);
		if (handle == vr::k_ulOverlayHandleInvalid) {
			continue;
		}
		vr::HmdMatrix34_t below = {{{1, 0, 0, -0.5f + 0.5f * v}, {0, 1, 0, -0.5f}, {0, 0, 1, -1.5f}}};
		overlay->SetOverlayTransformTrackedDeviceRelative(handle, vr::k_unTrackedDeviceIndex_Hmd, &below);
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = variant.width;
		desc.Height = variant.height;
		desc.MipLevels = desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // CEF's frame format
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		desc.MiscFlags = variant.miscFlags;
		std::vector<uint8_t> yellow((size_t)variant.width * variant.height * 4);
		for (size_t i = 0; i < yellow.size(); i += 4) {
			yellow[i + 1] = yellow[i + 2] = yellow[i + 3] = 255; // B G R A
		}
		D3D11_SUBRESOURCE_DATA data = {yellow.data(), variant.width * 4, 0};
		ID3D11Texture2D *produced = nullptr;
		HRESULT hr = device->CreateTexture2D(&desc, &data, &produced);
		Log("cef %s: CreateTexture2D 0x%08lx\n", variant.name, (unsigned long)hr);
		if (FAILED(hr)) {
			continue;
		}
		ID3D11Texture2D *given = produced;
		if (variant.reopen) {
			IDXGIResource1 *resource = nullptr;
			HANDLE nt = nullptr;
			hr = produced->QueryInterface(__uuidof(IDXGIResource1), (void **)&resource);
			if (SUCCEEDED(hr)) {
				hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
				                                  nullptr, &nt);
				resource->Release();
			}
			Log("cef %s: CreateSharedHandle 0x%08lx handle %p\n", variant.name, (unsigned long)hr, nt);
			given = nullptr;
			if (SUCCEEDED(hr)) {
				hr = consumer->OpenSharedResource1(nt, __uuidof(ID3D11Texture2D), (void **)&given);
				Log("cef %s: OpenSharedResource1 0x%08lx\n", variant.name, (unsigned long)hr);
			}
			if (!given) {
				continue;
			}
			D3D11_TEXTURE2D_DESC opened;
			given->GetDesc(&opened);
			Log("cef %s: opened %ux%u format %d bind 0x%x misc 0x%x\n", variant.name, opened.Width,
			    opened.Height, opened.Format, opened.BindFlags, opened.MiscFlags);
		}
		{
			// vrclient picks its texture path by QueryInterface: ID3D10Texture2D with a device first.
			ID3D10Texture2D *d3d10 = nullptr;
			ID3D10Device *d3d10Device = nullptr;
			HRESULT qi = given->QueryInterface(__uuidof(ID3D10Texture2D), (void **)&d3d10);
			if (SUCCEEDED(qi)) {
				d3d10->GetDevice(&d3d10Device);
				d3d10->Release();
			}
			Log("cef %s: QI ID3D10Texture2D 0x%08lx, its device %p\n", variant.name, (unsigned long)qi,
			    (void *)d3d10Device);
			if (d3d10Device) {
				d3d10Device->Release();
			}
		}
		vr::Texture_t t = {given, vr::TextureType_DirectX, vr::ColorSpace_Auto};
		vr::EVROverlayError result;
		if (overlay029) {
			using SetTexture = vr::EVROverlayError(__thiscall *)(void *, vr::VROverlayHandle_t, const vr::Texture_t *);
			vr::EVRInitError error = vr::VRInitError_None;
			void *latest = vr::VR_GetGenericInterface("IVROverlay_029", &error);
			if (latest && v == 0) {
				Log("IVROverlay_029 %p, SetOverlayTexture %p\n", latest, (void *)(*(SetTexture **)latest)[61]);
			}
			result = latest ? (*(SetTexture **)latest)[61](latest, handle, &t) : vr::VROverlayError_RequestFailed;
		} else {
			result = overlay->SetOverlayTexture(handle, &t);
		}
		Log("cef %s: SetOverlayTexture%s %s\n", variant.name, overlay029 ? " (029)" : "", OverlayError(result));
		Log("cef %s: ShowOverlay %s\n", variant.name, OverlayError(overlay->ShowOverlay(handle)));
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

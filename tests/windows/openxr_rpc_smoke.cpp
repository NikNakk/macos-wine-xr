// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#define XR_NO_PROTOTYPES
#include <cstdio>
#include <cstring>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <windows.h>
#define CHECK(call)                                                                                                    \
	do {                                                                                                           \
		XrResult x = (call);                                                                                   \
		if (XR_FAILED(x)) {                                                                                    \
			fprintf(stderr, "%s: %d\n", #call, x);                                                         \
			return 1;                                                                                      \
		}                                                                                                      \
	} while (0)
int main(int argc, char **argv) {
	if (argc != 2)
		return 2;
	HMODULE dll = LoadLibraryA(argv[1]);
	if (!dll) {
		fprintf(stderr, "LoadLibrary=%lu\n", GetLastError());
		return 1;
	}
	auto negotiate =
	    (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(dll, "xrNegotiateLoaderRuntimeInterface");
	XrNegotiateLoaderInfo li = {XR_LOADER_INTERFACE_STRUCT_LOADER_INFO,
				    1,
				    sizeof(li),
				    1,
				    1,
				    XR_MAKE_VERSION(1, 0, 0),
				    XR_MAKE_VERSION(1, 0, 0)};
	XrNegotiateRuntimeRequest ri = {XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, 1, sizeof(ri)};
	if (!negotiate)
		return 1;
	CHECK(negotiate(&li, &ri));
	PFN_xrCreateInstance create;
	CHECK(ri.getInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&create));
	XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
	strcpy(ci.applicationInfo.applicationName, "generic RPC smoke");
	ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	XrInstance instance;
	CHECK(create(&ci, &instance));
#define LOAD(name)                                                                                                     \
	PFN_xr##name name;                                                                                             \
	CHECK(ri.getInstanceProcAddr(instance, "xr" #name, (PFN_xrVoidFunction *)&name))
	LOAD(GetInstanceProperties);
	LOAD(GetSystem);
	LOAD(GetSystemProperties);
	LOAD(EnumerateViewConfigurationViews);
	LOAD(DestroyInstance);
	XrInstanceProperties ip = {XR_TYPE_INSTANCE_PROPERTIES};
	CHECK(GetInstanceProperties(instance, &ip));
	XrSystemGetInfo si = {XR_TYPE_SYSTEM_GET_INFO};
	si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId system;
	CHECK(GetSystem(instance, &si, &system));
	XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
	CHECK(GetSystemProperties(instance, system, &sp));
	XrViewConfigurationView views[4] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW},
					    {XR_TYPE_VIEW_CONFIGURATION_VIEW},
					    {XR_TYPE_VIEW_CONFIGURATION_VIEW},
					    {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
	uint32_t count;
	CHECK(EnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 4, &count,
					      views));
	printf("RPC runtime=%s system=%s views=%u recommended=%ux%u\n", ip.runtimeName, sp.systemName, count,
	       views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight);
	CHECK(DestroyInstance(instance));
	FreeLibrary(dll);
	return 0;
}

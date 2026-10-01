// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#define WIN32_LEAN_AND_MEAN
#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include "dxmt_native_interop.h"
#include "transport.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d11_4.h>
#include <dxgi.h>
#include <mutex>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_reflection.h>
#include <unordered_map>
#include <vector>
#include <windows.h>

// Local objects contain only remote IDs and graphics wrappers. All OpenXR
// tracking, actions, frame pacing and composition live in the native runtime.
enum Kind { INSTANCE, SESSION, SPACE, SWAPCHAIN, ACTION_SET, ACTION };
struct Local {
	Kind kind;
	uint64_t id;
	Local *parent;
	ID3D11Device *device = nullptr;
	IDXMTNativeDevice *native = nullptr;
	ID3D11Fence *fence = nullptr;
	uint64_t fence_value = 0;
	std::vector<ID3D11Texture2D *> textures;
	Local(Kind k, uint64_t n, Local *p) : kind(k), id(n), parent(p) {
	}
	~Local() {
		for (auto t : textures)
			t->Release();
		if (fence)
			fence->Release();
		if (native)
			native->Release();
		if (device)
			device->Release();
	}
};
static std::recursive_mutex mutex;
static std::unordered_map<void *, Local *> objects;
static mwxr_rpc_client client = {MWXR_INVALID_SOCKET, 0};
static bool d3d_enabled = false;
#define LOCK std::lock_guard<std::recursive_mutex> guard(mutex)
#define INFO(p, t)                                                                                                     \
	do {                                                                                                           \
		if (!(p) || (p)->type != (t))                                                                          \
			return XR_ERROR_VALIDATION_FAILURE;                                                            \
		if ((p)->next)                                                                                         \
			return XR_ERROR_FEATURE_UNSUPPORTED;                                                           \
	} while (0)
#define OUTPUT(p, t) INFO(p, t)
static Local *lookup(const void *p, Kind k) {
	auto i = objects.find((void *)p);
	return i == objects.end() || i->second->kind != k ? nullptr : i->second;
}
static Local *make(Kind k, uint64_t id, Local *parent) {
	auto p = new Local(k, id, parent);
	objects[p] = p;
	return p;
}
static void erase(Local *p) {
	std::vector<Local *> children;
	for (auto entry : objects)
		if (entry.second->parent == p)
			children.push_back(entry.second);
	for (auto c : children)
		erase(c);
	objects.erase(p);
	delete p;
}
static XrResult rpc(uint32_t op, const mwxr_wire_request &q, mwxr_wire_response &r) {
	int32_t result;
	if (mwxr_rpc_call(&client, op, &q, &r, &result)) {
		mwxr_rpc_close(client.socket);
		client.socket = MWXR_INVALID_SOCKET;
		return XR_ERROR_INSTANCE_LOST;
	}
	if (r.count > 64 || !memchr(r.name, 0, sizeof(r.name)) || !memchr(r.label, 0, sizeof(r.label)) ||
	    ((op == MWXR_OP_ENUM_VIEWS || op == MWXR_OP_LOCATE_VIEWS) && r.count > 4) ||
	    (op == MWXR_OP_CREATE_SWAPCHAIN && r.count > 16))
		return XR_ERROR_RUNTIME_FAILURE;
	if (op == MWXR_OP_CREATE_SWAPCHAIN)
		for (unsigned i = 0; i < r.count; i++)
			if (!memchr(r.images[i].name, 0, sizeof(r.images[i].name)))
				return XR_ERROR_RUNTIME_FAILURE;
	return (XrResult)result;
}
static mwxr_wire_pose pose(XrPosef p) {
	mwxr_wire_pose w;
	memcpy(w.orientation, &p.orientation, sizeof(w.orientation));
	memcpy(w.position, &p.position, sizeof(w.position));
	return w;
}
static XrPosef pose(mwxr_wire_pose p) {
	XrPosef n;
	memcpy(&n.orientation, p.orientation, sizeof(p.orientation));
	memcpy(&n.position, p.position, sizeof(p.position));
	return n;
}
static bool text(char *dest, size_t size, const char *src) {
	if (!src || strlen(src) >= size)
		return false;
	strcpy(dest, src);
	return true;
}
static XrResult enumerate(uint32_t cap, uint32_t *count, uint32_t available, const void *src, void *dst, size_t size) {
	if (!count || (cap && !dst))
		return XR_ERROR_VALIDATION_FAILURE;
	*count = available;
	if (cap && cap < available)
		return XR_ERROR_SIZE_INSUFFICIENT;
	if (cap)
		memcpy(dst, src, available * size);
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL xrEnumerateInstanceExtensionProperties(const char *layer, uint32_t cap, uint32_t *count,
								  XrExtensionProperties *out) {
	if (layer)
		return XR_ERROR_API_LAYER_NOT_PRESENT;
	if (!count || (cap && !out))
		return XR_ERROR_VALIDATION_FAILURE;
	*count = 1;
	if (cap) {
		OUTPUT(out, XR_TYPE_EXTENSION_PROPERTIES);
		out[0].extensionVersion = XR_KHR_D3D11_enable_SPEC_VERSION;
		strcpy(out[0].extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
	}
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL xrCreateInstance(const XrInstanceCreateInfo *ci, XrInstance *out) {
	LOCK;
	INFO(ci, XR_TYPE_INSTANCE_CREATE_INFO);
	if (!out)
		return XR_ERROR_VALIDATION_FAILURE;
	*out = XR_NULL_HANDLE;
	if (!objects.empty())
		return XR_ERROR_LIMIT_REACHED;
	if (ci->createFlags)
		return XR_ERROR_VALIDATION_FAILURE;
	if (ci->enabledApiLayerCount)
		return XR_ERROR_API_LAYER_NOT_PRESENT;
	if (XR_VERSION_MAJOR(ci->applicationInfo.apiVersion) != 1 ||
	    XR_VERSION_MINOR(ci->applicationInfo.apiVersion) > 0)
		return XR_ERROR_API_VERSION_UNSUPPORTED;
	d3d_enabled = false;
	for (unsigned i = 0; i < ci->enabledExtensionCount; i++) {
		if (!ci->enabledExtensionNames || !ci->enabledExtensionNames[i])
			return XR_ERROR_VALIDATION_FAILURE;
		if (strcmp(ci->enabledExtensionNames[i], XR_KHR_D3D11_ENABLE_EXTENSION_NAME))
			return XR_ERROR_EXTENSION_NOT_PRESENT;
		d3d_enabled = true;
	}
	if (!memchr(ci->applicationInfo.applicationName, 0, sizeof(ci->applicationInfo.applicationName)) ||
	    !memchr(ci->applicationInfo.engineName, 0, sizeof(ci->applicationInfo.engineName)))
		return XR_ERROR_VALIDATION_FAILURE;
	if (mwxr_rpc_connect(&client, getenv("MWXR_RPC_PORT"), getenv("MWXR_RPC_TOKEN")))
		return XR_ERROR_INITIALIZATION_FAILED;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.a = MWXR_RPC_VERSION;
	q.aux = MWXR_RPC_FINGERPRINT;
	q.flags = ci->applicationInfo.apiVersion;
	q.b = ci->applicationInfo.applicationVersion;
	q.c = ci->applicationInfo.engineVersion;
	text(q.name, sizeof(q.name), ci->applicationInfo.applicationName);
	text(q.label, sizeof(q.label), ci->applicationInfo.engineName);
	XrResult x = rpc(MWXR_OP_HELLO, q, r);
	if (XR_SUCCEEDED(x) && (r.a != MWXR_RPC_VERSION || r.aux != MWXR_RPC_FINGERPRINT))
		x = XR_ERROR_INITIALIZATION_FAILED;
	if (XR_FAILED(x)) {
		mwxr_rpc_close(client.socket);
		client.socket = MWXR_INVALID_SOCKET;
		return x;
	}
	*out = (XrInstance)make(INSTANCE, r.object, nullptr);
	return x;
}
static XrResult XRAPI_CALL xrDestroyInstance(XrInstance instance) {
	LOCK;
	auto p = lookup(instance, INSTANCE);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	mwxr_rpc_close(client.socket);
	client.socket = MWXR_INVALID_SOCKET;
	erase(p);
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL xrGetInstanceProperties(XrInstance instance, XrInstanceProperties *out) {
	LOCK;
	OUTPUT(out, XR_TYPE_INSTANCE_PROPERTIES);
	auto p = lookup(instance, INSTANCE);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	XrResult x = rpc(MWXR_OP_PROPERTIES, q, r);
	if (XR_SUCCEEDED(x)) {
		out->runtimeVersion = r.version;
		snprintf(out->runtimeName, sizeof(out->runtimeName), "%s", r.name);
	}
	return x;
}
static XrResult XRAPI_CALL xrGetSystem(XrInstance instance, const XrSystemGetInfo *ci, XrSystemId *out) {
	LOCK;
	INFO(ci, XR_TYPE_SYSTEM_GET_INFO);
	if (!out)
		return XR_ERROR_VALIDATION_FAILURE;
	auto p = lookup(instance, INSTANCE);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	q.a = ci->formFactor;
	XrResult x = rpc(MWXR_OP_GET_SYSTEM, q, r);
	if (XR_SUCCEEDED(x))
		*out = r.object;
	return x;
}
static XrResult XRAPI_CALL xrGetSystemProperties(XrInstance instance, XrSystemId system, XrSystemProperties *out) {
	LOCK;
	OUTPUT(out, XR_TYPE_SYSTEM_PROPERTIES);
	auto p = lookup(instance, INSTANCE);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	if (system != 2)
		return XR_ERROR_SYSTEM_INVALID;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	XrResult x = rpc(MWXR_OP_PROPERTIES, q, r);
	if (XR_SUCCEEDED(x)) {
		out->systemId = system;
		out->vendorId = r.a;
		snprintf(out->systemName, sizeof(out->systemName), "%s", r.label);
		out->graphicsProperties = {r.c, r.b, r.d};
		out->trackingProperties = {(XrBool32)(r.flags & 1), (XrBool32)((r.flags >> 1) & 1)};
	}
	return x;
}
static XrResult XRAPI_CALL xrGetD3D11GraphicsRequirementsKHR(XrInstance instance, XrSystemId system,
							     XrGraphicsRequirementsD3D11KHR *out) {
	LOCK;
	OUTPUT(out, XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR);
	if (!lookup(instance, INSTANCE))
		return XR_ERROR_HANDLE_INVALID;
	if (!d3d_enabled)
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	if (system != 2)
		return XR_ERROR_SYSTEM_INVALID;
	IDXGIFactory *factory = nullptr;
	IDXGIAdapter *adapter = nullptr;
	DXGI_ADAPTER_DESC desc = {};
	HRESULT hr = CreateDXGIFactory(__uuidof(IDXGIFactory), (void **)&factory);
	if (SUCCEEDED(hr))
		hr = factory->EnumAdapters(0, &adapter);
	if (SUCCEEDED(hr))
		hr = adapter->GetDesc(&desc);
	if (adapter)
		adapter->Release();
	if (factory)
		factory->Release();
	if (FAILED(hr))
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	out->adapterLuid = desc.AdapterLuid;
	out->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL xrCreateSession(XrInstance instance, const XrSessionCreateInfo *ci, XrSession *out) {
	LOCK;
	if (!ci || ci->type != XR_TYPE_SESSION_CREATE_INFO || !out)
		return XR_ERROR_VALIDATION_FAILURE;
	*out = XR_NULL_HANDLE;
	auto p = lookup(instance, INSTANCE);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	if (ci->systemId != 2)
		return XR_ERROR_SYSTEM_INVALID;
	if (!d3d_enabled || !ci->next)
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	auto binding = (const XrGraphicsBindingD3D11KHR *)ci->next;
	if (binding->type != XR_TYPE_GRAPHICS_BINDING_D3D11_KHR || binding->next || !binding->device)
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	if (ci->createFlags)
		return XR_ERROR_VALIDATION_FAILURE;
	IDXMTNativeDevice *native = nullptr;
	if (FAILED(binding->device->QueryInterface(DXMT_IID_NATIVE_DEVICE, (void **)&native)))
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	q.aux = ci->systemId;
	XrResult x = rpc(MWXR_OP_CREATE_SESSION, q, r);
	if (XR_FAILED(x)) {
		native->Release();
		return x;
	}
	ID3D11Fence *fence = nullptr;
	if (FAILED(native->ImportSharedEvent(r.name, &fence))) {
		q.object = r.object;
		rpc(MWXR_OP_DESTROY_SESSION, q, r);
		native->Release();
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	}
	auto session = make(SESSION, r.object, p);
	session->native = native;
	session->device = binding->device;
	session->device->AddRef();
	session->fence = fence;
	*out = (XrSession)session;
	return XR_SUCCESS;
}
static XrResult destroy(void *handle, Kind kind, uint32_t op) {
	auto p = lookup(handle, kind);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	XrResult x = rpc(op, q, r);
	if (XR_SUCCEEDED(x))
		erase(p);
	return x;
}
static XrResult XRAPI_CALL xrDestroySession(XrSession h) {
	LOCK;
	return destroy(h, SESSION, MWXR_OP_DESTROY_SESSION);
}
static XrResult XRAPI_CALL xrDestroySpace(XrSpace h) {
	LOCK;
	return destroy(h, SPACE, MWXR_OP_DESTROY_SPACE);
}
static XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain h) {
	LOCK;
	return destroy(h, SWAPCHAIN, MWXR_OP_DESTROY_SWAPCHAIN);
}
static XrResult XRAPI_CALL xrDestroyAction(XrAction h) {
	LOCK;
	return destroy(h, ACTION, MWXR_OP_DESTROY_ACTION);
}
static XrResult XRAPI_CALL xrDestroyActionSet(XrActionSet h) {
	LOCK;
	return destroy(h, ACTION_SET, MWXR_OP_DESTROY_ACTION_SET);
}
static XrResult session_call(XrSession h, uint32_t op, uint32_t a = 0) {
	auto p = lookup(h, SESSION);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	q.a = a;
	return rpc(op, q, r);
}
static XrResult XRAPI_CALL xrBeginSession(XrSession h, const XrSessionBeginInfo *ci) {
	LOCK;
	INFO(ci, XR_TYPE_SESSION_BEGIN_INFO);
	return session_call(h, MWXR_OP_BEGIN_SESSION, ci->primaryViewConfigurationType);
}
static XrResult XRAPI_CALL xrEndSession(XrSession h) {
	LOCK;
	return session_call(h, MWXR_OP_END_SESSION);
}
static XrResult XRAPI_CALL xrRequestExitSession(XrSession h) {
	LOCK;
	return session_call(h, MWXR_OP_REQUEST_EXIT);
}
static XrResult XRAPI_CALL xrEnumerateViewConfigurations(XrInstance h, XrSystemId system, uint32_t cap, uint32_t *count,
							 XrViewConfigurationType *out) {
	LOCK;
	if (!lookup(h, INSTANCE))
		return XR_ERROR_HANDLE_INVALID;
	if (system != 2)
		return XR_ERROR_SYSTEM_INVALID;
	XrViewConfigurationType stereo = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	return enumerate(cap, count, 1, &stereo, out, sizeof(stereo));
}
static XrResult XRAPI_CALL xrGetViewConfigurationProperties(XrInstance h, XrSystemId system,
							    XrViewConfigurationType type,
							    XrViewConfigurationProperties *out) {
	LOCK;
	OUTPUT(out, XR_TYPE_VIEW_CONFIGURATION_PROPERTIES);
	if (!lookup(h, INSTANCE))
		return XR_ERROR_HANDLE_INVALID;
	if (system != 2)
		return XR_ERROR_SYSTEM_INVALID;
	if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
		return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
	out->viewConfigurationType = type;
	out->fovMutable = XR_FALSE;
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL xrEnumerateViewConfigurationViews(XrInstance h, XrSystemId system,
							     XrViewConfigurationType type, uint32_t cap,
							     uint32_t *count, XrViewConfigurationView *out) {
	LOCK;
	auto p = lookup(h, INSTANCE);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	if (system != 2)
		return XR_ERROR_SYSTEM_INVALID;
	if (!count || (cap && !out))
		return XR_ERROR_VALIDATION_FAILURE;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	q.aux = system;
	q.a = type;
	XrResult x = rpc(MWXR_OP_ENUM_VIEWS, q, r);
	if (XR_FAILED(x))
		return x;
	*count = r.count;
	if (cap && cap < r.count)
		return XR_ERROR_SIZE_INSUFFICIENT;
	if (cap)
		for (unsigned i = 0; i < r.count; i++) {
			OUTPUT(&out[i], XR_TYPE_VIEW_CONFIGURATION_VIEW);
			out[i].recommendedImageRectWidth = r.dimensions[6 * i];
			out[i].maxImageRectWidth = r.dimensions[6 * i + 1];
			out[i].recommendedImageRectHeight = r.dimensions[6 * i + 2];
			out[i].maxImageRectHeight = r.dimensions[6 * i + 3];
			out[i].recommendedSwapchainSampleCount = 1;
			out[i].maxSwapchainSampleCount = 1;
		}
	return x;
}
static XrResult XRAPI_CALL xrEnumerateEnvironmentBlendModes(XrInstance h, XrSystemId system,
							    XrViewConfigurationType type, uint32_t cap, uint32_t *count,
							    XrEnvironmentBlendMode *out) {
	LOCK;
	auto p = lookup(h, INSTANCE);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	if (system != 2)
		return XR_ERROR_SYSTEM_INVALID;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	q.aux = system;
	q.a = type;
	XrResult x = rpc(MWXR_OP_ENUM_BLEND, q, r);
	if (XR_FAILED(x))
		return x;
	XrEnvironmentBlendMode modes[64];
	for (unsigned i = 0; i < r.count; i++)
		modes[i] = (XrEnvironmentBlendMode)r.formats[i];
	return enumerate(cap, count, r.count, modes, out, sizeof(*modes));
}
static XrResult XRAPI_CALL xrEnumerateReferenceSpaces(XrSession h, uint32_t cap, uint32_t *count,
						      XrReferenceSpaceType *out) {
	LOCK;
	auto p = lookup(h, SESSION);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	XrResult x = rpc(MWXR_OP_ENUM_REFERENCE, q, r);
	if (XR_FAILED(x))
		return x;
	XrReferenceSpaceType types[64];
	for (unsigned i = 0; i < r.count; i++)
		types[i] = (XrReferenceSpaceType)r.formats[i];
	return enumerate(cap, count, r.count, types, out, sizeof(*types));
}
static XrResult XRAPI_CALL xrGetReferenceSpaceBoundsRect(XrSession h, XrReferenceSpaceType type, XrExtent2Df *out) {
	LOCK;
	if (!out)
		return XR_ERROR_VALIDATION_FAILURE;
	auto p = lookup(h, SESSION);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	q.a = type;
	XrResult x = rpc(MWXR_OP_SPACE_BOUNDS, q, r);
	if (XR_SUCCEEDED(x))
		*out = {r.values[0], r.values[1]};
	return x;
}
static XrResult XRAPI_CALL xrCreateReferenceSpace(XrSession h, const XrReferenceSpaceCreateInfo *ci, XrSpace *out) {
	LOCK;
	INFO(ci, XR_TYPE_REFERENCE_SPACE_CREATE_INFO);
	if (!out)
		return XR_ERROR_VALIDATION_FAILURE;
	auto p = lookup(h, SESSION);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	q.a = ci->referenceSpaceType;
	q.pose = pose(ci->poseInReferenceSpace);
	XrResult x = rpc(MWXR_OP_CREATE_SPACE, q, r);
	if (XR_SUCCEEDED(x))
		*out = (XrSpace)make(SPACE, r.object, p);
	return x;
}
static XrResult XRAPI_CALL xrCreateActionSpace(XrSession h, const XrActionSpaceCreateInfo *ci, XrSpace *out) {
	LOCK;
	INFO(ci, XR_TYPE_ACTION_SPACE_CREATE_INFO);
	if (!out)
		return XR_ERROR_VALIDATION_FAILURE;
	auto p = lookup(h, SESSION), a = lookup(ci->action, ACTION);
	if (!p || !a)
		return XR_ERROR_HANDLE_INVALID;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	q.aux = a->id;
	q.flags = ci->subactionPath;
	q.pose = pose(ci->poseInActionSpace);
	XrResult x = rpc(MWXR_OP_CREATE_ACTION_SPACE, q, r);
	if (XR_SUCCEEDED(x))
		*out = (XrSpace)make(SPACE, r.object, p);
	return x;
}
static XrResult XRAPI_CALL xrLocateSpace(XrSpace h, XrSpace base, XrTime time, XrSpaceLocation *out) {
	LOCK;
	OUTPUT(out, XR_TYPE_SPACE_LOCATION);
	auto p = lookup(h, SPACE), b = lookup(base, SPACE);
	if (!p || !b)
		return XR_ERROR_HANDLE_INVALID;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	q.aux = b->id;
	q.time = time;
	XrResult x = rpc(MWXR_OP_LOCATE_SPACE, q, r);
	if (XR_SUCCEEDED(x)) {
		out->locationFlags = r.flags;
		out->pose = pose(r.pose);
	}
	return x;
}
static XrResult XRAPI_CALL xrLocateViews(XrSession h, const XrViewLocateInfo *ci, XrViewState *state, uint32_t cap,
					 uint32_t *count, XrView *out) {
	LOCK;
	INFO(ci, XR_TYPE_VIEW_LOCATE_INFO);
	OUTPUT(state, XR_TYPE_VIEW_STATE);
	if (!count || (cap && !out))
		return XR_ERROR_VALIDATION_FAILURE;
	auto p = lookup(h, SESSION), s = lookup(ci->space, SPACE);
	if (!p || !s)
		return XR_ERROR_HANDLE_INVALID;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	q.aux = s->id;
	q.a = ci->viewConfigurationType;
	q.time = ci->displayTime;
	XrResult x = rpc(MWXR_OP_LOCATE_VIEWS, q, r);
	if (XR_FAILED(x))
		return x;
	*count = r.count;
	if (cap && cap < r.count)
		return XR_ERROR_SIZE_INSUFFICIENT;
	state->viewStateFlags = r.flags;
	if (cap)
		for (unsigned i = 0; i < r.count; i++) {
			OUTPUT(&out[i], XR_TYPE_VIEW);
			out[i].pose = pose(r.views[i].pose);
			memcpy(&out[i].fov, r.views[i].fov, sizeof(out[i].fov));
		}
	return x;
}
static XrResult XRAPI_CALL xrWaitFrame(XrSession h, const XrFrameWaitInfo *ci, XrFrameState *state) {
	LOCK;
	if (ci)
		INFO(ci, XR_TYPE_FRAME_WAIT_INFO);
	OUTPUT(state, XR_TYPE_FRAME_STATE);
	auto p = lookup(h, SESSION);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	XrResult x = rpc(MWXR_OP_WAIT_FRAME, q, r);
	if (XR_SUCCEEDED(x)) {
		state->predictedDisplayTime = r.time;
		state->predictedDisplayPeriod = r.period;
		state->shouldRender = r.a;
	}
	return x;
}
static XrResult XRAPI_CALL xrBeginFrame(XrSession h, const XrFrameBeginInfo *ci) {
	LOCK;
	if (ci)
		INFO(ci, XR_TYPE_FRAME_BEGIN_INFO);
	return session_call(h, MWXR_OP_BEGIN_FRAME);
}
static XrResult XRAPI_CALL xrEndFrame(XrSession h, const XrFrameEndInfo *ci) {
	LOCK;
	INFO(ci, XR_TYPE_FRAME_END_INFO);
	auto p = lookup(h, SESSION);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	if (ci->layerCount > 1)
		return XR_ERROR_FEATURE_UNSUPPORTED;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	q.time = ci->displayTime;
	q.b = ci->environmentBlendMode;
	if (ci->layerCount) {
		if (!ci->layers || !ci->layers[0] || ci->layers[0]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION)
			return XR_ERROR_LAYER_INVALID;
		auto l = (const XrCompositionLayerProjection *)ci->layers[0];
		if (l->next || l->viewCount > 4)
			return XR_ERROR_FEATURE_UNSUPPORTED;
		auto s = lookup(l->space, SPACE);
		if (!s)
			return XR_ERROR_HANDLE_INVALID;
		if (l->viewCount && !l->views)
			return XR_ERROR_VALIDATION_FAILURE;
		q.aux = s->id;
		q.flags = l->layerFlags;
		q.a = l->viewCount;
		for (unsigned i = 0; i < l->viewCount; i++) {
			INFO(&l->views[i], XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW);
			auto c = lookup(l->views[i].subImage.swapchain, SWAPCHAIN);
			if (!c || c->parent != p)
				return XR_ERROR_HANDLE_INVALID;
			auto &v = q.views[i];
			v.pose = pose(l->views[i].pose);
			memcpy(v.fov, &l->views[i].fov, sizeof(v.fov));
			v.swapchain = c->id;
			v.x = l->views[i].subImage.imageRect.offset.x;
			v.y = l->views[i].subImage.imageRect.offset.y;
			v.width = l->views[i].subImage.imageRect.extent.width;
			v.height = l->views[i].subImage.imageRect.extent.height;
			v.array_index = l->views[i].subImage.imageArrayIndex;
		}
	}
	return rpc(MWXR_OP_END_FRAME, q, r);
}
struct Format {
	int64_t metal, dxgi;
};
// Formats with identical byte layout in Metal and DXGI. Unsupported layouts
// are omitted; conversion copies are never silently added.
static const Format formats[] = {{70, 28}, {71, 29}, {80, 87}, {81, 91}, {115, 10}};
static int64_t convert(int64_t value, bool to_metal) {
	for (auto f : formats)
		if ((to_metal ? f.dxgi : f.metal) == value)
			return to_metal ? f.metal : f.dxgi;
	return 0;
}
static XrResult XRAPI_CALL xrEnumerateSwapchainFormats(XrSession h, uint32_t cap, uint32_t *count, int64_t *out) {
	LOCK;
	auto p = lookup(h, SESSION);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	XrResult x = rpc(MWXR_OP_ENUM_FORMATS, q, r);
	if (XR_FAILED(x))
		return x;
	int64_t translated[64];
	unsigned n = 0;
	for (unsigned i = 0; i < r.count; i++) {
		int64_t v = convert(r.formats[i], false);
		if (v)
			translated[n++] = v;
	}
	return enumerate(cap, count, n, translated, out, sizeof(*out));
}
static XrResult XRAPI_CALL xrCreateSwapchain(XrSession h, const XrSwapchainCreateInfo *ci, XrSwapchain *out) {
	LOCK;
	INFO(ci, XR_TYPE_SWAPCHAIN_CREATE_INFO);
	if (!out)
		return XR_ERROR_VALIDATION_FAILURE;
	auto p = lookup(h, SESSION);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	if (ci->mipCount != 1 || ci->sampleCount != 1 || ci->faceCount != 1)
		return XR_ERROR_FEATURE_UNSUPPORTED;
	int64_t metal = convert(ci->format, true);
	if (!metal)
		return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	q.aux = ci->createFlags;
	q.flags = ci->usageFlags;
	q.a = ci->width;
	q.b = ci->height;
	q.c = ci->arraySize;
	q.d = ci->sampleCount;
	q.format = metal;
	XrResult x = rpc(MWXR_OP_CREATE_SWAPCHAIN, q, r);
	if (XR_FAILED(x))
		return x;
	auto c = make(SWAPCHAIN, r.object, p);
	for (unsigned i = 0; i < r.count; i++) {
		auto &img = r.images[i];
		D3D11_TEXTURE2D_DESC d = {};
		d.Width = img.width;
		d.Height = img.height;
		d.MipLevels = 1;
		d.ArraySize = img.array_size;
		d.Format = (DXGI_FORMAT)ci->format;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags =
		    (ci->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT ? D3D11_BIND_RENDER_TARGET : 0) |
		    (ci->usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT ? D3D11_BIND_SHADER_RESOURCE : 0);
		ID3D11Texture2D *t = nullptr;
		if (FAILED(p->native->ImportSharedTexture(img.name, &d, &t))) {
			q.object = c->id;
			rpc(MWXR_OP_DESTROY_SWAPCHAIN, q, r);
			erase(c);
			return XR_ERROR_GRAPHICS_DEVICE_INVALID;
		}
		c->textures.push_back(t);
	}
	*out = (XrSwapchain)c;
	return x;
}
static XrResult XRAPI_CALL xrEnumerateSwapchainImages(XrSwapchain h, uint32_t cap, uint32_t *count,
						      XrSwapchainImageBaseHeader *out) {
	LOCK;
	auto p = lookup(h, SWAPCHAIN);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	if (!count || (cap && !out))
		return XR_ERROR_VALIDATION_FAILURE;
	*count = (uint32_t)p->textures.size();
	if (cap && cap < *count)
		return XR_ERROR_SIZE_INSUFFICIENT;
	if (cap) {
		auto images = (XrSwapchainImageD3D11KHR *)out;
		for (unsigned i = 0; i < *count; i++) {
			OUTPUT(&images[i], XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR);
			images[i].texture = p->textures[i];
		}
	}
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL xrAcquireSwapchainImage(XrSwapchain h, const XrSwapchainImageAcquireInfo *ci,
						   uint32_t *index) {
	LOCK;
	if (ci)
		INFO(ci, XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
	if (!index)
		return XR_ERROR_VALIDATION_FAILURE;
	auto p = lookup(h, SWAPCHAIN);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	XrResult x = rpc(MWXR_OP_ACQUIRE, q, r);
	if (XR_SUCCEEDED(x))
		*index = r.a;
	return x;
}
static XrResult XRAPI_CALL xrWaitSwapchainImage(XrSwapchain h, const XrSwapchainImageWaitInfo *ci) {
	LOCK;
	INFO(ci, XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
	auto p = lookup(h, SWAPCHAIN);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	q.time = ci->timeout;
	return rpc(MWXR_OP_WAIT, q, r);
}
static XrResult XRAPI_CALL xrReleaseSwapchainImage(XrSwapchain h, const XrSwapchainImageReleaseInfo *ci) {
	LOCK;
	if (ci)
		INFO(ci, XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
	auto p = lookup(h, SWAPCHAIN);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	auto session = p->parent;
	ID3D11DeviceContext *ctx = nullptr;
	ID3D11DeviceContext4 *ctx4 = nullptr;
	session->device->GetImmediateContext(&ctx);
	HRESULT hr = ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&ctx4);
	if (SUCCEEDED(hr))
		hr = ctx4->Signal(session->fence, ++session->fence_value);
	ctx->Flush();
	if (ctx4)
		ctx4->Release();
	ctx->Release();
	if (FAILED(hr))
		return XR_ERROR_RUNTIME_FAILURE;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	q.flags = session->fence_value;
	return rpc(MWXR_OP_RELEASE, q, r);
}
static XrResult XRAPI_CALL xrPollEvent(XrInstance h, XrEventDataBuffer *out) {
	LOCK;
	OUTPUT(out, XR_TYPE_EVENT_DATA_BUFFER);
	auto p = lookup(h, INSTANCE);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	XrResult x = rpc(MWXR_OP_POLL_EVENT, q, r);
	if (x != XR_SUCCESS)
		return x;
	XrSession session = XR_NULL_HANDLE;
	for (auto entry : objects)
		if (entry.second->kind == SESSION && entry.second->id == r.object)
			session = (XrSession)entry.second;
	memset(out, 0, sizeof(*out));
	switch (r.a) {
	case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
		auto e = (XrEventDataSessionStateChanged *)out;
		e->type = (XrStructureType)r.a;
		e->session = session;
		e->state = (XrSessionState)r.b;
		e->time = r.time;
		break;
	}
	case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: {
		auto e = (XrEventDataInstanceLossPending *)out;
		e->type = (XrStructureType)r.a;
		e->lossTime = r.time;
		break;
	}
	case XR_TYPE_EVENT_DATA_EVENTS_LOST: {
		auto e = (XrEventDataEventsLost *)out;
		e->type = (XrStructureType)r.a;
		e->lostEventCount = r.count;
		break;
	}
	case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: {
		auto e = (XrEventDataInteractionProfileChanged *)out;
		e->type = (XrStructureType)r.a;
		e->session = session;
		break;
	}
	case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
		auto e = (XrEventDataReferenceSpaceChangePending *)out;
		e->type = (XrStructureType)r.a;
		e->session = session;
		e->referenceSpaceType = (XrReferenceSpaceType)r.b;
		e->changeTime = r.time;
		e->poseValid = r.c;
		e->poseInPreviousSpace = pose(r.pose);
		break;
	}
	default:
		return XR_EVENT_UNAVAILABLE;
	}
	return x;
}
static XrResult XRAPI_CALL xrStringToPath(XrInstance h, const char *str, XrPath *out) {
	LOCK;
	if (!out)
		return XR_ERROR_VALIDATION_FAILURE;
	auto p = lookup(h, INSTANCE);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	if (!text(q.name, sizeof(q.name), str))
		return XR_ERROR_PATH_FORMAT_INVALID;
	q.object = p->id;
	XrResult x = rpc(MWXR_OP_STRING_TO_PATH, q, r);
	if (XR_SUCCEEDED(x))
		*out = r.object;
	return x;
}
static XrResult XRAPI_CALL xrPathToString(XrInstance h, XrPath path, uint32_t cap, uint32_t *count, char *out) {
	LOCK;
	auto p = lookup(h, INSTANCE);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	q.aux = path;
	XrResult x = rpc(MWXR_OP_PATH_TO_STRING, q, r);
	if (XR_FAILED(x))
		return x;
	return enumerate(cap, count, r.count, r.name, out, 1);
}
static XrResult XRAPI_CALL xrCreateActionSet(XrInstance h, const XrActionSetCreateInfo *ci, XrActionSet *out) {
	LOCK;
	INFO(ci, XR_TYPE_ACTION_SET_CREATE_INFO);
	if (!out)
		return XR_ERROR_VALIDATION_FAILURE;
	auto p = lookup(h, INSTANCE);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	q.a = ci->priority;
	if (!text(q.name, sizeof(q.name), ci->actionSetName) ||
	    !text(q.label, sizeof(q.label), ci->localizedActionSetName))
		return XR_ERROR_VALIDATION_FAILURE;
	XrResult x = rpc(MWXR_OP_CREATE_ACTION_SET, q, r);
	if (XR_SUCCEEDED(x))
		*out = (XrActionSet)make(ACTION_SET, r.object, p);
	return x;
}
static XrResult XRAPI_CALL xrCreateAction(XrActionSet h, const XrActionCreateInfo *ci, XrAction *out) {
	LOCK;
	INFO(ci, XR_TYPE_ACTION_CREATE_INFO);
	if (!out || (ci->countSubactionPaths && !ci->subactionPaths))
		return XR_ERROR_VALIDATION_FAILURE;
	if (ci->countSubactionPaths > 64)
		return XR_ERROR_LIMIT_REACHED;
	auto p = lookup(h, ACTION_SET);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	q.a = ci->countSubactionPaths;
	q.b = ci->actionType;
	for (unsigned i = 0; i < q.a; i++)
		q.ids[i] = ci->subactionPaths[i];
	if (!text(q.name, sizeof(q.name), ci->actionName) || !text(q.label, sizeof(q.label), ci->localizedActionName))
		return XR_ERROR_VALIDATION_FAILURE;
	XrResult x = rpc(MWXR_OP_CREATE_ACTION, q, r);
	if (XR_SUCCEEDED(x))
		*out = (XrAction)make(ACTION, r.object, p);
	return x;
}
static XrResult XRAPI_CALL xrSuggestInteractionProfileBindings(XrInstance h,
							       const XrInteractionProfileSuggestedBinding *ci) {
	LOCK;
	INFO(ci, XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING);
	auto p = lookup(h, INSTANCE);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	if (ci->countSuggestedBindings > 32)
		return XR_ERROR_LIMIT_REACHED;
	if (ci->countSuggestedBindings && !ci->suggestedBindings)
		return XR_ERROR_VALIDATION_FAILURE;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	q.aux = ci->interactionProfile;
	q.a = ci->countSuggestedBindings;
	for (unsigned i = 0; i < q.a; i++) {
		auto a = lookup(ci->suggestedBindings[i].action, ACTION);
		if (!a)
			return XR_ERROR_HANDLE_INVALID;
		q.ids[2 * i] = a->id;
		q.ids[2 * i + 1] = ci->suggestedBindings[i].binding;
	}
	return rpc(MWXR_OP_SUGGEST_BINDINGS, q, r);
}
static XrResult XRAPI_CALL xrAttachSessionActionSets(XrSession h, const XrSessionActionSetsAttachInfo *ci) {
	LOCK;
	INFO(ci, XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO);
	auto p = lookup(h, SESSION);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	if (ci->countActionSets > 16)
		return XR_ERROR_LIMIT_REACHED;
	if (ci->countActionSets && !ci->actionSets)
		return XR_ERROR_VALIDATION_FAILURE;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	q.a = ci->countActionSets;
	for (unsigned i = 0; i < q.a; i++) {
		auto a = lookup(ci->actionSets[i], ACTION_SET);
		if (!a)
			return XR_ERROR_HANDLE_INVALID;
		q.ids[i] = a->id;
	}
	return rpc(MWXR_OP_ATTACH_ACTION_SETS, q, r);
}
static XrResult XRAPI_CALL xrSyncActions(XrSession h, const XrActionsSyncInfo *ci) {
	LOCK;
	INFO(ci, XR_TYPE_ACTIONS_SYNC_INFO);
	auto p = lookup(h, SESSION);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	if (ci->countActiveActionSets > 32)
		return XR_ERROR_LIMIT_REACHED;
	if (ci->countActiveActionSets && !ci->activeActionSets)
		return XR_ERROR_VALIDATION_FAILURE;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	q.a = ci->countActiveActionSets;
	for (unsigned i = 0; i < q.a; i++) {
		auto a = lookup(ci->activeActionSets[i].actionSet, ACTION_SET);
		if (!a)
			return XR_ERROR_HANDLE_INVALID;
		q.ids[2 * i] = a->id;
		q.ids[2 * i + 1] = ci->activeActionSets[i].subactionPath;
	}
	return rpc(MWXR_OP_SYNC_ACTIONS, q, r);
}
static XrResult action_state(XrSession h, const XrActionStateGetInfo *ci, uint32_t op, mwxr_wire_response &r) {
	INFO(ci, XR_TYPE_ACTION_STATE_GET_INFO);
	auto p = lookup(h, SESSION), a = lookup(ci->action, ACTION);
	if (!p || !a)
		return XR_ERROR_HANDLE_INVALID;
	mwxr_wire_request q = {};
	q.object = p->id;
	q.aux = a->id;
	q.flags = ci->subactionPath;
	return rpc(op, q, r);
}
static XrResult XRAPI_CALL xrGetActionStateBoolean(XrSession h, const XrActionStateGetInfo *ci,
						   XrActionStateBoolean *out) {
	LOCK;
	OUTPUT(out, XR_TYPE_ACTION_STATE_BOOLEAN);
	mwxr_wire_response r = {};
	XrResult x = action_state(h, ci, MWXR_OP_ACTION_BOOLEAN, r);
	if (XR_SUCCEEDED(x)) {
		out->currentState = r.a;
		out->changedSinceLastSync = r.b;
		out->lastChangeTime = r.time;
		out->isActive = r.c;
	}
	return x;
}
static XrResult XRAPI_CALL xrGetActionStateFloat(XrSession h, const XrActionStateGetInfo *ci, XrActionStateFloat *out) {
	LOCK;
	OUTPUT(out, XR_TYPE_ACTION_STATE_FLOAT);
	mwxr_wire_response r = {};
	XrResult x = action_state(h, ci, MWXR_OP_ACTION_FLOAT, r);
	if (XR_SUCCEEDED(x)) {
		out->currentState = r.values[0];
		out->changedSinceLastSync = r.b;
		out->lastChangeTime = r.time;
		out->isActive = r.c;
	}
	return x;
}
static XrResult XRAPI_CALL xrGetActionStateVector2f(XrSession h, const XrActionStateGetInfo *ci,
						    XrActionStateVector2f *out) {
	LOCK;
	OUTPUT(out, XR_TYPE_ACTION_STATE_VECTOR2F);
	mwxr_wire_response r = {};
	XrResult x = action_state(h, ci, MWXR_OP_ACTION_VECTOR, r);
	if (XR_SUCCEEDED(x)) {
		out->currentState = {r.values[0], r.values[1]};
		out->changedSinceLastSync = r.b;
		out->lastChangeTime = r.time;
		out->isActive = r.c;
	}
	return x;
}
static XrResult XRAPI_CALL xrGetActionStatePose(XrSession h, const XrActionStateGetInfo *ci, XrActionStatePose *out) {
	LOCK;
	OUTPUT(out, XR_TYPE_ACTION_STATE_POSE);
	mwxr_wire_response r = {};
	XrResult x = action_state(h, ci, MWXR_OP_ACTION_POSE, r);
	if (XR_SUCCEEDED(x))
		out->isActive = r.c;
	return x;
}
static XrResult XRAPI_CALL xrGetCurrentInteractionProfile(XrSession h, XrPath path, XrInteractionProfileState *out) {
	LOCK;
	OUTPUT(out, XR_TYPE_INTERACTION_PROFILE_STATE);
	auto p = lookup(h, SESSION);
	if (!p)
		return XR_ERROR_HANDLE_INVALID;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	q.aux = path;
	XrResult x = rpc(MWXR_OP_CURRENT_PROFILE, q, r);
	if (XR_SUCCEEDED(x))
		out->interactionProfile = r.object;
	return x;
}
static XrResult haptic(XrSession h, const XrHapticActionInfo *ci, uint32_t op, const XrHapticBaseHeader *value) {
	INFO(ci, XR_TYPE_HAPTIC_ACTION_INFO);
	auto p = lookup(h, SESSION), a = lookup(ci->action, ACTION);
	if (!p || !a)
		return XR_ERROR_HANDLE_INVALID;
	mwxr_wire_request q = {};
	mwxr_wire_response r = {};
	q.object = p->id;
	q.aux = a->id;
	q.flags = ci->subactionPath;
	if (value) {
		INFO(value, XR_TYPE_HAPTIC_VIBRATION);
		auto v = (const XrHapticVibration *)value;
		q.time = v->duration;
		float values[2] = {v->frequency, v->amplitude};
		memcpy(&q.ids[0], values, sizeof(values));
	}
	return rpc(op, q, r);
}
static XrResult XRAPI_CALL xrApplyHapticFeedback(XrSession h, const XrHapticActionInfo *ci,
						 const XrHapticBaseHeader *value) {
	LOCK;
	if (!value)
		return XR_ERROR_VALIDATION_FAILURE;
	return haptic(h, ci, MWXR_OP_APPLY_HAPTIC, value);
}
static XrResult XRAPI_CALL xrStopHapticFeedback(XrSession h, const XrHapticActionInfo *ci) {
	LOCK;
	return haptic(h, ci, MWXR_OP_STOP_HAPTIC, nullptr);
}
static XrResult XRAPI_CALL xrResultToString(XrInstance h, XrResult result, char out[XR_MAX_RESULT_STRING_SIZE]) {
	LOCK;
	if (!lookup(h, INSTANCE))
		return XR_ERROR_HANDLE_INVALID;
	if (!out)
		return XR_ERROR_VALIDATION_FAILURE;
	switch (result) {
#define CASE(n, v)                                                                                                     \
	case n:                                                                                                        \
		strcpy(out, #n);                                                                                       \
		return XR_SUCCESS;
		XR_LIST_ENUM_XrResult(CASE)
#undef CASE
		    default : snprintf(out, XR_MAX_RESULT_STRING_SIZE, "XR_UNKNOWN_%d", result);
		break;
	}
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL xrStructureTypeToString(XrInstance h, XrStructureType type,
						   char out[XR_MAX_STRUCTURE_NAME_SIZE]) {
	LOCK;
	if (!lookup(h, INSTANCE))
		return XR_ERROR_HANDLE_INVALID;
	if (!out)
		return XR_ERROR_VALIDATION_FAILURE;
	switch (type) {
#define CASE(n, v)                                                                                                     \
	case n:                                                                                                        \
		strcpy(out, #n);                                                                                       \
		return XR_SUCCESS;
		XR_LIST_ENUM_XrStructureType(CASE)
#undef CASE
		    default : snprintf(out, XR_MAX_STRUCTURE_NAME_SIZE, "XR_UNKNOWN_%d", type);
		break;
	}
	return XR_SUCCESS;
}

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char *name,
									   PFN_xrVoidFunction *out) {
	LOCK;
	if (!name || !out)
		return XR_ERROR_VALIDATION_FAILURE;
	*out = nullptr;
	if (instance && !lookup(instance, INSTANCE))
		return XR_ERROR_HANDLE_INVALID;
	if (!instance && strcmp(name, "xrCreateInstance") && strcmp(name, "xrEnumerateInstanceExtensionProperties") &&
	    strcmp(name, "xrGetInstanceProcAddr"))
		return XR_ERROR_HANDLE_INVALID;
	if (!strcmp(name, "xrGetD3D11GraphicsRequirementsKHR") && !d3d_enabled)
		return XR_ERROR_FUNCTION_UNSUPPORTED;
#define PROC(fn)                                                                                                       \
	if (!strcmp(name, #fn)) {                                                                                      \
		*out = (PFN_xrVoidFunction)fn;                                                                         \
		return XR_SUCCESS;                                                                                     \
	}
	PROC(xrGetInstanceProcAddr)
	PROC(xrEnumerateInstanceExtensionProperties)
	PROC(xrCreateInstance)
	PROC(xrDestroyInstance)
	PROC(xrGetInstanceProperties)
	PROC(xrGetSystem)
	PROC(xrGetSystemProperties)
	PROC(xrGetD3D11GraphicsRequirementsKHR)
	PROC(xrCreateSession)
	PROC(xrDestroySession)
	PROC(xrDestroySpace)
	PROC(xrDestroySwapchain)
	PROC(xrDestroyAction)
	PROC(xrDestroyActionSet)
	PROC(xrBeginSession)
	PROC(xrEndSession)
	PROC(xrRequestExitSession)
	PROC(xrEnumerateViewConfigurations)
	PROC(xrGetViewConfigurationProperties)
	PROC(xrEnumerateViewConfigurationViews)
	PROC(xrEnumerateEnvironmentBlendModes)
	PROC(xrEnumerateReferenceSpaces)
	PROC(xrGetReferenceSpaceBoundsRect)
	PROC(xrCreateReferenceSpace)
	PROC(xrCreateActionSpace)
	PROC(xrLocateSpace)
	PROC(xrLocateViews)
	PROC(xrWaitFrame)
	PROC(xrBeginFrame)
	PROC(xrEndFrame)
	PROC(xrEnumerateSwapchainFormats)
	PROC(xrCreateSwapchain)
	PROC(xrEnumerateSwapchainImages)
	PROC(xrAcquireSwapchainImage)
	PROC(xrWaitSwapchainImage)
	PROC(xrReleaseSwapchainImage)
	PROC(xrPollEvent)
	PROC(xrStringToPath)
	PROC(xrPathToString)
	PROC(xrCreateActionSet)
	PROC(xrCreateAction)
	PROC(xrSuggestInteractionProfileBindings)
	PROC(xrAttachSessionActionSets)
	PROC(xrSyncActions)
	PROC(xrGetActionStateBoolean)
	PROC(xrGetActionStateFloat)
	PROC(xrGetActionStateVector2f)
	PROC(xrGetActionStatePose)
	PROC(xrGetCurrentInteractionProfile)
	PROC(xrApplyHapticFeedback)
	PROC(xrStopHapticFeedback)
	PROC(xrResultToString)
	PROC(xrStructureTypeToString)
#undef PROC
	return XR_ERROR_FUNCTION_UNSUPPORTED;
}
extern "C" __declspec(dllexport) XrResult XRAPI_CALL
xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo *loader, XrNegotiateRuntimeRequest *runtime) {
	if (!loader || !runtime || loader->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
	    loader->structVersion != XR_LOADER_INFO_STRUCT_VERSION || loader->structSize != sizeof(*loader) ||
	    runtime->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
	    runtime->structVersion != XR_RUNTIME_INFO_STRUCT_VERSION || runtime->structSize != sizeof(*runtime))
		return XR_ERROR_INITIALIZATION_FAILED;
	if (loader->minInterfaceVersion > 1 || loader->maxInterfaceVersion < 1 ||
	    loader->minApiVersion > XR_MAKE_VERSION(1, 0, 0) || loader->maxApiVersion < XR_MAKE_VERSION(1, 0, 0))
		return XR_ERROR_INITIALIZATION_FAILED;
	runtime->runtimeInterfaceVersion = 1;
	runtime->runtimeApiVersion = XR_MAKE_VERSION(1, 0, 0);
	runtime->getInstanceProcAddr = xrGetInstanceProcAddr;
	return XR_SUCCESS;
}

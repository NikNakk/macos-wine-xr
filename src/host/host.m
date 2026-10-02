// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "macos_wine_xr/native_capability.h"
#include "native_openxr_internal.h"
#include "tcp_auth.h"
#include "transport.h"
#include <errno.h>
#include <mach/mach.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
@interface MTLSharedTextureHandle (MWXRHost)
- (mach_port_t)createMachPort;
@end
@interface MTLSharedEventHandle (MWXRHost)
- (mach_port_t)eventPort;
@end
#define SPACE_LIMIT 64
#define SWAPCHAIN_LIMIT 16
#define ACTION_LIMIT 128
#define SET_LIMIT 16
struct host_swapchain {
	struct mwxr_native_swapchain *native;
	id<MTLTexture> staging[16];
	struct mwxr_native_capability *caps[16];
	uint32_t acquired;
};
static FILE *trace;
static uint64_t now_ns(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}
struct host {
	struct mwxr_native_backend *backend;
	XrSpace spaces[SPACE_LIMIT];
	XrAction actions[ACTION_LIMIT];
	XrActionSet sets[SET_LIMIT];
	uint64_t action_owner[ACTION_LIMIT];
	struct host_swapchain swapchains[SWAPCHAIN_LIMIT];
	id<MTLSharedEvent> event;
	struct mwxr_native_capability *event_cap;
	uint64_t frames, last_frame_ns;
	unsigned end_frame_reports;
};
// Handle classes occupy distinct ranges; IDs never contain native handles.
#define INSTANCE_ID 1
#define SYSTEM_ID 2
#define SESSION_ID 3
#define SPACE_BASE 0x10000
#define SWAPCHAIN_BASE 0x20000
#define ACTION_BASE 0x30000
#define SET_BASE 0x40000
static int index_id(uint64_t id, uint64_t base, unsigned count) {
	return id >= base && id < base + count ? (int)(id - base) : -1;
}
static XrSpace space(struct host *h, uint64_t id) {
	int i = index_id(id, SPACE_BASE, SPACE_LIMIT);
	return i < 0 ? XR_NULL_HANDLE : h->spaces[i];
}
static XrAction action(struct host *h, uint64_t id) {
	int i = index_id(id, ACTION_BASE, ACTION_LIMIT);
	return i < 0 ? XR_NULL_HANDLE : h->actions[i];
}
static XrActionSet set(struct host *h, uint64_t id) {
	int i = index_id(id, SET_BASE, SET_LIMIT);
	return i < 0 ? XR_NULL_HANDLE : h->sets[i];
}
static struct host_swapchain *chain(struct host *h, uint64_t id) {
	int i = index_id(id, SWAPCHAIN_BASE, SWAPCHAIN_LIMIT);
	return i < 0 || !h->swapchains[i].native ? NULL : &h->swapchains[i];
}
static XrPosef native_pose(const struct mwxr_wire_pose *p) {
	return (XrPosef){{p->orientation[0], p->orientation[1], p->orientation[2], p->orientation[3]},
			 {p->position[0], p->position[1], p->position[2]}};
}
static void wire_pose(struct mwxr_wire_pose *p, XrPosef n) {
	memcpy(p->orientation, &n.orientation, sizeof(p->orientation));
	memcpy(p->position, &n.position, sizeof(p->position));
}
static void destroy_chain(struct host_swapchain *s) {
	for (unsigned i = 0; i < 16; i++) {
		mwxr_native_capability_close(s->caps[i]);
		[s->staging[i] release];
	}
	mwxr_native_swapchain_destroy(s->native);
	memset(s, 0, sizeof(*s));
}
static void cleanup_session(struct host *h) {
	if (!h->backend)
		return;
	if (h->frames)
		fprintf(stderr, "host: submitted_frames=%llu\n", h->frames);
	h->frames = 0;
	h->last_frame_ns = 0;
	for (unsigned i = 0; i < SWAPCHAIN_LIMIT; i++)
		destroy_chain(&h->swapchains[i]);
	for (unsigned i = 0; i < SPACE_LIMIT; i++)
		if (h->spaces[i]) {
			h->backend->DestroySpace(h->spaces[i]);
			h->spaces[i] = XR_NULL_HANDLE;
		}
	mwxr_native_capability_close(h->event_cap);
	h->event_cap = NULL;
	[h->event release];
	h->event = nil;
	if (h->backend->session) {
		h->backend->DestroySession(h->backend->session);
		h->backend->session = XR_NULL_HANDLE;
	}
}
static void cleanup(struct host *h) {
	cleanup_session(h);
	mwxr_native_backend_close(h->backend);
	memset(h, 0, sizeof(*h));
}
static XrResult create_chain(struct host *h, const struct mwxr_wire_request *q, struct mwxr_wire_response *r) {
	struct mwxr_native_backend *b = h->backend;
	unsigned slot = 0;
	while (slot < SWAPCHAIN_LIMIT && h->swapchains[slot].native)
		slot++;
	if (slot == SWAPCHAIN_LIMIT)
		return XR_ERROR_LIMIT_REACHED;
	XrSwapchainCreateInfo ci = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
				    .createFlags = q->aux,
				    .usageFlags = q->flags,
				    .format = q->format,
				    .sampleCount = q->d,
				    .width = q->a,
				    .height = q->b,
				    .arraySize = q->c,
				    .faceCount = 1,
				    .mipCount = 1};
	struct host_swapchain *s = &h->swapchains[slot];
	XrResult result = mwxr_native_swapchain_create(b, &ci, &s->native);
	if (XR_FAILED(result))
		return result;
	uint32_t count = mwxr_native_swapchain_image_count(s->native);
	if (count > 16) {
		destroy_chain(s);
		return XR_ERROR_LIMIT_REACHED;
	}
	for (unsigned i = 0; i < count; i++) {
		const struct mwxr_image_info *info = mwxr_native_swapchain_image(s->native, i);
		mach_port_t port = MACH_PORT_NULL;
		if (info->strategy == MWXR_IMAGE_SHARED_METAL)
			result = mwxr_native_swapchain_export(s->native, i, &port);
		else {
			MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:ci.format
													width:ci.width
												       height:ci.height
												    mipmapped:NO];
			desc.textureType = ci.arraySize > 1 ? MTLTextureType2DArray : MTLTextureType2D;
			desc.arrayLength = ci.arraySize;
			desc.storageMode = MTLStorageModePrivate;
			desc.usage =
			    MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
			s->staging[i] = [b->device newSharedTextureWithDescriptor:desc];
			MTLSharedTextureHandle *handle = [s->staging[i] newSharedTextureHandle];
			port = [handle createMachPort];
			[handle release];
			if (!port)
				result = XR_ERROR_FEATURE_UNSUPPORTED;
		}
		if (XR_SUCCEEDED(result) && mwxr_native_capability_create(port, 1, &s->caps[i]))
			result = XR_ERROR_RUNTIME_FAILURE;
		if (port)
			mach_port_deallocate(mach_task_self(), port);
		if (XR_FAILED(result)) {
			destroy_chain(s);
			return result;
		}
		r->images[i] = (struct mwxr_wire_image){.id = info->image_id,
							.format = info->metal_format,
							.width = info->width,
							.height = info->height,
							.array_size = info->array_size,
							.strategy = info->strategy};
		snprintf(r->images[i].name, sizeof(r->images[i].name), "%s", mwxr_native_capability_name(s->caps[i]));
	}
	r->object = SWAPCHAIN_BASE + slot;
	r->count = count;
	s->acquired = UINT32_MAX;
	fprintf(stderr, "host: swapchain=%llu images=%u strategy=%s\n", r->object, count,
		r->images[0].strategy == MWXR_IMAGE_SHARED_METAL ? "shared-metal-zero-copy" : "gpu-blit");
	return XR_SUCCESS;
}
static XrResult dispatch(struct host *h, uint32_t op, const struct mwxr_wire_request *q, struct mwxr_wire_response *r) {
	struct mwxr_native_backend *b = h->backend;
	if (op == MWXR_OP_HELLO) {
		if (b || q->aux != MWXR_RPC_FINGERPRINT || q->a != MWXR_RPC_VERSION)
			return XR_ERROR_VALIDATION_FAILURE;
		XrApplicationInfo app = {.apiVersion = q->flags, .applicationVersion = q->b, .engineVersion = q->c};
		memcpy(app.applicationName, q->name, sizeof(app.applicationName) - 1);
		memcpy(app.engineName, q->label, sizeof(app.engineName) - 1);
		XrResult result = mwxr_native_backend_open_host(NULL, &app, &h->backend);
		if (XR_FAILED(result))
			return result;
		r->object = INSTANCE_ID;
		r->aux = MWXR_RPC_FINGERPRINT;
		r->a = MWXR_RPC_VERSION;
		r->flags = 3;
		return XR_SUCCESS;
	}
	if (!b)
		return XR_ERROR_HANDLE_INVALID;
	if (op == MWXR_OP_PROPERTIES || op == MWXR_OP_GET_SYSTEM) {
		if (q->object != INSTANCE_ID)
			return XR_ERROR_HANDLE_INVALID;
		if (op == MWXR_OP_GET_SYSTEM && q->a != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY)
			return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
		r->object = SYSTEM_ID;
		r->version = b->info.runtime.runtimeVersion;
		snprintf(r->name, sizeof(r->name), "%s", b->info.runtime.runtimeName);
		snprintf(r->label, sizeof(r->label), "%s", b->info.system.systemName);
		r->a = b->info.system.vendorId;
		r->b = b->info.system.graphicsProperties.maxSwapchainImageWidth;
		r->c = b->info.system.graphicsProperties.maxSwapchainImageHeight;
		r->d = 1;
		r->flags = (b->info.system.trackingProperties.orientationTracking ? 1 : 0) |
			   (b->info.system.trackingProperties.positionTracking ? 2 : 0);
		return XR_SUCCESS;
	}
	if (op == MWXR_OP_ENUM_VIEWS || op == MWXR_OP_ENUM_BLEND) {
		if (q->object != INSTANCE_ID || q->aux != SYSTEM_ID)
			return XR_ERROR_HANDLE_INVALID;
		if (q->a != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
			return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
		if (op == MWXR_OP_ENUM_BLEND) {
			XrEnvironmentBlendMode modes[64];
			XrResult x =
			    b->EnumerateEnvironmentBlendModes(b->instance, b->system, q->a, 64, &r->count, modes);
			if (XR_SUCCEEDED(x))
				for (unsigned i = 0; i < r->count; i++)
					r->formats[i] = modes[i];
			return x;
		}
		XrViewConfigurationView views[4] = {{0}};
		for (unsigned i = 0; i < 4; i++)
			views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
		XrResult x = b->EnumerateViewConfigurationViews(b->instance, b->system, q->a, 4, &r->count, views);
		if (XR_SUCCEEDED(x))
			for (unsigned i = 0; i < r->count; i++) {
				r->dimensions[6 * i] = views[i].recommendedImageRectWidth;
				r->dimensions[6 * i + 1] = views[i].maxImageRectWidth;
				r->dimensions[6 * i + 2] = views[i].recommendedImageRectHeight;
				r->dimensions[6 * i + 3] = views[i].maxImageRectHeight;
				r->dimensions[6 * i + 4] = views[i].recommendedSwapchainSampleCount;
				r->dimensions[6 * i + 5] = views[i].maxSwapchainSampleCount;
			}
		return x;
	}
	if (op == MWXR_OP_CREATE_SESSION) {
		if (q->object != INSTANCE_ID || q->aux != SYSTEM_ID)
			return XR_ERROR_HANDLE_INVALID;
		XrResult x = mwxr_native_backend_create_session(b);
		if (XR_FAILED(x))
			return x;
		h->event = [b->device newSharedEvent];
		MTLSharedEventHandle *handle = [h->event newSharedEventHandle];
		mach_port_t port = [handle eventPort];
		int status = port ? mwxr_native_capability_create(port, 2, &h->event_cap) : -1;
		[handle release];
		if (status) {
			cleanup_session(h);
			return XR_ERROR_RUNTIME_FAILURE;
		}
		r->object = SESSION_ID;
		snprintf(r->name, sizeof(r->name), "%s", mwxr_native_capability_name(h->event_cap));
		return XR_SUCCESS;
	}
	if (op == MWXR_OP_POLL_EVENT) {
		if (q->object != INSTANCE_ID)
			return XR_ERROR_HANDLE_INVALID;
		XrEventDataBuffer event = {.type = XR_TYPE_EVENT_DATA_BUFFER};
		XrResult x = b->PollEvent(b->instance, &event);
		if (x != XR_SUCCESS)
			return x;
		r->a = event.type;
		switch (event.type) {
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
			XrEventDataSessionStateChanged *e = (void *)&event;
			r->object = SESSION_ID;
			r->b = e->state;
			r->time = e->time;
			break;
		}
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
			r->time = ((XrEventDataInstanceLossPending *)&event)->lossTime;
			break;
		case XR_TYPE_EVENT_DATA_EVENTS_LOST:
			r->count = ((XrEventDataEventsLost *)&event)->lostEventCount;
			break;
		case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
			r->object = SESSION_ID;
			break;
		case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
			XrEventDataReferenceSpaceChangePending *e = (void *)&event;
			r->object = SESSION_ID;
			r->b = e->referenceSpaceType;
			r->time = e->changeTime;
			r->c = e->poseValid;
			wire_pose(&r->pose, e->poseInPreviousSpace);
			break;
		}
		default:
			return XR_EVENT_UNAVAILABLE;
		}
		return x;
	}
	if (op == MWXR_OP_STRING_TO_PATH) {
		if (q->object != INSTANCE_ID)
			return XR_ERROR_HANDLE_INVALID;
		return b->StringToPath(b->instance, q->name, &r->object);
	}
	if (op == MWXR_OP_PATH_TO_STRING) {
		if (q->object != INSTANCE_ID)
			return XR_ERROR_HANDLE_INVALID;
		return b->PathToString(b->instance, q->aux, sizeof(r->name), &r->count, r->name);
	}
	if (op == MWXR_OP_CREATE_ACTION_SET) {
		if (q->object != INSTANCE_ID)
			return XR_ERROR_HANDLE_INVALID;
		unsigned i = 0;
		while (i < SET_LIMIT && h->sets[i])
			i++;
		if (i == SET_LIMIT)
			return XR_ERROR_LIMIT_REACHED;
		XrActionSetCreateInfo ci = {.type = XR_TYPE_ACTION_SET_CREATE_INFO, .priority = q->a};
		snprintf(ci.actionSetName, sizeof(ci.actionSetName), "%s", q->name);
		snprintf(ci.localizedActionSetName, sizeof(ci.localizedActionSetName), "%s", q->label);
		XrResult x = b->CreateActionSet(b->instance, &ci, &h->sets[i]);
		if (XR_SUCCEEDED(x))
			r->object = SET_BASE + i;
		return x;
	}
	if (op == MWXR_OP_CREATE_ACTION) {
		XrActionSet as = set(h, q->object);
		if (!as)
			return XR_ERROR_HANDLE_INVALID;
		if (q->a > 64)
			return XR_ERROR_LIMIT_REACHED;
		unsigned i = 0;
		while (i < ACTION_LIMIT && h->actions[i])
			i++;
		if (i == ACTION_LIMIT)
			return XR_ERROR_LIMIT_REACHED;
		XrActionCreateInfo ci = {.type = XR_TYPE_ACTION_CREATE_INFO,
					 .actionType = q->b,
					 .countSubactionPaths = q->a,
					 .subactionPaths = q->ids};
		snprintf(ci.actionName, sizeof(ci.actionName), "%s", q->name);
		snprintf(ci.localizedActionName, sizeof(ci.localizedActionName), "%s", q->label);
		XrResult x = b->CreateAction(as, &ci, &h->actions[i]);
		if (XR_SUCCEEDED(x)) {
			r->object = ACTION_BASE + i;
			h->action_owner[i] = q->object;
		}
		return x;
	}
	if (op == MWXR_OP_DESTROY_ACTION) {
		int i = index_id(q->object, ACTION_BASE, ACTION_LIMIT);
		if (i < 0 || !h->actions[i])
			return XR_ERROR_HANDLE_INVALID;
		XrResult x = b->DestroyAction(h->actions[i]);
		if (XR_SUCCEEDED(x))
			h->actions[i] = XR_NULL_HANDLE;
		return x;
	}
	if (op == MWXR_OP_DESTROY_ACTION_SET) {
		int i = index_id(q->object, SET_BASE, SET_LIMIT);
		if (i < 0 || !h->sets[i])
			return XR_ERROR_HANDLE_INVALID;
		XrResult x = b->DestroyActionSet(h->sets[i]);
		if (XR_SUCCEEDED(x)) {
			h->sets[i] = XR_NULL_HANDLE;
			for (unsigned j = 0; j < ACTION_LIMIT; j++)
				if (h->action_owner[j] == q->object) {
					h->actions[j] = XR_NULL_HANDLE;
					h->action_owner[j] = 0;
				}
		}
		return x;
	}
	if (op == MWXR_OP_SUGGEST_BINDINGS) {
		if (q->object != INSTANCE_ID)
			return XR_ERROR_HANDLE_INVALID;
		if (q->a > 32)
			return XR_ERROR_LIMIT_REACHED;
		XrActionSuggestedBinding bindings[32];
		for (unsigned i = 0; i < q->a; i++) {
			bindings[i] = (XrActionSuggestedBinding){action(h, q->ids[2 * i]), q->ids[2 * i + 1]};
			if (!bindings[i].action)
				return XR_ERROR_HANDLE_INVALID;
		}
		XrInteractionProfileSuggestedBinding ci = {.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
							   .interactionProfile = q->aux,
							   .countSuggestedBindings = q->a,
							   .suggestedBindings = bindings};
		return b->SuggestInteractionProfileBindings(b->instance, &ci);
	}
	if (!b->session)
		return XR_ERROR_HANDLE_INVALID;
	if (op >= MWXR_OP_BEGIN_SESSION && op <= MWXR_OP_REQUEST_EXIT && q->object != SESSION_ID)
		return XR_ERROR_HANDLE_INVALID;
	switch (op) {
	case MWXR_OP_DESTROY_SESSION:
		if (q->object != SESSION_ID)
			return XR_ERROR_HANDLE_INVALID;
		cleanup_session(h);
		return XR_SUCCESS;
	case MWXR_OP_BEGIN_SESSION: {
		XrSessionBeginInfo ci = {.type = XR_TYPE_SESSION_BEGIN_INFO, .primaryViewConfigurationType = q->a};
		return b->BeginSession(b->session, &ci);
	}
	case MWXR_OP_END_SESSION:
		return b->EndSession(b->session);
	case MWXR_OP_REQUEST_EXIT:
		return b->RequestExitSession(b->session);
	case MWXR_OP_ENUM_REFERENCE: {
		if (q->object != SESSION_ID)
			return XR_ERROR_HANDLE_INVALID;
		XrReferenceSpaceType types[64];
		XrResult x = b->EnumerateReferenceSpaces(b->session, 64, &r->count, types);
		if (XR_SUCCEEDED(x))
			for (unsigned i = 0; i < r->count; i++)
				r->formats[i] = types[i];
		return x;
	}
	case MWXR_OP_SPACE_BOUNDS: {
		if (q->object != SESSION_ID)
			return XR_ERROR_HANDLE_INVALID;
		XrExtent2Df bounds = {0};
		XrResult x = b->GetReferenceSpaceBoundsRect(b->session, q->a, &bounds);
		r->values[0] = bounds.width;
		r->values[1] = bounds.height;
		return x;
	}
	case MWXR_OP_ENUM_FORMATS:
		if (q->object != SESSION_ID)
			return XR_ERROR_HANDLE_INVALID;
		return mwxr_native_backend_formats(b, 64, &r->count, r->formats);
	case MWXR_OP_CREATE_SPACE:
	case MWXR_OP_CREATE_ACTION_SPACE: {
		if (q->object != SESSION_ID)
			return XR_ERROR_HANDLE_INVALID;
		unsigned i = 0;
		while (i < SPACE_LIMIT && h->spaces[i])
			i++;
		if (i == SPACE_LIMIT)
			return XR_ERROR_LIMIT_REACHED;
		XrResult x;
		if (op == MWXR_OP_CREATE_SPACE) {
			XrReferenceSpaceCreateInfo ci = {.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
							 .referenceSpaceType = q->a,
							 .poseInReferenceSpace = native_pose(&q->pose)};
			x = b->CreateReferenceSpace(b->session, &ci, &h->spaces[i]);
		} else {
			XrActionSpaceCreateInfo ci = {.type = XR_TYPE_ACTION_SPACE_CREATE_INFO,
						      .action = action(h, q->aux),
						      .subactionPath = q->flags,
						      .poseInActionSpace = native_pose(&q->pose)};
			if (!ci.action)
				return XR_ERROR_HANDLE_INVALID;
			x = b->CreateActionSpace(b->session, &ci, &h->spaces[i]);
		}
		if (XR_SUCCEEDED(x))
			r->object = SPACE_BASE + i;
		return x;
	}
	case MWXR_OP_DESTROY_SPACE: {
		int i = index_id(q->object, SPACE_BASE, SPACE_LIMIT);
		if (i < 0 || !h->spaces[i])
			return XR_ERROR_HANDLE_INVALID;
		XrResult x = b->DestroySpace(h->spaces[i]);
		if (XR_SUCCEEDED(x))
			h->spaces[i] = XR_NULL_HANDLE;
		return x;
	}
	case MWXR_OP_LOCATE_SPACE: {
		if (!space(h, q->object) || !space(h, q->aux))
			return XR_ERROR_HANDLE_INVALID;
		XrSpaceLocation loc = {.type = XR_TYPE_SPACE_LOCATION};
		XrResult x = b->LocateSpace(space(h, q->object), space(h, q->aux), q->time, &loc);
		r->flags = loc.locationFlags;
		wire_pose(&r->pose, loc.pose);
		return x;
	}
	case MWXR_OP_LOCATE_VIEWS: {
		if (q->object != SESSION_ID || !space(h, q->aux))
			return XR_ERROR_HANDLE_INVALID;
		XrViewLocateInfo ci = {.type = XR_TYPE_VIEW_LOCATE_INFO,
				       .viewConfigurationType = q->a,
				       .displayTime = q->time,
				       .space = space(h, q->aux)};
		XrViewState state = {.type = XR_TYPE_VIEW_STATE};
		XrView views[4] = {{0}};
		for (unsigned i = 0; i < 4; i++)
			views[i].type = XR_TYPE_VIEW;
		XrResult x = b->LocateViews(b->session, &ci, &state, 4, &r->count, views);
		r->flags = state.viewStateFlags;
		if (XR_SUCCEEDED(x))
			for (unsigned i = 0; i < r->count; i++) {
				wire_pose(&r->views[i].pose, views[i].pose);
				memcpy(r->views[i].fov, &views[i].fov, sizeof(r->views[i].fov));
			}
		return x;
	}
	case MWXR_OP_WAIT_FRAME: {
		if (q->object != SESSION_ID)
			return XR_ERROR_HANDLE_INVALID;
		XrFrameWaitInfo ci = {.type = XR_TYPE_FRAME_WAIT_INFO};
		XrFrameState state = {.type = XR_TYPE_FRAME_STATE};
		XrResult x = b->WaitFrame(b->session, &ci, &state);
		r->time = state.predictedDisplayTime;
		r->period = state.predictedDisplayPeriod;
		r->a = state.shouldRender;
		return x;
	}
	case MWXR_OP_BEGIN_FRAME: {
		if (q->object != SESSION_ID)
			return XR_ERROR_HANDLE_INVALID;
		XrFrameBeginInfo ci = {.type = XR_TYPE_FRAME_BEGIN_INFO};
		return b->BeginFrame(b->session, &ci);
	}
	case MWXR_OP_END_FRAME: {
		if (q->object != SESSION_ID)
			return XR_ERROR_HANDLE_INVALID;
		if (q->a > 4)
			return XR_ERROR_LIMIT_REACHED;
		XrCompositionLayerProjectionView views[4] = {{0}};
		for (unsigned i = 0; i < q->a; i++) {
			struct host_swapchain *s = chain(h, q->views[i].swapchain);
			if (!s)
				return XR_ERROR_HANDLE_INVALID;
			views[i] = (XrCompositionLayerProjectionView){
			    .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
			    .pose = native_pose(&q->views[i].pose),
			    .subImage = {
				.swapchain = s->native->handle,
				.imageRect = {{q->views[i].x, q->views[i].y}, {q->views[i].width, q->views[i].height}},
				.imageArrayIndex = q->views[i].array_index}};
			memcpy(&views[i].fov, q->views[i].fov, sizeof(views[i].fov));
		}
		XrCompositionLayerProjection layer = {.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
						      .layerFlags = q->flags,
						      .space = space(h, q->aux),
						      .viewCount = q->a,
						      .views = views};
		if (q->a && !layer.space)
			return XR_ERROR_HANDLE_INVALID;
		const XrCompositionLayerBaseHeader *layers[] = {(const void *)&layer};
		XrFrameEndInfo ci = {.type = XR_TYPE_FRAME_END_INFO,
				     .displayTime = q->time,
				     .environmentBlendMode = q->b,
				     .layerCount = q->a ? 1 : 0,
				     .layers = q->a ? layers : NULL};
		XrResult x = b->EndFrame(b->session, &ci);
		if (XR_FAILED(x)) {
			if (h->end_frame_reports++ < 2) {
				fprintf(stderr, "host: EndFrame failed=%d blend=%u flags=%llu views=%u\n", x, q->b,
				        (unsigned long long)q->flags, q->a);
				for (unsigned i = 0; i < q->a; i++) {
					const XrCompositionLayerProjectionView *v = &views[i];
					fprintf(stderr,
					        "host: view=%u rect=%d,%d %dx%d array=%u pose=%g,%g,%g,%g "
					        "fov=%g,%g,%g,%g\n",
					        i, v->subImage.imageRect.offset.x, v->subImage.imageRect.offset.y,
					        v->subImage.imageRect.extent.width, v->subImage.imageRect.extent.height,
					        v->subImage.imageArrayIndex, v->pose.orientation.x,
					        v->pose.orientation.y, v->pose.orientation.z, v->pose.orientation.w,
					        v->fov.angleLeft, v->fov.angleRight, v->fov.angleUp, v->fov.angleDown);
				}
			}
		}
		if (XR_SUCCEEDED(x)) {
			uint64_t n = now_ns();
			h->frames++;
			if (trace) {
				fprintf(trace, "frame,%llu,%llu,%llu,%lld,%d\n", h->frames, n,
					h->last_frame_ns ? n - h->last_frame_ns : 0, q->time, q->a);
				fflush(trace);
			}
			h->last_frame_ns = n;
		}
		return x;
	}
	case MWXR_OP_CREATE_SWAPCHAIN:
		if (q->object != SESSION_ID)
			return XR_ERROR_HANDLE_INVALID;
		return create_chain(h, q, r);
	case MWXR_OP_DESTROY_SWAPCHAIN: {
		struct host_swapchain *s = chain(h, q->object);
		if (!s)
			return XR_ERROR_HANDLE_INVALID;
		destroy_chain(s);
		return XR_SUCCESS;
	}
	case MWXR_OP_ACQUIRE: {
		struct host_swapchain *s = chain(h, q->object);
		if (!s)
			return XR_ERROR_HANDLE_INVALID;
		if (s->acquired != UINT32_MAX)
			return XR_ERROR_CALL_ORDER_INVALID;
		XrResult x = mwxr_native_swapchain_acquire(s->native, &r->a);
		if (XR_SUCCEEDED(x))
			s->acquired = r->a;
		return x;
	}
	case MWXR_OP_WAIT: {
		struct host_swapchain *s = chain(h, q->object);
		if (!s)
			return XR_ERROR_HANDLE_INVALID;
		return mwxr_native_swapchain_wait(s->native, q->time);
	}
	case MWXR_OP_RELEASE: {
		struct host_swapchain *s = chain(h, q->object);
		if (!s)
			return XR_ERROR_HANDLE_INVALID;
		if (s->acquired == UINT32_MAX)
			return XR_ERROR_CALL_ORDER_INVALID;
		XrResult x;
		struct mwxr_blit_timing timing = {0};
		if (s->staging[s->acquired])
			x = mwxr_native_swapchain_blit_release(s->native, s->acquired, s->staging[s->acquired],
							       h->event, q->flags, 10000000000ll, &timing);
		else {
			uint64_t start = now_ns();
			bool ready = [h->event waitUntilSignaledValue:q->flags timeoutMS:10000];
			timing.fence_wait_ns = now_ns() - start;
			x = ready ? mwxr_native_swapchain_release(s->native) : XR_ERROR_RUNTIME_FAILURE;
			timing.release_ns = now_ns();
		}
		if (x == XR_TIMEOUT_EXPIRED)
			x = XR_ERROR_RUNTIME_FAILURE;
		if (trace) {
			fprintf(trace, "release,%llu,%u,%llu,%llu,%.0f,%llu,%d,%llu,%llu,%.9f,%.9f\n", q->object,
				s->acquired, q->flags, timing.fence_wait_ns,
				(timing.gpu_end_seconds - timing.gpu_start_seconds) * 1e9, timing.release_ns, x,
				timing.copy_submit_ns, timing.copy_complete_ns, timing.gpu_start_seconds,
				timing.gpu_end_seconds);
			fflush(trace);
		}
		if (XR_SUCCEEDED(x) && x != XR_TIMEOUT_EXPIRED) {
			s->acquired = UINT32_MAX;
			r->time = timing.fence_wait_ns;
			r->period = (int64_t)((timing.gpu_end_seconds - timing.gpu_start_seconds) * 1e9);
		}
		return x;
	}
	case MWXR_OP_ATTACH_ACTION_SETS: {
		if (q->object != SESSION_ID)
			return XR_ERROR_HANDLE_INVALID;
		if (q->a > SET_LIMIT)
			return XR_ERROR_LIMIT_REACHED;
		XrActionSet sets[SET_LIMIT];
		for (unsigned i = 0; i < q->a; i++) {
			sets[i] = set(h, q->ids[i]);
			if (!sets[i])
				return XR_ERROR_HANDLE_INVALID;
		}
		XrSessionActionSetsAttachInfo ci = {
		    .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, .countActionSets = q->a, .actionSets = sets};
		return b->AttachSessionActionSets(b->session, &ci);
	}
	case MWXR_OP_SYNC_ACTIONS: {
		if (q->object != SESSION_ID)
			return XR_ERROR_HANDLE_INVALID;
		if (q->a > 32)
			return XR_ERROR_LIMIT_REACHED;
		XrActiveActionSet sets[32];
		for (unsigned i = 0; i < q->a; i++) {
			sets[i] = (XrActiveActionSet){set(h, q->ids[2 * i]), q->ids[2 * i + 1]};
			if (!sets[i].actionSet)
				return XR_ERROR_HANDLE_INVALID;
		}
		XrActionsSyncInfo ci = {
		    .type = XR_TYPE_ACTIONS_SYNC_INFO, .countActiveActionSets = q->a, .activeActionSets = sets};
		return b->SyncActions(b->session, &ci);
	}
	case MWXR_OP_ACTION_BOOLEAN:
	case MWXR_OP_ACTION_FLOAT:
	case MWXR_OP_ACTION_VECTOR:
	case MWXR_OP_ACTION_POSE: {
		if (q->object != SESSION_ID || !action(h, q->aux))
			return XR_ERROR_HANDLE_INVALID;
		XrActionStateGetInfo ci = {
		    .type = XR_TYPE_ACTION_STATE_GET_INFO, .action = action(h, q->aux), .subactionPath = q->flags};
		XrResult x;
		if (op == MWXR_OP_ACTION_BOOLEAN) {
			XrActionStateBoolean state = {.type = XR_TYPE_ACTION_STATE_BOOLEAN};
			x = b->GetActionStateBoolean(b->session, &ci, &state);
			r->a = state.currentState;
			r->b = state.changedSinceLastSync;
			r->time = state.lastChangeTime;
			r->c = state.isActive;
		} else if (op == MWXR_OP_ACTION_FLOAT) {
			XrActionStateFloat state = {.type = XR_TYPE_ACTION_STATE_FLOAT};
			x = b->GetActionStateFloat(b->session, &ci, &state);
			r->values[0] = state.currentState;
			r->b = state.changedSinceLastSync;
			r->time = state.lastChangeTime;
			r->c = state.isActive;
		} else if (op == MWXR_OP_ACTION_VECTOR) {
			XrActionStateVector2f state = {.type = XR_TYPE_ACTION_STATE_VECTOR2F};
			x = b->GetActionStateVector2f(b->session, &ci, &state);
			r->values[0] = state.currentState.x;
			r->values[1] = state.currentState.y;
			r->b = state.changedSinceLastSync;
			r->time = state.lastChangeTime;
			r->c = state.isActive;
		} else {
			XrActionStatePose state = {.type = XR_TYPE_ACTION_STATE_POSE};
			x = b->GetActionStatePose(b->session, &ci, &state);
			r->c = state.isActive;
		}
		return x;
	}
	case MWXR_OP_CURRENT_PROFILE: {
		if (q->object != SESSION_ID)
			return XR_ERROR_HANDLE_INVALID;
		XrInteractionProfileState state = {.type = XR_TYPE_INTERACTION_PROFILE_STATE};
		XrResult x = b->GetCurrentInteractionProfile(b->session, q->aux, &state);
		r->object = state.interactionProfile;
		return x;
	}
	case MWXR_OP_APPLY_HAPTIC:
	case MWXR_OP_STOP_HAPTIC: {
		if (q->object != SESSION_ID || !action(h, q->aux))
			return XR_ERROR_HANDLE_INVALID;
		XrHapticActionInfo ci = {
		    .type = XR_TYPE_HAPTIC_ACTION_INFO, .action = action(h, q->aux), .subactionPath = q->flags};
		if (op == MWXR_OP_STOP_HAPTIC)
			return b->StopHapticFeedback(b->session, &ci);
		float values[2];
		memcpy(values, &q->ids[0], sizeof(values));
		XrHapticVibration vib = {.type = XR_TYPE_HAPTIC_VIBRATION,
					 .duration = q->time,
					 .frequency = values[0],
					 .amplitude = values[1]};
		return b->ApplyHapticFeedback(b->session, &ci, (const void *)&vib);
	}
	default:
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	}
}
int main(int argc, char **argv) {
	if (argc != 2 || !mwxr_tcp_auth_token_valid(getenv("MWXR_RPC_TOKEN"))) {
		fprintf(stderr, "usage: MWXR_RPC_TOKEN=<64 lowercase hex chars> %s port\n", argv[0]);
		return 2;
	}
	char *end;
	long port = strtol(argv[1], &end, 10);
	if (*end || port < 1 || port > 65535)
		return 2;
	const char *trace_path = getenv("MWXR_TIMING_TRACE");
	if (trace_path) {
		trace = fopen(trace_path, "w");
		if (!trace) {
			perror("timing trace");
			return 1;
		}
	}
	signal(SIGPIPE, SIG_IGN);
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in addr = {
	    .sin_family = AF_INET, .sin_port = htons(port), .sin_addr = {htonl(INADDR_LOOPBACK)}};
	if (listener < 0 || bind(listener, (void *)&addr, sizeof(addr)) || listen(listener, 4)) {
		perror("host listen");
		return 1;
	}
	fprintf(stderr, "host: authenticated loopback port=%ld protocol=%u\n", port, MWXR_RPC_VERSION);
	for (;;) {
		int client = accept(listener, NULL, NULL);
		if (client < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		int no_delay = 1;
		setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
		struct timeval timeout = {15, 0};
		setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
		setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
		@autoreleasepool {
			struct host h = {0};
			uint32_t op, seq, previous = 0;
			struct mwxr_wire_request q;
			struct mwxr_wire_response r;
			if (mwxr_tcp_authenticate_server(client, getenv("MWXR_RPC_TOKEN"), 5000))
				while (!mwxr_rpc_receive(client, &op, &seq, &q)) {
					if (seq != previous + 1)
						break;
					previous = seq;
					memset(&r, 0, sizeof(r));
					// Fixed strings are validated before any native API sees them.
					if (!memchr(q.name, 0, sizeof(q.name)) || !memchr(q.label, 0, sizeof(q.label)))
						break;
					XrResult result;
					@autoreleasepool {
						result = dispatch(&h, op, &q, &r);
					}
					if (XR_FAILED(result))
						fprintf(stderr, "host: op=%u result=%d\n", op, result);
					if (mwxr_rpc_respond(client, op, seq, result, &r))
						break;
				}
			cleanup(&h);
		}
		close(client);
	}
	close(listener);
	return 1;
}

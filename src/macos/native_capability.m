// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "macos_wine_xr/native_capability.h"
#include <dxmt_native_capability.h>
#import <Foundation/Foundation.h>
#include <mach/mach.h>
#include <bsm/libbsm.h>
#include <servers/bootstrap.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern kern_return_t bootstrap_register2(mach_port_t, name_t, mach_port_t, uint64_t);

struct mwxr_native_capability {
	mach_port_t resource, broker;
	uint32_t kind;
	char name[128];
	pthread_t thread;
	bool started;
	atomic_bool closed;
};

static void *serve(void *opaque)
{
	struct mwxr_native_capability *cap = opaque;
	while (!atomic_load(&cap->closed)) {
		union {
			struct dxmt_native_cap_request request;
			unsigned char buffer[sizeof(struct dxmt_native_cap_request) + MAX_TRAILER_SIZE];
		} msg = {0};
		mach_msg_return_t kr = mach_msg(&msg.request.header,
		    MACH_RCV_MSG | MACH_RCV_TIMEOUT | MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |
		    MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT), 0, sizeof(msg), cap->broker, 50, MACH_PORT_NULL);
		if (kr == MACH_RCV_TIMED_OUT || kr == MACH_RCV_TOO_LARGE) continue;
		if (kr != MACH_MSG_SUCCESS) break;
		struct dxmt_native_cap_request *request = &msg.request;
		bool valid = request->header.msgh_size == sizeof(*request) &&
		    !(request->header.msgh_bits & MACH_MSGH_BITS_COMPLEX) &&
		    MACH_MSGH_BITS_REMOTE(request->header.msgh_bits) == MACH_MSG_TYPE_PORT_SEND_ONCE &&
		    request->header.msgh_id == DXMT_NATIVE_CAP_REQUEST_ID &&
		    request->version == DXMT_NATIVE_CAP_VERSION && request->kind == cap->kind;
		if (valid) {
			mach_msg_audit_trailer_t *trailer = (void *)(msg.buffer + request->header.msgh_size);
			valid = trailer->msgh_trailer_type == MACH_MSG_TRAILER_FORMAT_0 &&
			    trailer->msgh_trailer_size >= sizeof(*trailer) && audit_token_to_euid(trailer->msgh_audit) == geteuid();
		}
		if (!valid || atomic_load(&cap->closed)) { mach_msg_destroy(&request->header); continue; }
		struct dxmt_native_cap_reply reply = {
		    .header = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE,0) | MACH_MSGH_BITS_COMPLEX,
		               .msgh_size = sizeof(reply), .msgh_remote_port = request->header.msgh_remote_port,
		               .msgh_id = DXMT_NATIVE_CAP_REPLY_ID},
		    .body = {.msgh_descriptor_count = 1},
		    .resource = {.name = cap->resource, .disposition = MACH_MSG_TYPE_MOVE_SEND,
		                 .type = MACH_MSG_PORT_DESCRIPTOR},
		    .version = DXMT_NATIVE_CAP_VERSION, .kind = cap->kind,
		};
		// Give the reply its own right so mach_msg_destroy can clean up all
		// unconsumed rights if sending fails.
		if (mach_port_mod_refs(mach_task_self(),cap->resource,MACH_PORT_RIGHT_SEND,1) != KERN_SUCCESS) {
			mach_msg_destroy(&request->header); continue;
		}
		kr = mach_msg(&reply.header,MACH_SEND_MSG | MACH_SEND_TIMEOUT,sizeof(reply),0,MACH_PORT_NULL,1000,MACH_PORT_NULL);
		if (kr != MACH_MSG_SUCCESS) mach_msg_destroy(&reply.header);
	}
	return NULL;
}

int mwxr_native_capability_create(uint32_t resource, uint32_t kind, struct mwxr_native_capability **out)
{
	if (!out) return -1;
	*out = NULL;
	if (!resource || (kind != DXMT_NATIVE_CAP_TEXTURE && kind != DXMT_NATIVE_CAP_EVENT)) return -1;
	struct mwxr_native_capability *cap = calloc(1,sizeof(*cap));
	if (!cap) return -1;
	atomic_init(&cap->closed,false); cap->kind = kind;
	if (mach_port_mod_refs(mach_task_self(),resource,MACH_PORT_RIGHT_SEND,1) != KERN_SUCCESS) goto fail;
	cap->resource = resource;
	if (mach_port_allocate(mach_task_self(),MACH_PORT_RIGHT_RECEIVE,&cap->broker) != KERN_SUCCESS) goto fail;
	if (mach_port_insert_right(mach_task_self(),cap->broker,cap->broker,MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS) goto fail;
	@autoreleasepool {
		NSString *name = [@"dxmt-native-" stringByAppendingString:NSUUID.UUID.UUIDString];
		strlcpy(cap->name,name.UTF8String,sizeof(cap->name));
	}
	mach_port_t bp = MACH_PORT_NULL;
	if (task_get_bootstrap_port(mach_task_self(),&bp) != KERN_SUCCESS) goto fail;
	kern_return_t kr = bootstrap_register2(bp,cap->name,cap->broker,0);
	mach_port_deallocate(mach_task_self(),bp);
	if (kr != KERN_SUCCESS) goto fail;
	if (pthread_create(&cap->thread,NULL,serve,cap)) goto fail;
	cap->started = true;
	*out = cap;
	return 0;
fail:
	mwxr_native_capability_close(cap);
	return -1;
}

const char *mwxr_native_capability_name(const struct mwxr_native_capability *cap) { return cap ? cap->name : NULL; }

void mwxr_native_capability_close(struct mwxr_native_capability *cap)
{
	if (!cap) return;
	atomic_store(&cap->closed,true);
	if (cap->started) pthread_join(cap->thread,NULL);
	if (cap->broker) {
		mach_port_mod_refs(mach_task_self(),cap->broker,MACH_PORT_RIGHT_RECEIVE,-1);
		mach_port_deallocate(mach_task_self(),cap->broker);
	}
	if (cap->resource) mach_port_deallocate(mach_task_self(),cap->resource);
	free(cap);
}

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "macos_wine_xr/native_capability.h"
#include <dxmt_native_capability.h>
#include <mach/mach.h>
#include <servers/bootstrap.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr,"failed: %s:%d: %s\n",__FILE__,__LINE__,#condition); return 1; } } while (0)

static mach_port_t request(mach_port_t broker, uint32_t version, uint32_t kind, bool abandon)
{
	mach_port_t reply_port = MACH_PORT_NULL;
	if (mach_port_allocate(mach_task_self(),MACH_PORT_RIGHT_RECEIVE,&reply_port)) return MACH_PORT_NULL;
	union {
		struct dxmt_native_cap_request request;
		struct dxmt_native_cap_reply reply;
		unsigned char bytes[sizeof(struct dxmt_native_cap_reply)+MAX_TRAILER_SIZE];
	} msg = {0};
	msg.request = (struct dxmt_native_cap_request){
	    .header = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,MACH_MSG_TYPE_MAKE_SEND_ONCE),
	               .msgh_size = sizeof(msg.request), .msgh_remote_port = broker, .msgh_local_port = reply_port,
	               .msgh_id = DXMT_NATIVE_CAP_REQUEST_ID}, .version = version, .kind = kind};
	mach_msg_return_t result = mach_msg(&msg.request.header,MACH_SEND_MSG | MACH_SEND_TIMEOUT,
	    sizeof(msg.request),0,MACH_PORT_NULL,1000,MACH_PORT_NULL);
	if (result == MACH_MSG_SUCCESS && !abandon)
		result = mach_msg(&msg.reply.header,MACH_RCV_MSG | MACH_RCV_TIMEOUT,0,sizeof(msg),reply_port,1000,MACH_PORT_NULL);
	mach_port_mod_refs(mach_task_self(),reply_port,MACH_PORT_RIGHT_RECEIVE,-1);
	if (result != MACH_MSG_SUCCESS || abandon) return MACH_PORT_NULL;
	if (msg.reply.header.msgh_size != sizeof(msg.reply) || !(msg.reply.header.msgh_bits & MACH_MSGH_BITS_COMPLEX) ||
	    msg.reply.header.msgh_id != DXMT_NATIVE_CAP_REPLY_ID || msg.reply.version != version || msg.reply.kind != kind ||
	    msg.reply.body.msgh_descriptor_count != 1) {
		mach_msg_destroy(&msg.reply.header); return MACH_PORT_NULL;
	}
	return msg.reply.resource.name;
}

int main(void)
{
	mach_port_t resource = MACH_PORT_NULL, bp = MACH_PORT_NULL, broker = MACH_PORT_NULL;
	CHECK(mach_port_allocate(mach_task_self(),MACH_PORT_RIGHT_RECEIVE,&resource) == KERN_SUCCESS);
	CHECK(mach_port_insert_right(mach_task_self(),resource,resource,MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS);
	struct mwxr_native_capability *cap = NULL;
	CHECK(mwxr_native_capability_create(resource,DXMT_NATIVE_CAP_TEXTURE,&cap) == 0);
	CHECK(task_get_bootstrap_port(mach_task_self(),&bp) == KERN_SUCCESS);
	CHECK(bootstrap_look_up(bp,(char *)mwxr_native_capability_name(cap),&broker) == KERN_SUCCESS);
	mach_port_deallocate(mach_task_self(),bp);
	CHECK(!request(broker,DXMT_NATIVE_CAP_VERSION+1,DXMT_NATIVE_CAP_TEXTURE,false));
	CHECK(!request(broker,DXMT_NATIVE_CAP_VERSION,DXMT_NATIVE_CAP_EVENT,false));
	for (unsigned i = 0; i < 256; ++i) {
		mach_port_t imported = request(broker,DXMT_NATIVE_CAP_VERSION,DXMT_NATIVE_CAP_TEXTURE,false);
		CHECK(imported == resource);
		CHECK(mach_port_deallocate(mach_task_self(),imported) == KERN_SUCCESS);
		mach_port_urefs_t refs = 0;
		CHECK(mach_port_get_refs(mach_task_self(),resource,MACH_PORT_RIGHT_SEND,&refs) == KERN_SUCCESS && refs == 2);
	}
	// Failed reply sends must also release their duplicated resource right.
	for (unsigned i = 0; i < 64; ++i) request(broker,DXMT_NATIVE_CAP_VERSION,DXMT_NATIVE_CAP_TEXTURE,true);
	mach_port_t imported = request(broker,DXMT_NATIVE_CAP_VERSION,DXMT_NATIVE_CAP_TEXTURE,false);
	CHECK(imported == resource);
	CHECK(mach_port_deallocate(mach_task_self(),imported) == KERN_SUCCESS);
	mwxr_native_capability_close(cap);
	mach_port_urefs_t refs = 0;
	CHECK(mach_port_get_refs(mach_task_self(),resource,MACH_PORT_RIGHT_SEND,&refs) == KERN_SUCCESS && refs == 1);
	CHECK(!request(broker,DXMT_NATIVE_CAP_VERSION,DXMT_NATIVE_CAP_TEXTURE,false));
	mach_port_deallocate(mach_task_self(),broker);
	mach_port_mod_refs(mach_task_self(),resource,MACH_PORT_RIGHT_RECEIVE,-1);
	mach_port_deallocate(mach_task_self(),resource);
	puts("native capability version/type/lifetime/send-right tests passed");
	return 0;
}

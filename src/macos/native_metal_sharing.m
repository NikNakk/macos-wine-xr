// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#import "macos_wine_xr/native_metal_sharing.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <mach/mach.h>
#include <servers/bootstrap.h>
#include <string.h>

@interface MTLSharedTextureHandle (MacOSWineXRBootstrapSPI)
- (instancetype)initWithMachPort:(mach_port_t)port;
@end

@protocol MacOSWineXRSharedEventSPI <MTLDevice>
- (id<MTLSharedEvent>)newSharedEventWithMachPort:(mach_port_t)machPort;
@end

static int
lookup_bootstrap_port(const char *name, mach_port_t *out_port)
{
	if (name == NULL || name[0] == '\0' || out_port == NULL) {
		return -1;
	}
	*out_port = MACH_PORT_NULL;

	mach_port_t bootstrap_port = MACH_PORT_NULL;
	kern_return_t kr = task_get_bootstrap_port(mach_task_self(), &bootstrap_port);
	if (kr != KERN_SUCCESS || bootstrap_port == MACH_PORT_NULL) {
		return -2;
	}

	mach_port_t object_port = MACH_PORT_NULL;
	kr = bootstrap_look_up(bootstrap_port, (char *)name, &object_port);
	mach_port_deallocate(mach_task_self(), bootstrap_port);
	if (kr != KERN_SUCCESS || object_port == MACH_PORT_NULL) {
		return -3;
	}

	*out_port = object_port;
	return 0;
}

int
macos_wine_xr_resolve_shared_texture(const char *bootstrap_name,
                                     void *metal_device,
                                     void **out_texture)
{
	if (metal_device == NULL || out_texture == NULL) {
		return -1;
	}
	*out_texture = NULL;

	mach_port_t texture_port = MACH_PORT_NULL;
	int ret = lookup_bootstrap_port(bootstrap_name, &texture_port);
	if (ret != 0) {
		return ret;
	}

	@autoreleasepool {
		MTLSharedTextureHandle *handle = [[MTLSharedTextureHandle alloc] initWithMachPort:texture_port];
		mach_port_deallocate(mach_task_self(), texture_port);
		if (handle == nil) {
			return -4;
		}

		id<MTLDevice> device = (__bridge id<MTLDevice>)metal_device;
		id<MTLTexture> texture = [device newSharedTextureWithHandle:handle];
		[handle release];
		if (texture == nil) {
			return -5;
		}

		*out_texture = (__bridge_retained void *)texture;
		return 0;
	}
}

int
macos_wine_xr_resolve_shared_event(const char *bootstrap_name,
                                   void *metal_device,
                                   void **out_event)
{
	if (metal_device == NULL || out_event == NULL) {
		return -1;
	}
	*out_event = NULL;

	mach_port_t event_port = MACH_PORT_NULL;
	int ret = lookup_bootstrap_port(bootstrap_name, &event_port);
	if (ret != 0) {
		return ret;
	}

	@autoreleasepool {
		id<MacOSWineXRSharedEventSPI> device =
		    (id<MacOSWineXRSharedEventSPI>)(__bridge id<MTLDevice>)metal_device;
		id<MTLSharedEvent> event = [device newSharedEventWithMachPort:event_port];
		mach_port_deallocate(mach_task_self(), event_port);
		if (event == nil) {
			return -4;
		}

		*out_event = (__bridge_retained void *)event;
		return 0;
	}
}

void
macos_wine_xr_release_metal_object(void *object)
{
	if (object == NULL) {
		return;
	}
	CFRelease(object);
}

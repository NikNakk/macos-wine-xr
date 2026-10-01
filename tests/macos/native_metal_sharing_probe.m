// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#import "macos_wine_xr/native_metal_sharing.h"

#import <Metal/Metal.h>

#include <stdio.h>

int
main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s texture-bootstrap-name fence-bootstrap-name\n", argv[0]);
		return 2;
	}

	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	if (device == nil) {
		fprintf(stderr, "no default Metal device\n");
		return 1;
	}

	void *texture_ptr = NULL;
	int ret = macos_wine_xr_resolve_shared_texture(argv[1], (__bridge void *)device, &texture_ptr);
	if (ret != 0) {
		fprintf(stderr, "texture bootstrap resolution failed: %d\n", ret);
		[device release];
		return 1;
	}

	id<MTLTexture> texture = (__bridge id<MTLTexture>)texture_ptr;
	printf("texture: %lux%lu array=%lu type=%lu format=%lu\n",
	       (unsigned long)texture.width,
	       (unsigned long)texture.height,
	       (unsigned long)texture.arrayLength,
	       (unsigned long)texture.textureType,
	       (unsigned long)texture.pixelFormat);

	void *event_ptr = NULL;
	ret = macos_wine_xr_resolve_shared_event(argv[2], (__bridge void *)device, &event_ptr);
	if (ret != 0) {
		fprintf(stderr, "event bootstrap resolution failed: %d\n", ret);
		macos_wine_xr_release_metal_object(texture_ptr);
		[device release];
		return 1;
	}

	id<MTLSharedEvent> event = (__bridge id<MTLSharedEvent>)event_ptr;
	printf("event: signaledValue=%llu\n", (unsigned long long)event.signaledValue);

	macos_wine_xr_release_metal_object(event_ptr);
	macos_wine_xr_release_metal_object(texture_ptr);
	[device release];
	return 0;
}

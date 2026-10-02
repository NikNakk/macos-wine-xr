// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#import "macos_wine_xr/monado_handoff.h"
#import "macos_wine_xr/native_metal_sharing.h"

#import <Metal/Metal.h>

#include <stdio.h>

int
main(int argc, char **argv)
{
	if (argc < 3 || argc > 4) {
		fprintf(stderr,
		        "usage: %s texture-bootstrap-name fence-bootstrap-name [libmonado_metal_xpc_client.dylib]\n",
		        argv[0]);
		return 2;
	}

	const char *dylib_path = argc == 4 ? argv[3] : NULL;
	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	if (device == nil) {
		fprintf(stderr, "no default Metal device\n");
		return 1;
	}

	void *texture = NULL;
	int ret = macos_wine_xr_resolve_shared_texture(argv[1], (__bridge void *)device, &texture);
	if (ret != 0) {
		fprintf(stderr, "texture resolution failed: %d\n", ret);
		[device release];
		return 1;
	}

	void *event = NULL;
	ret = macos_wine_xr_resolve_shared_event(argv[2], (__bridge void *)device, &event);
	if (ret != 0) {
		fprintf(stderr, "event resolution failed: %d\n", ret);
		macos_wine_xr_release_metal_object(texture);
		[device release];
		return 1;
	}

	struct macos_wine_xr_monado_handoff *handoff = NULL;
	ret = macos_wine_xr_monado_handoff_open(dylib_path, &handoff);
	if (ret != 0) {
		fprintf(stderr, "Monado handoff dylib open failed: %d\n", ret);
		macos_wine_xr_release_metal_object(event);
		macos_wine_xr_release_metal_object(texture);
		[device release];
		return 1;
	}

	void *textures[1] = {texture};
	uint64_t texture_token = 0;
	ret = macos_wine_xr_monado_publish_textures(handoff, textures, 1, &texture_token);
	if (ret != 0) {
		fprintf(stderr, "Monado texture publication failed: %d\n", ret);
		macos_wine_xr_monado_handoff_close(handoff);
		macos_wine_xr_release_metal_object(event);
		macos_wine_xr_release_metal_object(texture);
		[device release];
		return 1;
	}

	uint64_t event_token = 0;
	ret = macos_wine_xr_monado_publish_shared_event(handoff, event, &event_token);
	if (ret != 0) {
		fprintf(stderr, "Monado shared-event publication failed: %d\n", ret);
		macos_wine_xr_monado_handoff_close(handoff);
		macos_wine_xr_release_metal_object(event);
		macos_wine_xr_release_metal_object(texture);
		[device release];
		return 1;
	}

	printf("texture_token=0x%016llx\n", (unsigned long long)texture_token);
	printf("event_token=0x%016llx\n", (unsigned long long)event_token);

	macos_wine_xr_monado_handoff_close(handoff);
	macos_wine_xr_release_metal_object(event);
	macos_wine_xr_release_metal_object(texture);
	[device release];
	return 0;
}

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#include "macos_wine_xr/monado_handoff.h"

#include <dlfcn.h>
#include <stdlib.h>

typedef int (*activate_service_fn)(void);
typedef int (*publish_textures_fn)(void *const *, uint32_t, uint64_t *);
typedef int (*publish_shared_event_fn)(void *, uint64_t *);

struct macos_wine_xr_monado_handoff
{
	void *dylib;
	activate_service_fn activate_service;
	publish_textures_fn publish_textures;
	publish_shared_event_fn publish_shared_event;
};

int
macos_wine_xr_monado_handoff_open(const char *dylib_path,
                                  struct macos_wine_xr_monado_handoff **out_handoff)
{
	if (out_handoff == NULL) {
		return -1;
	}
	*out_handoff = NULL;

	const char *path =
	    dylib_path != NULL && dylib_path[0] != '\0' ? dylib_path : "libmonado_metal_xpc_client.dylib";
	void *dylib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (dylib == NULL) {
		return -2;
	}

	activate_service_fn activate_service =
	    (activate_service_fn)dlsym(dylib, "monado_metal_xpc_activate_service");
	publish_textures_fn publish_textures =
	    (publish_textures_fn)dlsym(dylib, "monado_metal_xpc_publish_textures");
	publish_shared_event_fn publish_shared_event =
	    (publish_shared_event_fn)dlsym(dylib, "monado_metal_xpc_publish_shared_event");
	if (publish_textures == NULL || publish_shared_event == NULL || activate_service == NULL) {
		dlclose(dylib);
		return -3;
	}

	struct macos_wine_xr_monado_handoff *handoff =
	    (struct macos_wine_xr_monado_handoff *)calloc(1, sizeof(*handoff));
	if (handoff == NULL) {
		dlclose(dylib);
		return -4;
	}

	handoff->dylib = dylib;
	handoff->activate_service = activate_service;
	handoff->publish_textures = publish_textures;
	handoff->publish_shared_event = publish_shared_event;
	*out_handoff = handoff;
	return 0;
}

int
macos_wine_xr_monado_activate_service(struct macos_wine_xr_monado_handoff *handoff)
{
	if (handoff == NULL || handoff->activate_service == NULL) {
		return -1;
	}
	return handoff->activate_service();
}

void
macos_wine_xr_monado_handoff_close(struct macos_wine_xr_monado_handoff *handoff)
{
	if (handoff == NULL) {
		return;
	}
	if (handoff->dylib != NULL) {
		dlclose(handoff->dylib);
	}
	free(handoff);
}

int
macos_wine_xr_monado_publish_textures(struct macos_wine_xr_monado_handoff *handoff,
                                      void *const *metal_textures,
                                      uint32_t image_count,
                                      uint64_t *out_token)
{
	if (handoff == NULL || handoff->publish_textures == NULL) {
		return -1;
	}
	return handoff->publish_textures(metal_textures, image_count, out_token);
}

int
macos_wine_xr_monado_publish_shared_event(struct macos_wine_xr_monado_handoff *handoff,
                                         void *metal_shared_event,
                                         uint64_t *out_token)
{
	if (handoff == NULL || handoff->publish_shared_event == NULL) {
		return -1;
	}
	return handoff->publish_shared_event(metal_shared_event, out_token);
}

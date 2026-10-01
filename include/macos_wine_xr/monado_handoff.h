// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Load Monado's small external Metal XPC client dylib and publish native Metal
 * resources into the running service's broker. The dylib path may be supplied
 * explicitly; otherwise dyld's normal search path is used.
 */
struct macos_wine_xr_monado_handoff;

int
macos_wine_xr_monado_handoff_open(const char *dylib_path,
                                  struct macos_wine_xr_monado_handoff **out_handoff);

void
macos_wine_xr_monado_handoff_close(struct macos_wine_xr_monado_handoff *handoff);

int
macos_wine_xr_monado_publish_textures(struct macos_wine_xr_monado_handoff *handoff,
                                      void *const *metal_textures,
                                      uint32_t image_count,
                                      uint64_t *out_token);

int
macos_wine_xr_monado_publish_shared_event(struct macos_wine_xr_monado_handoff *handoff,
                                         void *metal_shared_event,
                                         uint64_t *out_token);

#ifdef __cplusplus
}
#endif

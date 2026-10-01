// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Resolve a bootstrap-registered MTLSharedTextureHandle and reopen it on the
 * supplied id<MTLDevice>. Both Objective-C objects are passed as void * so the
 * public C ABI does not require Objective-C syntax.
 *
 * On success *out_texture owns one Objective-C reference. Release it with
 * macos_wine_xr_release_metal_object().
 */
int
macos_wine_xr_resolve_shared_texture(const char *bootstrap_name,
                                     void *metal_device,
                                     void **out_texture);

/*
 * Resolve a bootstrap-registered MTLSharedEvent and reopen it on the supplied
 * id<MTLDevice>. On success *out_event owns one Objective-C reference.
 */
int
macos_wine_xr_resolve_shared_event(const char *bootstrap_name,
                                   void *metal_device,
                                   void **out_event);

void
macos_wine_xr_release_metal_object(void *object);

#ifdef __cplusplus
}
#endif

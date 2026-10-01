// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <stdint.h>
struct mwxr_native_capability;

/* Duplicate a borrowed send right and publish a revocable broker for it.
 * kind is DXMT_NATIVE_CAP_TEXTURE/EVENT from DXMT's public native contract.
 * The owner must close the broker before releasing its texture/swapchain.
 * Requests require a kernel-verified matching effective UID. Names are random
 * capabilities; only distribute them over the authenticated bridge channel. */
int mwxr_native_capability_create(uint32_t resource_port, uint32_t kind,
                                  struct mwxr_native_capability **out_capability);
const char *mwxr_native_capability_name(const struct mwxr_native_capability *capability);
void mwxr_native_capability_close(struct mwxr_native_capability *capability);

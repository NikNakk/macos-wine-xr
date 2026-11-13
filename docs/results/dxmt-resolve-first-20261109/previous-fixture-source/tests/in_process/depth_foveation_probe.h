// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <stdint.h>
enum { DEPTH_CREATE, DEPTH_RECONSTRUCT, DEPTH_RELEASE };
struct depth_foveation_params {
 uint64_t device, color, depth, event, map, value, output_color, output_depth;
 uint32_t size, samples, mapped, filter, physical_width, physical_height, status;
 double reconstruction_gpu_ms;
};

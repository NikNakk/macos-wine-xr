// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <stdint.h>
enum { DEPTH_CREATE, DEPTH_RECONSTRUCT, DEPTH_RELEASE, DEPTH_RESOLVE_FIRST, DEPTH_COMPRESS_PREPASS, DEPTH_AXIS_LUT, DEPTH_SAMPLE_AXIS_LUT };
struct depth_foveation_params {
 uint64_t device, color, depth, event, map, value, output_color, output_depth;
 uint32_t size, samples, mapped, filter, physical_width, physical_height, status;
 double reconstruction_gpu_ms;
 float peripheral_rate;
 double resolve_gpu_ms, unwarp_gpu_ms;
 uint64_t source_depth; // test-only full-resolution prepass source
 uint64_t axis_lut; // caller-owned CPU floats: 2*size for AXIS_LUT; samples*(2*size+2) for SAMPLE_AXIS_LUT

};

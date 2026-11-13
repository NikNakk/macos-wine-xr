// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <stdint.h>

enum { FOVEATION_CREATE, FOVEATION_VERIFY, FOVEATION_RELEASE_MAP, FOVEATION_RELEASE, FOVEATION_DESCRIBE,
       FOVEATION_RECONSTRUCT };
#ifndef FOVEATION_PROBE_SIZE
#define FOVEATION_PROBE_SIZE 512
#endif
#define FOVEATION_PROBE_SAMPLES 25
struct foveation_probe_sample { uint32_t x, y, expected; };
struct foveation_probe_params {
    uint64_t device, texture, event, map, value;
    uint32_t mode, physical_width, physical_height, status, checked;
    struct foveation_probe_sample samples[FOVEATION_PROBE_SAMPLES];
    double reconstruction_gpu_ms;
    double reconstruction_cpu_ms;
};

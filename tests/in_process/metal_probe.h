// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <stdint.h>
struct metal_probe_params {
    uint64_t device, texture, event, value;
    uint32_t arrays, slice, timeout_ms, expected_pixel;
    uint32_t pixel, status;
};

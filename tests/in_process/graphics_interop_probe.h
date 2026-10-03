// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Test-only native operations for the graphics interop probe. Unix call 0 is
// the production interop helper; the probe's own operations follow it.
#pragma once
#include <stdint.h>

enum { PROBE_NATIVE_INTEROP, PROBE_CREATE, PROBE_VERIFY, PROBE_FILL, PROBE_RELEASE };
enum { PROBE_PRIVATE_2D, PROBE_PRIVATE_ARRAY, PROBE_IOSURFACE_2D, PROBE_EVENT };

struct graphics_probe_params {
    uint64_t device, object, event, value;
    uint32_t kind, slice, timeout_ms, pixel_in;
    uint32_t pixel_out, status;
};

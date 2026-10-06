// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

namespace mwxr {

// Writes to SteamVR's vrserver log once the driver context exists.
void
Log(const char *format, ...) __attribute__((format(printf, 1, 2)));

} // namespace mwxr

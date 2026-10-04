// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <stdint.h>

// A reused completion event may still hold a previous fence's notification.
// Only the fence value proves completion; notifications merely wake the loop.
template<typename Completed, typename Wait, typename Clock>
bool mw_wait_for_fence(uint64_t value, uint32_t timeout_ms, Completed completed, Wait wait, Clock clock)
{
    const uint64_t start = clock();
    while (completed() < value) {
        const uint64_t elapsed = clock() - start;
        if (elapsed >= timeout_ms || !wait(timeout_ms - (uint32_t)elapsed)) return false;
    }
    return true;
}

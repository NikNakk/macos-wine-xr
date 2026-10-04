// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
#include "../../src/in_process/fence_wait.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main()
{
    uint64_t completed = 1, now = 0;
    unsigned waits = 0;
    bool wrong_timeout = false;
    bool ok = mw_wait_for_fence(2, 5000, [&] { return completed; },
        [&](uint32_t remaining) {
            wrong_timeout |= remaining != 5000 - now;
            ++waits; now += 10;
            // First wake consumes a stale notification for fence 1.
            if (waits == 2) completed = 2;
            return true;
        }, [&] { return now; });
    CHECK(ok && waits == 2 && completed == 2 && !wrong_timeout);
    waits = 0;
    ok = mw_wait_for_fence(2, 5000, [&] { return completed; },
        [&](uint32_t) { ++waits; return true; }, [&] { return now; });
    CHECK(ok && waits == 0);
    completed = 1; now = 0; waits = 0;
    ok = mw_wait_for_fence(2, 30, [&] { return completed; },
        [&](uint32_t) { ++waits; now += 10; return true; }, [&] { return now; });
    CHECK(!ok && waits == 3 && completed == 1);
    ok = mw_wait_for_fence(2, 5000, [&] { return completed; },
        [&](uint32_t) { return false; }, [&] { return now; });
    CHECK(!ok);
    puts("PASS stale fence notifications, completed fence and bounded timeout");
    return 0;
}

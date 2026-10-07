<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# SteamVR Home Workshop cycle guard

An opt-in Windows x64 debugger helper for the Home dependency traversal studied
in `docs/steamvr-home.md`. It attaches to `steamtours.exe`, checks the complete
`client.dll` SHA-256 and function prologue, and applies a process-memory-only
breakpoint. It does not change Home files or published Workshop metadata and
contains no item-ID exclusion list. This is an experimental workaround for a
specific closed-source binary, not a general Steam API fix.

Supported `client.dll` SHA-256:
`c331ca4fc37723dd496d06930049a5f847433aa169b70943a4511b080fa16f96`.
Other DLLs are refused. Do not update the hash/offset without inspecting and
validating that build's function and calling convention.

## Build

From this repository's root, with x64 MinGW:

```sh
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -Werror \
  -Wno-misleading-indentation -Wno-cast-function-type -static \
  tools/workshop_guard/main.cpp -lbcrypt -o workshop-guard.exe
```

Or in an x64 Native Tools Command Prompt for Visual Studio:

```bat
cl /nologo /std:c++17 /EHsc /W4 tools\workshop_guard\main.cpp bcrypt.lib /Fe:workshop-guard.exe
```

## Run

Start the guard **before launching Home**, in another terminal:

```powershell
.\workshop-guard.exe
```

It waits up to 90 seconds for a unique `steamtours.exe` process. Keep it running
while Home runs. An explicit Windows process ID avoids ambiguity when there
are multiple instances. A second numeric argument optionally limits the
attached observation period in seconds:

```powershell
.\workshop-guard.exe 1234 300
```

Under Wine, use the same Wine runtime and prefix as Home, for example from the
Monado workspace after compiling into `.build/steamvr-cycle-guard`:

```sh
WINEPREFIX="$PWD/.build/steamvr-dxmt/prefix" WINEDEBUG=-all \
.build/steamvr-dxmt/wine-11.10/bin/wine \
  .build/steamvr-cycle-guard/workshop-guard.exe 0 600 \
  > .build/steamvr-cycle-guard/guard.log 2>&1
```

Then start the normal simulated SteamVR launcher. PID `0` means automatic
process discovery. The guard must attach before dependency recursion begins;
it cannot repair a stack already filled with recursive calls. Only one
debugger can attach to Home at a time. `ARMED` confirms installation; a
hash/prologue refusal means no guard is active.

Press Ctrl+C for orderly restoration/detach, or stop Home first. Do not forcibly
kill the helper while Home continues running: outstanding return addresses
refer to its debugger-managed return gate. After detach the original traversal
is restored and may encounter the cycle again. This debugger approach adds
per-call overhead; it is a diagnostic workaround, not a validated production
performance solution. Native Windows execution of the guard still requires
validation, even though the helper is built with Windows APIs.

## Operation and logs

The helper tracks `(manager pointer, published item ID)` on the active call
path **per thread**. Revisiting that pair on the same path returns state 0 and
return value 0, matching the previously tested unavailable-item result. Calls
on other paths/threads and shared acyclic dependencies are permitted. A depth
limit of 128 also prevents an unexpectedly deep traversal exhausting the stack.

It emulates the function's first complete instruction and substitutes a
return-gate address for each permitted call. The gate removes that call from
the tracked path before continuing at its original caller. Keeping the entry
breakpoint armed avoids a single-step window where another thread could enter
without tracking. All edits occur with target threads stopped by a debug event.
On orderly detach, the original entry byte and outstanding return slots are
restored. Exception-unwound frames are pruned on the next function entry;
this is not a substitute for complete exception/unwind instrumentation.

`BLOCKED reason=cycle` prints the active dependency path (first 20 occurrences).
`STATUS` / `SUMMARY` report permitted and blocked traversal traffic, and
exceptions are reported without suppressing non-breakpoint exceptions. A
process exit, timeout or Ctrl+C ends the helper. Log counts alone do not prove
rendering: pair them with the OpenVR shim's sustained Submit windows.

The accompanying bug-report draft is
`docs/bug-reports/steamvr-home-workshop-cycle.md`; it has not been submitted.

## Repeatable debugger tests

With x64 MinGW and Python installed, run from this repository's root:

```sh
python3 tools/workshop_guard/test/run.py \
  --wine /absolute/path/to/wine \
  --prefix /absolute/path/to/test-prefix \
  --out /absolute/path/to/test-output
```

The runner builds a separate fixture DLL and a **fixture-only** helper with
that DLL's computed hash and function RVA. It uses actual debug events and
return addresses to test a shared acyclic dependency, a cycle, the depth limit,
32 completed traversals on four threads, and detach during an active call.
It also verifies a changed DLL hash is refused while the unpatched fixture
continues normally. Fixture helpers do not target Home and must not be used
as the production helper. These tests run under Wine; native Windows validation
is still separate.

The Wine graph suite passed with 393 intercepted calls, 34 blocked calls and
359 normal returns. The active-call detach test restored the pending return
slot and the fixture completed after detach. Home accepted the checked entry
and ordinary calls returned, but the current live run did not encounter the
published cycle: independent fresh and cache-allowed UGC requests both timed
out after 30 seconds. Do not interpret that run as proof that Home's actual
cycle has been blocked or that frame pacing is acceptable.

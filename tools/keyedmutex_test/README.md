# Keyed-mutex ping-pong

Checks that a Wine build's D3DKMT keyed mutexes hand over between processes.

```sh
x86_64-w64-mingw32-gcc -O2 -o keyedmutex_pingpong.exe keyedmutex_pingpong.c
WINEPREFIX=... wine keyedmutex_pingpong.exe 2000
```

Under CrossOver 26.3 with `WINEMSYNC=1`, stock wineserver hangs on the first
cross-process acquire; `patches/crossover/0001-*.patch` fixes it.

## msync wait leak

`msync_wait_leak.c` waits worker-style on {never-signalled event, busy
semaphore} from 8 threads while 4 threads release the semaphore. Under stock
CrossOver 26.3 with `WINEMSYNC=1`, wineserver printed "msync: warn: node
memory pool exhausted" 12.9 million times in 60 s; with
`patches/crossover/0002-*.patch`, it printed none, used 16 MB rather than 79 MB,
and satisfied 68% more waits.

```sh
x86_64-w64-mingw32-gcc -O2 -o msync_wait_leak.exe msync_wait_leak.c
WINEPREFIX=... WINEMSYNC=1 wine msync_wait_leak.exe 60
```


## Ownership-history diagnostics

`patches/crossover/0006-server-keyed-mutex-ownership-history.patch` adds a
64-event per-mutex ring in wineserver. It prints sampled timeouts/errors with
caller and owner PID/TID, hold duration, key/fence, waiter counts and the
first queued signal target. It changes diagnostics only; it does not reserve
ownership for a woken waiter. The server announces `mwxr-keyed-history active`.
Fully exit the prefix before testing a replaced server. Its stderr goes to
the log of whichever launcher first starts that prefix's server.

Build `keyedmutex_owner_history.c` as for the ping-pong fixture above. It
forces two child timeouts while the parent holds key 0, then recovers for
2,000 handoffs per process and forces an unowned wrong-key timeout.
`keyedmutex_samekey_contenders.c` runs the parent and two child processes
against key 0; all must complete. The 2026-10-10 test with 10,000 rounds each
had no timeouts or failures (slowest 0.80 ms), which does not reproduce
SteamVR's GPU workload. Wine history tests and a 200-frame DXMT GPU handoff
pass; evidence is in Monado `doc/results/wine-keyed-owner-history-20261010/`.


## Home ownership stacks

The minimal DXMT diagnostic build now logs `KeyedMutex ownership interval`
with acquire setup, application ownership interval, release signal enqueue
and release handoff durations. Match PID/shared handle/release fence and tick
counts to the server's ownership history. The application interval includes
CPU work, waiting and scheduling; it is not a CPU or GPU timer.

While Home is running, capture an early sample and another when black frames
become more frequent, with separate new output directories:

```sh
python3 tools/keyedmutex_test/capture_home_stacks.py --output /tmp/home-owner-early
python3 tools/keyedmutex_test/capture_home_stacks.py --output /tmp/home-owner-late
```

Each samples for five seconds at 10 ms intervals. `--pid` accepts the macOS
process ID when the process name cannot be uniquely detected. `--wine-log`
can also retain filtered diagnostics from the prefix's first-launch stderr
log. Sampling does not launch/stop applications, but can perturb timing;
use these for attribution, not a clean pacing benchmark. The helper smoke
test passes on an isolated sleep process; headset captures are pending.


The next-launch minimal DXMT also emits `KeyedMutex owned waits` for context
lock, flush, command capacity, CPU fence and frame-latency intervals inside
ownership. Join by PID/handle/release fence. These device-wide categories can
overlap; do not add them or interpret the remainder as CPU rendering time.
The capture helper retains these records and recognizes Windows-backslash
process names as well as POSIX names. Native Home samples have many unresolved
Windows frames, so use the producer wait counters for further attribution.

`keyedmutex_owned_waits.cpp` verifies timeout recovery, cross-thread release,
controlled context contention and 200 GPU pixel handoffs, with a first-round
flush/readback interval deliberately lengthened so bounded diagnostics appear:

```sh
x86_64-w64-mingw32-g++ -O2 -std=c++17 -static keyedmutex_owned_waits.cpp -o keyedmutex_owned_waits.exe -ld3d11 -ldxgi -lole32 -luuid
WINEPREFIX=/path/to/isolated-prefix /path/to/matched-runtime/bin/wine keyedmutex_owned_waits.exe
```

Expect successful checks, a context-lock attribution around the deliberate
35 ms hold (scheduling can extend it), and flush/capacity/CPU-fence call counts.
Frame-latency ownership attribution needs a presentation workload separately.
Restart Steam/SteamVR for the installed 2026-10-11 build before a hardware run.


The 2026-10-11 presentation update adds `swapchain_latency_ms/calls` and
`present_ms/calls` to the same records. The former measures the waitable
swapchain's separate SyncFrame fence; the latter includes the whole Present1
API call. Existing frame-latency fields cover queue pacing. Present overlaps
narrower categories, so do not add them. The fixture now includes six actual
presents while owning a keyed texture, asserting six Present calls and five
swapchain-wait calls; controlled contention/recovery and all 200 GPU pixel
handoffs still pass. New-build hardware attribution needs Steam/SteamVR restart.

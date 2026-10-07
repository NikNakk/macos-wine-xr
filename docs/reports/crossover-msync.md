# Draft reports for CodeWeavers: two msync bugs in CrossOver 26.3

Not filed. Each section below is one report. Attach the files it names from
this repository.

Environment for both: CrossOver 26.3.0 FOSS sources
(`crossover-sources-26.3.0.tar.gz`), Wine built x86_64-only, run under Rosetta
with `WINEMSYNC=1`. macOS 26.6.2 (25G83), Apple M5.

---

## 1. msync leaks wait registrations: "node memory pool exhausted"

**Summary.** Under msync, `server/msync.c` keeps a per-object list of waiting
thread IDs, taken from a pool of 524,288 nodes. Two paths leave entries on
those lists. On objects that are rarely or never signalled, the entries
accumulate until the pool is exhausted. The server then prints `msync: warn:
node memory pool exhausted` and falls back to `malloc`, and the lists grow
without bound. Each unregister walks the list (`remove_tid`), so the
wineserver also slows down the longer a prefix runs.

**Where entries leak.**

1. Client, `dlls/ntdll/unix/msync.c`, `msync_wait_multiple()`. While the
   client spins waiting for the server to finish registering
   (`shm_tid_map[tid] == 2`), it returns `STATUS_PENDING` as soon as
   `check_shm_contention()` sees a signalled object. It decrements
   `multiple_waiters` but never sends the removal message. If the server has
   already registered some or all of the objects, those entries stay behind.
2. Server, `server/msync.c`, `mach_message_pump()`. When object `i` is found
   already signalled during registration, objects `0 … i - 1` have been
   registered, but the removal is guarded by `if (i > 1)`. For `i == 1`,
   object 0 keeps its entry.

A stale entry also gets woken later by an unrelated signal of that object
(`wake_tid`), which can produce a spurious wake-up of a thread that is by
then waiting on something else.

**Typical trigger.** Worker threads that call
`WaitForMultipleObjects({shutdown_event, job_semaphore}, FALSE, timeout)`
while other threads release the semaphore at a high rate. Source 2 games
(SteamVR Home, Half-Life: Alyx) do this. The shutdown event is never
signalled, so every abandoned registration on it is permanent. In a SteamVR
session the warning appeared after a few hours, and the game's CPU use
dropped sharply after restarting the prefix.

**Reproducer.** `tools/keyedmutex_test/msync_wait_leak.c` uses 8 threads
waiting as above and 4 threads releasing the semaphore.

```sh
x86_64-w64-mingw32-gcc -O2 -o msync_wait_leak.exe msync_wait_leak.c
WINEMSYNC=1 wine msync_wait_leak.exe 60
```

| Build | "pool exhausted" warnings in 60 s | wineserver RSS at end | Waits satisfied |
| --- | --- | --- | --- |
| Stock 26.3 | 12,922,954 | 79 MB | 7,139,486 |
| Patched | 0 | 16 MB | 11,970,570 |

**Fix.** `patches/crossover/0002-msync-remove-abandoned-wait-registrations.patch`:

- the client sends `server_remove_wait()` when it abandons a registration
  (it also decrements `multiple_waiters`, as before);
- the server's early exit unregisters every object it registered (`i > 0`).

Removing an entry that is not present is already a no-op in `remove_tid`, so
a removal that races with the server's own early exit is harmless. The
patched build also passes the keyed-mutex test in report 2.

---

## 2. Cross-process D3DKMT keyed mutex acquire hangs under msync

**Summary.** With `WINEMSYNC=1`, the first cross-process
`D3DKMTAcquireKeyedMutex` that has to wait never returns. Without msync it
works. SteamVR hits this at once: its compositor waits on the backbuffer key
released by its driver process.

**Cause.** In `server/d3dkmt.c`, a keyed-mutex wait creates its sync object
with `create_internal_sync()`. Under msync, the client waits on such internal
syncs in-process (as `MSYNC_AUTO_SERVER` objects). But the keyed mutex only
signals the wait from the server side, so the in-process wait is never woken.

**Reproducer.** `tools/keyedmutex_test/keyedmutex_pingpong.c`. A parent and a
child process pass a shared keyed mutex back and forth (acquire key 0,
release key 1, and so on), with a 1 s timeout on each acquire.

```sh
x86_64-w64-mingw32-gcc -O2 -o keyedmutex_pingpong.exe keyedmutex_pingpong.c
WINEMSYNC=1 wine keyedmutex_pingpong.exe 2000
```

- Stock 26.3: the first wait hangs (the test's 1 s timeout is not honoured
  either).
- Patched: 2,000 rounds with no timeouts or failures; 5,000 rounds at about
  0.05 ms each.

**Fix.** `patches/crossover/0001-server-keyed-mutex-waits-on-server-syncs.patch`
creates the wait sync with `create_server_internal_sync()`, as
`server/debugger.c` already does for objects that only the server signals.

Note: I have not checked whether upstream Wine with ntsync behaves the same
way.

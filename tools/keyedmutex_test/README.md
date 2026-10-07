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

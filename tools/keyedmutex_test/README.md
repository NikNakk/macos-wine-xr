# Keyed-mutex ping-pong

Checks that a Wine build's D3DKMT keyed mutexes hand over between processes.

```sh
x86_64-w64-mingw32-gcc -O2 -o keyedmutex_pingpong.exe keyedmutex_pingpong.c
WINEPREFIX=... wine keyedmutex_pingpong.exe 2000
```

Under CrossOver 26.3 with `WINEMSYNC=1`, stock wineserver hangs on the first
cross-process acquire; `patches/crossover/0001-*.patch` fixes it.

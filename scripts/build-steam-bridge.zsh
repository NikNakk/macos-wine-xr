#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Builds the Wine Steam bridge (docs/steam-bridge.md): the builtin
# steamclient64.dll with its native half, the stand-in for the Windows Steam
# client, and the smoke test. Output, laid out for WINEDLLPATH:
#   ${MWXR_STEAM_BRIDGE_BUILD}/x86_64-windows/steamclient64.dll
#   ${MWXR_STEAM_BRIDGE_BUILD}/x86_64-unix/steamclient64.so
#   ${MWXR_STEAM_BRIDGE_BUILD}/steam_standin.exe, steam_bridge_smoke.exe
set -euo pipefail
repo=${0:A:h:h}
: ${MWXR_WINE_SDK:?Wine SDK containing bin/winebuild, include/wine/windows and lib/wine/x86_64-windows}
: ${MWXR_WINE_SOURCE:?Wine source tree (include/wine/debug.h, list.h, unixlib.h)}
root=${MWXR_STEAM_BRIDGE_BUILD:-${repo}/build-steam-bridge}
src=${repo}/src/steam_bridge
mkdir -p "${root}/support/wine" "${root}/x86_64-windows" "${root}/x86_64-unix"
for header in debug.h list.h unixlib.h; do
 ln -sf "${MWXR_WINE_SOURCE}/include/wine/${header}" "${root}/support/wine/${header}"
done
defs=(-D__WINESRC__ -D_WIN64 -DWINE_NO_LONG_TYPES)
inc=(-I"${root}/support" -I"${MWXR_WINE_SDK}/include/wine/windows" -I"${src}")
pe_lib=${MWXR_WINE_SDK}/lib/wine/x86_64-windows

# Windows half: a Wine builtin, so Wine finds its native half beside it.
x86_64-w64-mingw32-gcc -shared -std=gnu11 -O2 -Wall "${defs[@]}" "${inc[@]}" \
 "${src}/steamclient_main.c" "${src}/bridge_core.c" "${src}/stubs.S" "${src}/steamclient64.def" \
 "${pe_lib}/libwinecrt0.a" "${pe_lib}/libntdll.a" -lkernel32 \
 -o "${root}/x86_64-windows/steamclient64.dll"
"${MWXR_WINE_SDK}/bin/winebuild" --builtin "${root}/x86_64-windows/steamclient64.dll"

# Native half: x86_64, like the Wine it runs in and the Steam client slice it loads.
clang -arch x86_64 -shared -std=gnu11 -O2 -Wall -DWINE_UNIX_LIB "${defs[@]}" "${inc[@]}" \
 "${src}/steamclient_unix.c" "${src}/sysv_invoke.S" \
 -Wl,-install_name,@rpath/steamclient64.so -o "${root}/x86_64-unix/steamclient64.so"

x86_64-w64-mingw32-gcc -std=gnu11 -O2 -Wall -municode -static "${src}/steam_standin.c" -luser32 -ladvapi32 \
 -o "${root}/steam_standin.exe"
x86_64-w64-mingw32-gcc -std=gnu11 -O2 -Wall -static "${repo}/tests/steam_bridge/steam_bridge_smoke.c" \
 -o "${root}/steam_bridge_smoke.exe"
print "Built the Steam bridge in ${root}"

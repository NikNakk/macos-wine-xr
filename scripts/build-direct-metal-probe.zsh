#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=${0:A:h:h}
: ${DXMT_SOURCE_DIR:?DXMT source with IDXMTNativeDevice2}
: ${MWXR_WINE_SDK:?Wine SDK}
: ${MWXR_WINE_RUNTIME:?Wine runtime}
root=${MWXR_IN_PROCESS_BUILD:-${repo}/build-in-process/gate}
inc=(-I"${root}/support" -I"${MWXR_WINE_SDK}/include/wine/windows")
x86_64-w64-mingw32-gcc -shared -D__WINESRC__ -D_WIN64 -DWINE_NO_LONG_TYPES "${inc[@]}" \
 "${repo}/tests/in_process/metal_probe_pe.c" \
 "${MWXR_WINE_SDK}/lib/wine/x86_64-windows/libwinecrt0.a" \
 "${MWXR_WINE_SDK}/lib/wine/x86_64-windows/libntdll.a" -lkernel32 \
 -Wl,--export-all-symbols -o "${root}/x86_64-windows/wine_metal_probe.dll"
"${MWXR_WINE_SDK}/bin/winebuild" --builtin "${root}/x86_64-windows/wine_metal_probe.dll"
clang -arch x86_64 -shared -D__WINESRC__ -D_WIN64 -DWINE_NO_LONG_TYPES -DWINE_UNIX_LIB "${inc[@]}" \
 "${repo}/tests/in_process/metal_probe_native.m" -framework Foundation -framework Metal \
 -o "${root}/x86_64-unix/wine_metal_probe.so"
x86_64-w64-mingw32-g++ -static -I"${DXMT_SOURCE_DIR}/include" \
 -I"${DXMT_SOURCE_DIR}/include/native/directx" "${repo}/tests/windows/direct_metal_import.cpp" \
 -ld3d11 -ldxguid -o "${root}/direct_metal_import.exe"

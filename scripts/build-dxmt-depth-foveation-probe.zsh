#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=${0:A:h:h}
: ${DXMT_SOURCE_DIR:?Private DXMT source with the explicit attachment probe patch}
: ${MWXR_WINE_SDK:?Wine SDK}
: ${MWXR_WINE_SOURCE:?Matching Wine source}
root=${MWXR_DEPTH_PROBE_BUILD:-${repo}/build-in-process/depth-foveation-probe}
mkdir -p "${root}/x86_64-windows" "${root}/x86_64-unix"
inc=(-I"${MWXR_WINE_SOURCE}/include" -I"${MWXR_WINE_SDK}/include/wine/windows")
x86_64-w64-mingw32-gcc -shared -D__WINESRC__ -D_WIN64 -DWINE_NO_LONG_TYPES "${inc[@]}" \
 "${repo}/tests/in_process/metal_probe_pe.c" \
 "${MWXR_WINE_SDK}/lib/wine/x86_64-windows/libwinecrt0.a" \
 "${MWXR_WINE_SDK}/lib/wine/x86_64-windows/libntdll.a" -lkernel32 \
 -Wl,--export-all-symbols -o "${root}/x86_64-windows/wine_depth_foveation_probe.dll"
"${MWXR_WINE_SDK}/bin/winebuild" --builtin "${root}/x86_64-windows/wine_depth_foveation_probe.dll"
clang -arch x86_64 -shared "${repo}/tests/in_process/depth_foveation_probe_native.m" \
 -framework Foundation -framework Metal -o "${root}/x86_64-unix/wine_depth_foveation_probe.so"
x86_64-w64-mingw32-g++ -O2 -static -I"${DXMT_SOURCE_DIR}/include" -I"${DXMT_SOURCE_DIR}/include/native/directx" \
 "${repo}/tests/windows/dxmt_depth_foveation_probe.cpp" -ld3d11 -ld3dcompiler_47 -ldxguid -o "${root}/dxmt_depth_foveation_probe.exe"
print "Built ${root}/dxmt_depth_foveation_probe.exe"

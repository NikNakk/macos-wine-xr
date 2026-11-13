#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# Builds the stay-warped read-translation fixture and its rate-map helper.
set -euo pipefail
repo=${0:A:h:h}
: ${DXMT_SOURCE_DIR:?DXMT source with IDXMTNativeContext1}
: ${MWXR_WINE_SDK:?Wine SDK}
: ${MWXR_WINE_SOURCE:?Matching Wine source headers}
root=${MWXR_WARP_READ_BUILD:-${repo}/build-in-process/warp-read-probe}
size=${MWXR_FOVEATION_PROBE_SIZE:-2048}
case ${size} in 512|1024|2048) ;; *) print -u2 "Probe size must be 512, 1024 or 2048"; exit 2;; esac
mkdir -p "${root}/x86_64-windows" "${root}/x86_64-unix"
inc=(-I"${MWXR_WINE_SOURCE}/include" -I"${MWXR_WINE_SDK}/include/wine/windows")
x86_64-w64-mingw32-gcc -shared -D__WINESRC__ -D_WIN64 -DWINE_NO_LONG_TYPES "${inc[@]}" \
 "${repo}/tests/in_process/metal_probe_pe.c" \
 "${MWXR_WINE_SDK}/lib/wine/x86_64-windows/libwinecrt0.a" \
 "${MWXR_WINE_SDK}/lib/wine/x86_64-windows/libntdll.a" -lkernel32 \
 -Wl,--export-all-symbols -o "${root}/x86_64-windows/wine_foveation_probe.dll"
"${MWXR_WINE_SDK}/bin/winebuild" --builtin "${root}/x86_64-windows/wine_foveation_probe.dll"
clang -arch x86_64 -DFOVEATION_PROBE_SIZE=${size} -shared "${repo}/tests/in_process/foveation_probe_native.m" \
 -framework Foundation -framework Metal -o "${root}/x86_64-unix/wine_foveation_probe.so"
x86_64-w64-mingw32-g++ -static -DFOVEATION_PROBE_SIZE=${size} -I"${DXMT_SOURCE_DIR}/include" \
 -I"${DXMT_SOURCE_DIR}/include/native/directx" "${repo}/tests/windows/dxmt_warp_read_probe.cpp" \
 -ld3d11 -ld3dcompiler_47 -ldxguid -o "${root}/dxmt_warp_read_probe.exe"
print "Built ${root}/dxmt_warp_read_probe.exe"

#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# Builds the OpenXR-independent graphics interop probe: the production
# GraphicsInterop backends and native helper in a test PE DLL/unixlib pair.
set -euo pipefail
repo=${0:A:h:h}
: ${DXMT_SOURCE_DIR:?DXMT headers with IDXMTNativeDevice2}
: ${MWXR_WINE_SDK:?Wine SDK containing bin/winebuild}
: ${MWXR_WINE_SOURCE:?Wine source headers (wine/unixlib.h)}
root=${MWXR_IN_PROCESS_BUILD:-${repo}/build-in-process/gate}
mkdir -p "${root}/support/wine" "${root}/x86_64-windows" "${root}/x86_64-unix" "${root}/probe"
for header in debug.h list.h unixlib.h; do
 ln -sf "${MWXR_WINE_SOURCE}/include/wine/${header}" "${root}/support/wine/${header}"
done
objects=()
for source in src/in_process/graphics_interop src/in_process/graphics_interop_dxmt \
 src/in_process/graphics_interop_d3dmetal tests/in_process/graphics_interop_probe_pe; do
 x86_64-w64-mingw32-g++ -c -fno-exceptions -fno-rtti -fcheck-new -D__WINESRC__ -D_WIN64 -DWINE_NO_LONG_TYPES \
  -I"${repo}/src/in_process" -I"${repo}/tests/in_process" -I"${root}/support" \
  -I"${DXMT_SOURCE_DIR}/include" \
  -I"${DXMT_SOURCE_DIR}/include/native/directx" "${repo}/${source}.cpp" -o "${root}/probe/${source:t}.o"
 objects+=("${root}/probe/${source:t}.o")
done
x86_64-w64-mingw32-gcc -shared "${objects[@]}" \
 "${MWXR_WINE_SDK}/lib/wine/x86_64-windows/libwinecrt0.a" "${MWXR_WINE_SDK}/lib/wine/x86_64-windows/libntdll.a" \
 -lkernel32 -ldxguid -Wl,--export-all-symbols -o "${root}/x86_64-windows/mwxr_graphics_probe.dll"
"${MWXR_WINE_SDK}/bin/winebuild" --builtin "${root}/x86_64-windows/mwxr_graphics_probe.dll"
clang -arch x86_64 -shared -I"${repo}/src/in_process" -I"${repo}/tests/in_process" \
 "${repo}/tests/in_process/graphics_interop_probe_native.m" "${repo}/src/in_process/graphics_interop_native.m" \
 -framework Foundation -framework Metal -framework IOSurface -o "${root}/x86_64-unix/mwxr_graphics_probe.so"
x86_64-w64-mingw32-g++ -static "${repo}/tests/windows/graphics_interop_probe.cpp" -ld3d11 \
 -o "${root}/graphics_interop_probe.exe"
print "Built graphics interop probe in ${root}"

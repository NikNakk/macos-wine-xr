#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=${0:A:h:h}
: ${DXMT_SOURCE_DIR:?DXMT headers with IDXMTNativeDevice2}
: ${MWXR_WINE_SDK:?Wine SDK containing bin/winebuild}
: ${MWXR_WINE_SOURCE:?Wine source headers matching the pinned Proton reference}
: ${MWXR_WINE_RUNTIME:?Wine 11 runtime directory containing lib/wine/x86_64-unix/ntdll.so}
: ${MWXR_NATIVE_LOADER:?x86_64 Khronos libopenxr_loader.dylib}
: ${OPENXR_SOURCE_DIR:?Khronos SDK source containing specification/registry/xr.xml}
root=${MWXR_IN_PROCESS_BUILD:-${repo}/build-in-process/gate}
mkdir -p "${root}/generated" "${root}/support/wine" "${root}/x86_64-windows" "${root}/x86_64-unix"
python3 "${repo}/tools/generate_in_process.py" \
 --xml "${OPENXR_SOURCE_DIR}/specification/registry/xr.xml" --output "${root}/generated"
for header in debug.h list.h unixlib.h; do
 ln -sf "${MWXR_WINE_SOURCE}/include/wine/${header}" "${root}/support/wine/${header}"
done
cp "${repo}/src/in_process/proton/loader_structs.h" "${root}/generated/"
print '#define HAVE_UNISTD_H 1' > "${root}/generated/config.h"
inc=(-I"${repo}/src/in_process" -I"${repo}/src/in_process/proton" \
 -I"${root}/generated" -I"${root}/support" -I"${MWXR_WINE_SDK}/include/wine/windows")
pe_lib=${MWXR_WINE_SDK}/lib/wine/x86_64-windows
# PE graphics: OpenXR adaptation plus the renderer-neutral interop backends.
graphics_objects=()
for source in graphics graphics_interop graphics_interop_dxmt graphics_interop_d3dmetal; do
 x86_64-w64-mingw32-g++ -c -fno-exceptions -fno-rtti -fcheck-new -D__WINESRC__ -D_WIN64 -DWINE_NO_LONG_TYPES \
  -I"${repo}/src/in_process" -I"${root}/generated" -I"${root}/support" -I"${DXMT_SOURCE_DIR}/include" -I"${DXMT_SOURCE_DIR}/include/native/directx" \
  "${repo}/src/in_process/${source}.cpp" -o "${root}/${source}.o"
 graphics_objects+=("${root}/${source}.o")
done
x86_64-w64-mingw32-gcc -shared -D__WINESRC__ -D_WIN64 -DWINE_NO_LONG_TYPES "${inc[@]}" \
 "${repo}/src/in_process/openxr_loader.c" "${root}/generated/loader_thunks.c" "${graphics_objects[@]}" \
 "${pe_lib}/libwinecrt0.a" "${pe_lib}/libntdll.a" -lkernel32 -ldxgi -ldxguid \
 -Wl,--export-all-symbols -o "${root}/x86_64-windows/wineopenxr.dll"
"${MWXR_WINE_SDK}/bin/winebuild" --builtin "${root}/x86_64-windows/wineopenxr.dll"
clang -arch x86_64 -shared -D__WINESRC__ -D_WIN64 -DWINE_NO_LONG_TYPES -DWINE_UNIX_LIB "${inc[@]}" \
 "${repo}/src/in_process/openxr.c" "${root}/generated/openxr_thunks.c" "${repo}/src/in_process/graphics_native.m" \
 "${repo}/src/in_process/graphics_interop_native.m" \
 -framework Metal -framework Foundation \
 "${MWXR_NATIVE_LOADER}" "${MWXR_WINE_RUNTIME}/lib/wine/x86_64-unix/ntdll.so" \
 -Wl,-install_name,@rpath/wineopenxr.so -Wl,-rpath,"${MWXR_NATIVE_LOADER:h}" \
 -Wl,-rpath,"${MWXR_WINE_RUNTIME}/lib/wine/x86_64-unix" \
 -o "${root}/x86_64-unix/wineopenxr.so"
print "Built experimental D3D11/Metal runtime at ${root}."

x86_64-w64-mingw32-gcc -I"${OPENXR_SOURCE_DIR}/include" \
 "${repo}/tests/windows/in_process_gate.c" -o "${root}/in_process_gate.exe"
x86_64-w64-mingw32-gcc -std=c11 -municode -static \
 "${repo}/src/windows/offscreen_unity_window.c" -luser32 -o "${root}/offscreen_unity_window.exe"

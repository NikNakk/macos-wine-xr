#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=${0:A:h:h}
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
x86_64-w64-mingw32-gcc -shared -D__WINESRC__ -D_WIN64 -DWINE_NO_LONG_TYPES "${inc[@]}" \
 "${repo}/src/in_process/openxr_loader.c" "${root}/generated/loader_thunks.c" \
 "${pe_lib}/libwinecrt0.a" "${pe_lib}/libntdll.a" -lkernel32 \
 -Wl,--export-all-symbols -o "${root}/x86_64-windows/wineopenxr.dll"
"${MWXR_WINE_SDK}/bin/winebuild" --builtin "${root}/x86_64-windows/wineopenxr.dll"
clang -arch x86_64 -shared -D__WINESRC__ -D_WIN64 -DWINE_NO_LONG_TYPES -DWINE_UNIX_LIB "${inc[@]}" \
 "${repo}/src/in_process/openxr.c" "${root}/generated/openxr_thunks.c" \
 "${MWXR_NATIVE_LOADER}" "${MWXR_WINE_RUNTIME}/lib/wine/x86_64-unix/ntdll.so" \
 -Wl,-install_name,@rpath/wineopenxr.so -Wl,-rpath,"${MWXR_NATIVE_LOADER:h}" \
 -Wl,-rpath,"${MWXR_WINE_RUNTIME}/lib/wine/x86_64-unix" \
 -o "${root}/x86_64-unix/wineopenxr.so"
print "Built initial core gate at ${root}; graphics remains unavailable."

x86_64-w64-mingw32-gcc -I"${OPENXR_SOURCE_DIR}/include" \
 "${repo}/tests/windows/in_process_gate.c" -o "${root}/in_process_gate.exe"

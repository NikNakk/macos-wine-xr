#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=${0:A:h:h}
: ${OPENXR_SOURCE_DIR:?Khronos SDK headers}
: ${DXMT_SOURCE_DIR:?DXMT DirectX headers}
: ${MWXR_WINDOWS_LOADER:?Static Win64 Khronos libopenxr_loader.a}
root=${MWXR_IN_PROCESS_BUILD:-${repo}/build-in-process/gate}
mkdir -p "${root}"
x86_64-w64-mingw32-g++ -static -I"${DXMT_SOURCE_DIR}/include/native/directx" \
 -I"${OPENXR_SOURCE_DIR}/include" "${repo}/tests/windows/in_process_latency.cpp" \
 "${MWXR_WINDOWS_LOADER}" -ld3d11 -ldxguid -ladvapi32 -lshlwapi -lole32 \
 -o "${root}/in_process_latency.exe"

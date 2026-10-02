#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
set -euo pipefail
repo=${0:A:h:h}
: ${OPENXR_INCLUDE_DIR:?Set OPENXR_INCLUDE_DIR to the official Khronos OpenXR include directory}
: ${DXMT_SOURCE_DIR:?Set DXMT_SOURCE_DIR to current DXMT with IDXMTNativeDevice support}
cmake -S "${repo}" -B "${repo}/build-native-openxr" -G Ninja \
 -DOPENXR_INCLUDE_DIR="${OPENXR_INCLUDE_DIR}" -DDXMT_INCLUDE_DIR="${DXMT_SOURCE_DIR}/include"
cmake --build "${repo}/build-native-openxr" --parallel
cmake -S "${repo}" -B "${repo}/build-win-openxr" -G Ninja \
 -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
 -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
 -DOPENXR_INCLUDE_DIR="${OPENXR_INCLUDE_DIR}" -DDXMT_INCLUDE_DIR="${DXMT_SOURCE_DIR}/include"
cmake --build "${repo}/build-win-openxr" --parallel

#!/bin/zsh
# Copyright 2026 Nick Kennedy; SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=${0:A:h:h}
: ${DXMT_SOURCE_DIR:?Private DXMT source with replay ownership diagnostic}
root=${repo}/build-in-process/replay-probe
mkdir -p "${root}"
x86_64-w64-mingw32-g++ -O2 -static -I"${DXMT_SOURCE_DIR}/include" -I"${DXMT_SOURCE_DIR}/include/native/directx" "${repo}/tests/windows/dxmt_replay_probe.cpp" -ld3d11 -ld3dcompiler_47 -ldxguid -o "${root}/dxmt_replay_probe.exe"

x86_64-w64-mingw32-g++ -O2 -static -I"${DXMT_SOURCE_DIR}/include" -I"${DXMT_SOURCE_DIR}/include/native/directx" "${repo}/tests/windows/dxmt_pass_replay_probe.cpp" -ld3d11 -ld3dcompiler_47 -ldxguid -o "${root}/dxmt_pass_replay_probe.exe"

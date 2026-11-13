#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=${0:A:h:h}
: ${DXMT_SOURCE_DIR:?Private DXMT source with the NGX ROI patch}
root=${MWXR_NGX_PROBE_BUILD:-${repo}/build-in-process/ngx-subrect-probe}
mkdir -p "${root}"
x86_64-w64-mingw32-g++ -static -I"${DXMT_SOURCE_DIR}/src/nvngx" -I"${DXMT_SOURCE_DIR}/src/d3d11" \
 -I"${DXMT_SOURCE_DIR}/src/util" "${repo}/tests/windows/dxmt_ngx_subrect_probe.cpp" \
 -ld3d11 -ld3dcompiler_47 -ldxguid -o "${root}/dxmt_ngx_subrect_probe.exe"

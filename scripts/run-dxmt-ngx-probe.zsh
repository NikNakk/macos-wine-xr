#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=${0:A:h:h}
: ${MWXR_IN_PROCESS_WINE:?Private runtime with paired NGX/ROI PE and Unix DLLs}
root=${MWXR_NGX_PROBE_BUILD:-${repo}/build-in-process/ngx-subrect-probe}
export WINEPREFIX=${repo}/build-in-process/prefix-foveation
export WINEARCH=win64 WINEDEBUG=${WINEDEBUG:--all}
export WINESERVER=${MWXR_IN_PROCESS_WINE}/bin/wineserver
export DYLD_FALLBACK_LIBRARY_PATH=${MWXR_IN_PROCESS_WINE}/lib/wine/x86_64-unix:${MWXR_IN_PROCESS_WINE}/lib
export DXMT_ENABLE_NVEXT=1 DXMT_NGX_TRACE=1 DXMT_NGX_SUBRECTS=${DXMT_NGX_SUBRECTS:-1}
export MTL_DEBUG_LAYER=1
unset WINEDLLOVERRIDES
[[ -f ${WINEPREFIX}/system.reg && -f ${root}/dxmt_ngx_subrect_probe.exe ]] || exit 2
exec "${MWXR_IN_PROCESS_WINE}/bin/wine" "${root}/dxmt_ngx_subrect_probe.exe" "$@"

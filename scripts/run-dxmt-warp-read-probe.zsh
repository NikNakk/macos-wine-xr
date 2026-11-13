#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=${0:A:h:h}
: ${MWXR_IN_PROCESS_WINE:?Private Wine runtime containing the experimental DXMT build}
root=${MWXR_WARP_READ_BUILD:-${repo}/build-in-process/warp-read-probe}
export WINEPREFIX=${MWXR_WARP_READ_PREFIX:-${repo}/build-in-process/prefix-warp}
export WINEARCH=win64 WINEDEBUG=${WINEDEBUG:--all}
export WINEDLLPATH=${root} WINESERVER=${MWXR_IN_PROCESS_WINE}/bin/wineserver
export DYLD_FALLBACK_LIBRARY_PATH=${MWXR_IN_PROCESS_WINE}/lib/wine/x86_64-unix:${MWXR_IN_PROCESS_WINE}/lib
export MTL_DEBUG_LAYER=${MTL_DEBUG_LAYER:-1}
unset WINEDLLOVERRIDES
[[ -x ${MWXR_IN_PROCESS_WINE}/bin/wine && -f ${root}/dxmt_warp_read_probe.exe ]] || exit 1
if [[ ! -f ${WINEPREFIX}/system.reg ]]; then
 WINEDLLOVERRIDES='mscoree=;mshtml=' "${MWXR_IN_PROCESS_WINE}/bin/wine" wineboot --init
fi
cp "${root}/x86_64-windows/wine_foveation_probe.dll" "${WINEPREFIX}/drive_c/windows/system32/"
exec "${MWXR_IN_PROCESS_WINE}/bin/wine" "${root}/dxmt_warp_read_probe.exe" "$@"

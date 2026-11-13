#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=${0:A:h:h}
: ${MWXR_IN_PROCESS_WINE:?Private paired runtime with explicit attachment probe patch}
root=${MWXR_DEPTH_PROBE_BUILD:-${repo}/build-in-process/depth-foveation-probe}
export WINEPREFIX=${repo}/build-in-process/prefix-foveation
export WINEARCH=win64 WINEDEBUG=${WINEDEBUG:--all}
export WINEDLLPATH=${root} WINESERVER=${MWXR_IN_PROCESS_WINE}/bin/wineserver
export DYLD_FALLBACK_LIBRARY_PATH=${MWXR_IN_PROCESS_WINE}/lib/wine/x86_64-unix:${MWXR_IN_PROCESS_WINE}/lib
export MTL_DEBUG_LAYER=1 DXMT_RATE_MAP_POSITION=1
export DXMT_RATE_MAP_ATTACHMENT_PROBE=${DXMT_RATE_MAP_ATTACHMENT_PROBE:-1}
unset WINEDLLOVERRIDES DXMT_BASE_FOVEATION
[[ -f ${WINEPREFIX}/system.reg && -f ${root}/dxmt_depth_foveation_probe.exe ]] || exit 2
cp "${root}/x86_64-windows/wine_depth_foveation_probe.dll" "${WINEPREFIX}/drive_c/windows/system32/"
cd "${root}"
exec "${MWXR_IN_PROCESS_WINE}/bin/wine" "${root}/dxmt_depth_foveation_probe.exe" "$@"

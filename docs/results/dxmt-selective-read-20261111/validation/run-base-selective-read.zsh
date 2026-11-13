#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=/Users/nickkennedy/Code/macos-wine-xr
: ${MWXR_IN_PROCESS_WINE:?Private Wine runtime containing the experimental DXMT build}
root=${MWXR_FOVEATION_BUILD:-${repo}/build-in-process/foveation-probe-2048}
export WINEPREFIX=${repo}/build-in-process/exd-warp/prefix
export WINEARCH=win64 WINEDEBUG=${WINEDEBUG:--all}
export WINESERVER=${MWXR_IN_PROCESS_WINE}/bin/wineserver
export DYLD_FALLBACK_LIBRARY_PATH=${MWXR_IN_PROCESS_WINE}/lib/wine/x86_64-unix:${MWXR_IN_PROCESS_WINE}/lib
export MTL_DEBUG_LAYER=1
unset WINEDLLOVERRIDES
[[ -f ${WINEPREFIX}/system.reg && -f ${root}/dxmt_base_foveation_probe.exe ]] || exit 1
exec "${MWXR_IN_PROCESS_WINE}/bin/wine" "${root}/dxmt_base_foveation_probe.exe" "$@"

#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=${0:A:h:h}
root=${MWXR_IN_PROCESS_BUILD:-${repo}/build-in-process/gate}
wine_root=${repo}/build-in-process/wine-direct
export WINEPREFIX=${repo}/build-in-process/prefix-metal
export WINEARCH=win64
export WINEDEBUG=${WINEDEBUG:--all}
export WINEDLLPATH=${root}
export DYLD_FALLBACK_LIBRARY_PATH=${wine_root}/lib/wine/x86_64-unix
export MTL_DEBUG_LAYER=1
unset WINEDLLOVERRIDES
[[ -x ${wine_root}/bin/wine && -f ${root}/direct_metal_import.exe ]] || {
 print -u2 'Build/install the direct DXMT and probe first'; exit 1
}
"${wine_root}/bin/wine" wineboot -u
cp "${root}/x86_64-windows/wine_metal_probe.dll" "${WINEPREFIX}/drive_c/windows/system32/"
"${wine_root}/bin/wine" "${root}/direct_metal_import.exe"

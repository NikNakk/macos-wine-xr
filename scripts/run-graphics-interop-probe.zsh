#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# Runs the graphics interop probe under a Wine runtime: by default the private
# direct-object DXMT runtime. Set MWXR_PROBE_WINE to a GPTK/D3DMetal Wine to
# test that backend; MWXR_GRAPHICS_BACKEND selects as in wineopenxr.
set -euo pipefail
repo=${0:A:h:h}
root=${MWXR_IN_PROCESS_BUILD:-${repo}/build-in-process/gate}
wine_root=${MWXR_PROBE_WINE:-${repo}/build-in-process/wine-direct}
export WINEPREFIX=${MWXR_PROBE_PREFIX:-${repo}/build-in-process/prefix-metal}
export WINEARCH=win64
export WINEDEBUG=${WINEDEBUG:--all}
export WINEDLLPATH=${root}
export MTL_DEBUG_LAYER=${MTL_DEBUG_LAYER:-1}
unset WINEDLLOVERRIDES
[[ -x ${wine_root}/bin/wine && -f ${root}/graphics_interop_probe.exe ]] || {
 print -u2 'Build the probe (scripts/build-graphics-interop-probe.zsh) and a Wine runtime first'; exit 1
}
# The probe needs neither .NET nor HTML; skip the Mono/Gecko install dialogs.
WINEDLLOVERRIDES="mscoree=;mshtml=" "${wine_root}/bin/wine" wineboot -u
cp "${root}/x86_64-windows/mwxr_graphics_probe.dll" "${WINEPREFIX}/drive_c/windows/system32/"
"${wine_root}/bin/wine" "${root}/graphics_interop_probe.exe"

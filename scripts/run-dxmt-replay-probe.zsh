#!/bin/zsh
# Copyright 2026 Nick Kennedy; SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=${0:A:h:h}
: ${MWXR_IN_PROCESS_WINE:?Private Wine runtime with paired DXMT overlay}
export WINEPREFIX=${repo}/build-in-process/prefix-foveation WINEARCH=win64 WINEDEBUG=-all
export WINESERVER=${MWXR_IN_PROCESS_WINE}/bin/wineserver
export DYLD_FALLBACK_LIBRARY_PATH=${MWXR_IN_PROCESS_WINE}/lib/wine/x86_64-unix:${MWXR_IN_PROCESS_WINE}/lib
export MTL_DEBUG_LAYER=1
unset WINEDLLOVERRIDES
export DXMT_REPLAY_CAPTURE=${DXMT_REPLAY_CAPTURE:-1}
[[ -f ${WINEPREFIX}/system.reg ]] || exit 1
exec "${MWXR_IN_PROCESS_WINE}/bin/wine" "${MWXR_REPLAY_PROBE_EXE:-${repo}/build-in-process/replay-probe/dxmt_replay_probe.exe}" "$@"

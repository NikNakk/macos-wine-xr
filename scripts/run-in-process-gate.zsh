#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# Requires an already-running isolated simulated Monado service.
set -euo pipefail
repo=${0:A:h:h}
: ${MWXR_WINE_RUNTIME:?Wine 11 runtime directory}
: ${MWXR_NATIVE_RUNTIME_JSON:?x86_64 Monado runtime manifest}
: ${XDG_RUNTIME_DIR:?isolated simulated service socket directory}
root=${MWXR_IN_PROCESS_BUILD:-${repo}/build-in-process/gate}
# Deliberately ignores the user's existing WINEPREFIX.
export WINEPREFIX=${repo}/build-in-process/prefix-core
export WINEARCH=win64
export WINEDEBUG=${WINEDEBUG:--all}
export WINEDLLPATH=${root}
export MWXR_NATIVE_RUNTIME_JSON
unset XR_RUNTIME_JSON
unset WINEDLLOVERRIDES
wine=${MWXR_WINE_RUNTIME}/bin/wine
[[ -f ${root}/in_process_gate.exe && -f ${root}/x86_64-unix/wineopenxr.so ]] || {
 print -u2 'Build scripts/build-in-process-gate.zsh first'; exit 1
}
"${wine}" wineboot -u
mkdir -p "${WINEPREFIX}/drive_c/openxr"
cat > "${WINEPREFIX}/drive_c/openxr/wineopenxr64.json" <<'JSON'
{
  "file_format_version": "1.0.0",
  "runtime": {"library_path": "C:\\windows\\system32\\wineopenxr.dll"}
}
JSON
"${wine}" reg add 'HKLM\Software\Khronos\OpenXR\1' /v ActiveRuntime /t REG_SZ \
 /d 'C:\openxr\wineopenxr64.json' /f
"${wine}" "${root}/in_process_gate.exe"

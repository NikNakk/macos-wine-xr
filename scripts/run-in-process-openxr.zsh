#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=${0:A:h:h}
: ${MWXR_NATIVE_RUNTIME_JSON:?x86_64 Monado runtime manifest}
[[ $# -gt 0 ]] || { print -u2 'usage: run-in-process-openxr.zsh executable [args...]'; exit 1; }
root=${MWXR_IN_PROCESS_BUILD:-${repo}/build-in-process/gate}
wine_root=${repo}/build-in-process/wine-direct
# Explicitly separate from the proxy, native-host and smoke-test prefixes.
export WINEPREFIX=${MWXR_IN_PROCESS_PREFIX:-${repo}/build-in-process/prefix-openxr}
export WINEARCH=win64
export WINEDEBUG=${WINEDEBUG:--all}
export WINEDLLPATH=${root}
export DYLD_FALLBACK_LIBRARY_PATH=${wine_root}/lib/wine/x86_64-unix
export MWXR_NATIVE_RUNTIME_JSON
unset XR_RUNTIME_JSON WINEDLLOVERRIDES
[[ -x ${wine_root}/bin/wine && -f ${root}/x86_64-unix/wineopenxr.so ]] || {
 print -u2 'Build the in-process runtime and install direct-object DXMT first'; exit 1
}
"${wine_root}/bin/wine" wineboot -u
mkdir -p "${WINEPREFIX}/drive_c/openxr"
cp "${root}/x86_64-windows/wineopenxr.dll" "${WINEPREFIX}/drive_c/windows/system32/"
cat > "${WINEPREFIX}/drive_c/openxr/wineopenxr64.json" <<'JSON'
{"file_format_version":"1.0.0","runtime":{"library_path":"C:\\windows\\system32\\wineopenxr.dll"}}
JSON
"${wine_root}/bin/wine" reg add 'HKLM\Software\Khronos\OpenXR\1' /v ActiveRuntime /t REG_SZ \
 /d 'C:\openxr\wineopenxr64.json' /f
exec "${wine_root}/bin/wine" "$@"

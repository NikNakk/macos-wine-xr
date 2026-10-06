#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=${0:A:h:h}
: ${MWXR_NATIVE_RUNTIME_JSON:?x86_64 Monado runtime manifest}
[[ $# -gt 0 ]] || { print -u2 'usage: run-in-process-openxr.zsh executable [args...]'; exit 1; }
root=${MWXR_IN_PROCESS_BUILD:-${repo}/build-in-process/gate}
# MWXR_IN_PROCESS_WINE selects another runtime, for example a GPTK/D3DMetal Wine.
wine_root=${MWXR_IN_PROCESS_WINE:-${repo}/build-in-process/wine-direct}
# Explicitly separate from the proxy, native-host and smoke-test prefixes.
export WINEPREFIX=${MWXR_IN_PROCESS_PREFIX:-${repo}/build-in-process/prefix-openxr}
export WINEARCH=win64
export WINEDEBUG=${WINEDEBUG:--all}
export WINEDLLPATH=${root}
# Wine loads GnuTLS, FreeType and friends by name from the runtime's lib.
export DYLD_FALLBACK_LIBRARY_PATH=${wine_root}/lib/wine/x86_64-unix:${wine_root}/lib
export MWXR_NATIVE_RUNTIME_JSON
unset XR_RUNTIME_JSON WINEDLLOVERRIDES
[[ -x ${wine_root}/bin/wine && -f ${root}/x86_64-unix/wineopenxr.so ]] || {
 print -u2 'Build the in-process runtime and install direct-object DXMT first'; exit 1
}
WINEDLLOVERRIDES="mscoree=;mshtml=" "${wine_root}/bin/wine" wineboot -u
mkdir -p "${WINEPREFIX}/drive_c/openxr"
cp "${root}/x86_64-windows/wineopenxr.dll" "${WINEPREFIX}/drive_c/windows/system32/"
cat > "${WINEPREFIX}/drive_c/openxr/wineopenxr64.json" <<'JSON'
{"file_format_version":"1.0.0","runtime":{"library_path":"C:\\windows\\system32\\wineopenxr.dll"}}
JSON
"${wine_root}/bin/wine" reg add 'HKLM\Software\Khronos\OpenXR\1' /v ActiveRuntime /t REG_SZ \
 /d 'C:\openxr\wineopenxr64.json' /f
case ${MWXR_DESKTOP_MIRROR:-window} in
 window)
  # MWXR_WINEDLLOVERRIDES applies to the application and its children, for
  # example "gameoverlayrenderer64=" to keep Steam's overlay out.
  [[ -n ${MWXR_WINEDLLOVERRIDES:-} ]] && export WINEDLLOVERRIDES=${MWXR_WINEDLLOVERRIDES}
  exec "${wine_root}/bin/wine" "$@" ;;
 offscreen)
  [[ -f ${root}/offscreen_unity_window.exe ]] || {
   print -u2 'Rebuild scripts/build-in-process-gate.zsh for the off-screen Unity launcher'; exit 1
  }
  game=$("${wine_root}/bin/wine" winepath -w "$1")
  game=${game//$'\r'/}
  shift
  exec "${wine_root}/bin/wine" "${root}/offscreen_unity_window.exe" "$game" -screen-fullscreen 0 "$@"
  ;;
 *) print -u2 'MWXR_DESKTOP_MIRROR must be window or offscreen'; exit 2 ;;
esac

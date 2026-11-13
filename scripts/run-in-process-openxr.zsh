#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=${0:A:h:h}
: ${MWXR_NATIVE_RUNTIME_JSON:?x86_64 Monado runtime manifest}
[[ $# -gt 0 ]] || { print -u2 'usage: run-in-process-openxr.zsh executable [args...]'; exit 1; }
source "${repo}/scripts/openxr-launch-settings.zsh"
root=${MWXR_IN_PROCESS_BUILD:-${repo}/build-in-process/gate}
# MWXR_IN_PROCESS_WINE selects another runtime, for example a GPTK/D3DMetal Wine.
wine_root=${MWXR_IN_PROCESS_WINE:-${MWXR_WINE_TREE:-$HOME/Code/wine-crossover/runtime}}
# Explicitly separate from the proxy, native-host and smoke-test prefixes.
export WINEPREFIX=${MWXR_IN_PROCESS_PREFIX:-${repo}/build-in-process/prefix-openxr}
export WINEARCH=win64
export WINEDEBUG=${WINEDEBUG:--all}
export WINEDLLPATH=${root}
export WINESERVER=${wine_root}/bin/wineserver
# Wine loads GnuTLS, FreeType and friends by name from the runtime's lib.
export DYLD_FALLBACK_LIBRARY_PATH=${wine_root}/lib/wine/x86_64-unix:${wine_root}/lib
export MWXR_NATIVE_RUNTIME_JSON
unset XR_RUNTIME_JSON WINEDLLOVERRIDES
# Select KK for native Vulkan applications without installing Zink's GL DLLs.
if [[ ${MWXR_VULKAN:-} == kosmickrisp ]]; then
 kk_root=${MWXR_KK_ROOT:-$HOME/Code/alyx-native/build/zink-wine}
 [[ -f ${kk_root}/lib/libvulkan.1.dylib && -f ${kk_root}/icd/kosmickrisp_x86_64.json ]] || {
  print -u2 "MWXR_VULKAN=kosmickrisp: missing loader or ICD in ${kk_root}"; exit 1
 }
 export DYLD_FALLBACK_LIBRARY_PATH=${kk_root}/lib:${DYLD_FALLBACK_LIBRARY_PATH}
 export VK_DRIVER_FILES=${kk_root}/icd/kosmickrisp_x86_64.json
 print -u2 "Vulkan: KosmicKrisp from ${kk_root}"
elif [[ -n ${MWXR_VULKAN:-} ]]; then
 print -u2 'MWXR_VULKAN must be kosmickrisp (or unset)'; exit 2
fi
# MWXR_OPENGL=zink: the application's OpenGL through Mesa's Zink over KosmicKrisp
# instead of winemac's CGL (docs/zink-kosmickrisp.md). MWXR_ZINK_ROOT holds bin/
# (opengl32.dll, libgallium_wgl.dll, libwinpthread-1.dll), lib/libvulkan.1.dylib (an
# x86_64 Vulkan loader for winevulkan) and icd/kosmickrisp_x86_64.json.
if [[ ${MWXR_OPENGL:-} == zink ]]; then
 zink=${MWXR_ZINK_ROOT:-$HOME/Code/alyx-native/build/zink-wine}
 [[ -f ${zink}/bin/opengl32.dll && -f ${zink}/lib/libvulkan.1.dylib ]] || {
  print -u2 "MWXR_OPENGL=zink: no Zink build in ${zink}"; exit 1
 }
 export DYLD_FALLBACK_LIBRARY_PATH=${zink}/lib:${DYLD_FALLBACK_LIBRARY_PATH}
 export VK_DRIVER_FILES=${zink}/icd/kosmickrisp_x86_64.json GALLIUM_DRIVER=zink
 # KosmicKrisp has neither transform feedback nor geometry shaders, so Zink offers 2.1.
 export MESA_GL_VERSION_OVERRIDE=${MESA_GL_VERSION_OVERRIDE-3.2COMPAT}
 export MESA_KK_EXPERIMENTAL=${MESA_KK_EXPERIMENTAL-custom_border}
 # Wine finds system32's builtin stub before PATH, so the DLLs go in system32. Other
 # programs in the prefix keep Wine's builtin opengl32: only "n,b" loads this one.
 zink_copy=1
 export MWXR_WINEDLLOVERRIDES="opengl32=n,b${MWXR_WINEDLLOVERRIDES:+;${MWXR_WINEDLLOVERRIDES}}"
 print -u2 "OpenGL: Zink over KosmicKrisp from ${zink} (GL override ${MESA_GL_VERSION_OVERRIDE:-none})"
fi
[[ -x ${wine_root}/bin/wine && -f ${root}/x86_64-unix/wineopenxr.so ]] || {
 print -u2 'Build the in-process runtime and select a compatible Wine runtime (MWXR_IN_PROCESS_WINE)'; exit 1
}
# Wine updates a prefix itself when its runtime's wine.inf changes. An explicit
# "wineboot -u" on every launch left a temporary copy of each DLL a running process
# had open (dll*.tmp, pending until the prefix restarts): 12.9 GB in the rig prefix.
[[ -f ${WINEPREFIX}/system.reg ]] || WINEDLLOVERRIDES="mscoree=;mshtml=" "${wine_root}/bin/wine" wineboot --init
mkdir -p "${WINEPREFIX}/drive_c/openxr"
cp "${root}/x86_64-windows/wineopenxr.dll" "${WINEPREFIX}/drive_c/windows/system32/"
[[ -n ${zink_copy:-} ]] && cp "${zink}"/bin/{opengl32,libgallium_wgl,libwinpthread-1}.dll \
 "${WINEPREFIX}/drive_c/windows/system32/"
# xrizer loads openxr_loader.dll by name. Keeping it next to vrclient_x64.dll
# does not put it on the Windows process DLL search path. Provision the
# Khronos Win64 loader into this prefix, not the host's arm64 dylib directory.
windows_loader=${MWXR_WINDOWS_OPENXR_LOADER:-${MWXR_XRIZER_ROOT:-}/openxr_loader.dll}
if [[ -n ${MWXR_WINDOWS_OPENXR_LOADER:-} || -n ${MWXR_XRIZER_ROOT:-} ]]; then
 [[ -f ${windows_loader} ]] || {
  print -u2 "Windows OpenXR loader is missing: ${windows_loader}"; exit 1
 }
 cp "${windows_loader}" "${WINEPREFIX}/drive_c/windows/system32/openxr_loader.dll"
fi
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

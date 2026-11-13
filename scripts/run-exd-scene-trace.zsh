#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# A prepared private game/prefix/runtime copy is required; see docs/dxmt-base-render-foveation.md.
set -euo pipefail
unsetopt BG_NICE
repo=${0:A:h:h}
root=${MWXR_EXD_EXPERIMENT_ROOT:-${repo}/build-in-process/exd-ngx}
[[ ${root} == ${repo}/build-in-process/* && ! -L ${root}/game && ! -L ${root}/prefix && ! -L ${root}/wine &&
   -f ${root}/game/EXD.exe && -f ${root}/prefix/system.reg && -x ${root}/wine/bin/wine ]] || {
 print -u2 'Use prepared, non-symlinked experimental copies under build-in-process'; exit 2
}
if [[ ${1:-} == --launch-game ]]; then
 shift
 export MWXR_LSTEAMCLIENT=1 SteamAppId=3962020 SteamGameId=3962020
 export STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS"
 exe_windows="Z:${root//\//\\}\\game\\EXD.exe"
 steam_windows="Z:${HOME//\//\\}\\Windows\\library\\steam-client"
 cd "${root}/game"
 exec "${repo}/scripts/run-in-process-openxr.zsh" "${repo}/build-in-process/lsteamclient/mwxr-steam.exe" \
  --steam-dir "${steam_windows}" -- "${exe_windows}" "$@" > "${GAME_LOG_DIR}/game.log" 2>&1
fi
export MWXR_IN_PROCESS_PREFIX=${root}/prefix MWXR_IN_PROCESS_WINE=${root}/wine
export MWXR_IN_PROCESS_BUILD=${MWXR_IN_PROCESS_BUILD:-${repo}/build-in-process/foveation-gate}
export MWXR_NATIVE_RUNTIME_JSON=${MWXR_NATIVE_RUNTIME_JSON:-${repo}/build-in-process/foveation-monado-x64/openxr_monado-dev.json}
export MONADO_FOVEATION_SERVICE=${MONADO_FOVEATION_SERVICE:-${repo}/build-in-process/foveation-monado-arm64/src/xrt/targets/service/monado-service}
export MONADO_VULKAN_ICD=${MONADO_VULKAN_ICD:-/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json}
export MWXR_FOVEATION_RUN_DIR=${MWXR_FOVEATION_RUN_DIR:-${root}/scene-$(date +%Y%m%d-%H%M%S)}
export GAME_LOG_DIR=${MWXR_FOVEATION_RUN_DIR}/game
mkdir -p "${GAME_LOG_DIR}"
export DXMT_SCENE_TRACE=${DXMT_SCENE_TRACE:-1} DXMT_SCENE_TRACE_LIMIT=${DXMT_SCENE_TRACE_LIMIT:-200000}
export DXMT_NGX_TRACE=${DXMT_NGX_TRACE:-1} DXMT_ENABLE_NVEXT=1 DXMT_NGX_CHEEKY=0
export DXMT_ENCODER_LABELS=${DXMT_ENCODER_LABELS:-1}
export DXMT_LOG_PATH="Z:${GAME_LOG_DIR//\//\\}"
export MWXR_WINEDLLOVERRIDES='nvapi64,nvngx,_nvngx,nvngx_dlss=b;gameoverlayrenderer64='
export MWXR_FOVEATION_LAUNCH_SCRIPT=${0:A}
mode=${EXD_SCENE_DLSS:-classic}
case ${mode} in
 classic) technology=Classic ;;
 advanced) technology=Advanced; export DXMT_NGX_CHEEKY=1 ;;
 *) print -u2 'EXD_SCENE_DLSS must be classic or advanced'; exit 2 ;;
esac
cleanup() {
 WINEPREFIX=${MWXR_IN_PROCESS_PREFIX} "${MWXR_IN_PROCESS_WINE}/bin/wineserver" -k 2>/dev/null || true
 if [[ -n ${child:-} ]]; then kill -TERM ${child} 2>/dev/null || true; wait ${child} 2>/dev/null || true; fi
}
trap cleanup EXIT
trap 'exit 143' TERM HUP
trap 'exit 130' INT
"${repo}/scripts/run-openxr-foveation-probe.zsh" --launch-game \
 -dx11 -hmd=OpenXRHMD -EXDOpenXROnly -DisablePlugins=OculusOpenXR "-EXDDLSS=${technology}" \
 -ngxenable -NGXLogLevel=2 -NGXLogFileEnable -windowed -ResX=1280 -ResY=720 "$@" &
child=$!
wait ${child}

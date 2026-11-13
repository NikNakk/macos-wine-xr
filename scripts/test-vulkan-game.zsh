#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# Headset test of the private Vulkan bridge on this rig. Start macOS Steam and
# the normal Monado headset service first. No simulator or automatic timeout.
set -euo pipefail
repo=${0:A:h:h}
case ${1:-} in
 talos|552440) appid=552440; folder='The Talos Principle VR'; exe=Bin/x64/Talos_VR.exe
              args=(+gfx_strAPI Vulkan) ;;
 nms|275850) appid=275850; folder="No Man's Sky"; exe=Binaries/NMS.exe
            args=(-HmdEnable 1) ;;
 *) print -u2 'usage: zsh scripts/test-vulkan-game.zsh talos|nms [extra game arguments]'; exit 2 ;;
esac
shift
common=${MWXR_STEAM_COMMON:-$HOME/Library/Application Support/Steam/steamapps/common}
export GAME_EXE=${common}/${folder}/${exe}
export GAME_WORKDIR=${GAME_EXE:h}
[[ -f ${GAME_EXE} ]] || { print -u2 "Missing game: ${GAME_EXE}"; exit 1; }
export MWXR_IN_PROCESS_BUILD=${MWXR_IN_PROCESS_BUILD:-${repo}/build-in-process/vk-fov-gate}
export MWXR_VULKAN=kosmickrisp MWXR_KK_ROOT=${repo}/build-in-process/zink-fov
[[ -f ${MWXR_IN_PROCESS_BUILD}/x86_64-windows/wineopenxr.dll &&
   -f ${MWXR_IN_PROCESS_BUILD}/x86_64-unix/wineopenxr.so ]] || {
 print -u2 'Build the private Vulkan gate first (docs/vulkan-bridge.md).'; exit 1
}
# Use the installed hardware service. Foveation stays off unless MWXR_FOVEATION is given
# (KosmicKrisp implicit foveation with the bridge's gaze; docs/kk-implicit-foveation.md).
unset XR_RUNTIME_JSON XRT_MACOS_METAL_IPC_SERVICE_NAME XDG_RUNTIME_DIR IPC_IGNORE_VERSION
unset MWXR_OPENGL KK_IMPLICIT_FDM
export MWXR_FOVEATION=${MWXR_FOVEATION:-off}; unset MWXR_GAZE_FOVEATION
export MWXR_IN_PROCESS_PREFIX=$HOME/Windows/prefix
export MWXR_IN_PROCESS_WINE=${MWXR_IN_PROCESS_WINE:-${repo}/build-in-process/vulkan-wine}
export MWXR_XRIZER_ROOT=${MWXR_XRIZER_ROOT:-${repo}/build-in-process/vulkan-xrizer}
export MWXR_NATIVE_RUNTIME_JSON=$HOME/Code/alyx-native/build/monado-system-x86_64/openxr_monado-dev.json
source "${repo}/scripts/steam-play-game-settings.zsh" "${appid}"
export GAME_LOG_DIR=${GAME_LOG_DIR:-${repo}/build-in-process/vulkan-game-inspection/headset-${appid}-$(date +%Y%m%d-%H%M%S)}
export RUST_LOG=${RUST_LOG:-xrizer=debug,openvr=warn}
print "Private Vulkan bridge; foveation ${MWXR_FOVEATION}. Logs: ${GAME_LOG_DIR}"
exec "${repo}/scripts/run-windows-game.zsh" "${appid}" "${args[@]}" "$@"

#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Half-Life: Alyx on a headset without SteamVR: xrizer in the game, the in-process
# wineopenxr, DXMT, and an already installed Monado service (for the PS VR2, the
# LaunchAgent; 90 Hz with U_PACING_APP_USE_MIN_FRAME_PERIOD=1 holds 90 fps on an M5 at
# the default table's lowest level). See docs/windows-vr-performance-opportunities.md.
#
# Usage: [ALYX_FIDELITY_TABLE=file.csv | ALYX_FIXED_PERCENT=50 [ALYX_MSAA=2]] \
#          run-alyx-xrizer.zsh [game arguments...]
#   default                Alyx's adaptive fidelity over scripts/alyx/fidelity-levels.csv:
#                          2x MSAA, render scale 0.50 to 0.72 of the runtime's recommended
#                          size. Alyx picks the level from its measured GPU time and the
#                          display rate. Its own table starts at 0.81 with 4x MSAA.
#   ALYX_FIDELITY_TABLE    another table, for example scripts/alyx/fidelity-levels-to-1.23.csv
#                          (up to Valve's top size; Alyx allocates its render targets for the
#                          largest level, which cost about 5 fps at 90 Hz on an M5)
#   ALYX_FIXED_PERCENT     a fixed size instead: this percentage of the recommended size
#                          (OXR_VIEWPORT_SCALE_PERCENTAGE), at ALYX_MSAA samples (2, 4 or 8)
#   ALYX_STEAM=macos       use the macOS Steam client through lsteamclient instead of a
#                          Windows Steam in the prefix (docs/lsteamclient.md): needs
#                          MWXR_IN_PROCESS_WINE and the prefix set up by
#                          scripts/install-lsteamclient.zsh, and macOS Steam signed in
#   ALYX_DRY_RUN=1         print the table, environment and command without starting the game
#   game arguments         for example "+load autosave"
#
# Required (a rig wrapper sets them, as for run-in-process-alyx.zsh):
#   MWXR_IN_PROCESS_PREFIX    the Wine prefix with Steam and Alyx installed
#   MWXR_IN_PROCESS_WINE      the Wine runtime with DXMT
#   MWXR_XRIZER_ROOT          xrizer provision (bin/vrclient_x64.dll, openxr_loader.dll)
#   MWXR_NATIVE_RUNTIME_JSON  the x86_64 Monado client's runtime manifest
# Optional: ALYX_GAME_ROOT (default: the prefix's Steam library), MWXR_IN_PROCESS_BUILD,
# ALYX_LOG_DIR. Windows Steam must already be running and signed in in that prefix (or
# macOS Steam, with ALYX_STEAM=macos).
#
# The Windows build ignores a fidelity table at a fixed level unless +vr_msaa is on its
# command line (it then renders at the full recommended size with 8x MSAA); adaptive
# fidelity uses the table as it is. The game's override table and the prefix's OpenVR
# path files (which SteamVR also uses) are put back on exit.
set -euo pipefail
repo=${0:A:h:h}
: ${MWXR_IN_PROCESS_PREFIX:?Wine prefix with Steam and Alyx}
: ${MWXR_IN_PROCESS_WINE:?Wine runtime with DXMT}
: ${MWXR_XRIZER_ROOT:?xrizer provision}
: ${MWXR_NATIVE_RUNTIME_JSON:?x86_64 Monado runtime manifest}
export ALYX_GAME_ROOT=${ALYX_GAME_ROOT:-${MWXR_IN_PROCESS_PREFIX}/drive_c/Program Files (x86)/Steam/steamapps/common/Half-Life Alyx}
hlvr="${ALYX_GAME_ROOT}/game/hlvr"
csv="${hlvr}/vr_fidelity_levels_override.csv"
msaa=${ALYX_MSAA:-2}
fixed=${ALYX_FIXED_PERCENT:-}
table=${ALYX_FIDELITY_TABLE:-${repo}/scripts/alyx/fidelity-levels.csv}
[[ -d ${hlvr} ]] || { print -u2 "No Alyx in ${ALYX_GAME_ROOT}"; exit 1; }
[[ -n ${fixed} || -f ${table} ]] || { print -u2 "No fidelity table ${table}"; exit 1; }

steam=${ALYX_STEAM:-windows}
mac_steam="$HOME/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS"
stand_in=${ALYX_STEAM_STAND_IN:-${repo}/build-in-process/lsteamclient/mwxr-steam.exe}
case ${steam} in
  windows) ;;
  macos)
    [[ -f ${MWXR_IN_PROCESS_WINE}/lib/wine/x86_64-windows/lsteamclient.dll &&
       -f ${MWXR_IN_PROCESS_PREFIX}/drive_c/windows/system32/lsteamclient.dll ]] || {
      print -u2 "No lsteamclient in ${MWXR_IN_PROCESS_WINE} or the prefix: run scripts/install-lsteamclient.zsh"; exit 1; }
    [[ -f ${stand_in} && -f ${mac_steam}/steamclient.dylib ]] || {
      print -u2 "Need ${stand_in} (scripts/build-lsteamclient.zsh) and macOS Steam"; exit 1; } ;;
  *) print -u2 "ALYX_STEAM must be windows or macos"; exit 2 ;;
esac
if [[ ${ALYX_DRY_RUN:-0} != 1 ]]; then
  if [[ ${steam} == macos ]]; then
    pgrep -x steam_osx >/dev/null || { print -u2 'Start macOS Steam first'; exit 1; }
    pgrep -f 'steam\.exe' >/dev/null && { print -u2 'Quit Windows Steam first: one account cannot be signed in to both'; exit 1; }
  else
    pgrep -f 'steam\.exe' >/dev/null || { print -u2 'Start Windows Steam in the prefix first'; exit 1; }
  fi
  pgrep -x hlvr.exe >/dev/null && { print -u2 'Half-Life: Alyx is already running'; exit 1; }
fi

# Keep copies of what this changes, and put them back however the game ends.
keep=$(mktemp -d "${TMPDIR:-/tmp}/run-alyx-xrizer.XXXXXX")
saved=()
for f in "${csv}" "${MWXR_IN_PROCESS_PREFIX}"/drive_c/users/*/AppData/Local/openvr/openvrpaths.vrpath(N); do
  [[ -f $f ]] || continue
  cp -p "$f" "${keep}/${#saved}"
  saved+=("$f")
done
restore() {
  local i
  rm -f "${csv}"
  for (( i = 1; i <= ${#saved}; i++ )); do
    cp -p "${keep}/$((i - 1))" "${saved[$i]}"
  done
  rm -rf "${keep}"
}
trap restore EXIT

if [[ -n ${fixed} ]]; then
  # one level at scale 1; the size comes from the viewport percentage
  print -n "hlvr,default,vr_msaa,vr_render_target_scale,min_gpu_memory,min_width,min_height,
0,1,${msaa},1,,,,
end,,,,,,,
" > "${csv}"
  export OXR_VIEWPORT_SCALE_PERCENTAGE=${fixed}
  fidelity="+vr_fidelity_level_auto 0 +vr_fidelity_level 0 +vr_msaa ${msaa}"
  describe="fixed ${fixed}% of the recommended size, ${msaa}x MSAA"
else
  cp "${table}" "${csv}"
  unset OXR_VIEWPORT_SCALE_PERCENTAGE
  fidelity="+vr_fidelity_level_auto 1"
  describe="adaptive fidelity, levels from ${table:t}"
fi

export ALYX_START_STEAM=0
if [[ ${steam} == macos ]]; then
  export MWXR_LSTEAMCLIENT=1 STEAM_COMPAT_CLIENT_INSTALL_PATH=${mac_steam}
  export SteamAppId=546560 SteamGameId=546560 ALYX_STEAM_STAND_IN=${stand_in}
  describe_steam=", macOS Steam"
fi
export WINEMSYNC=${WINEMSYNC:-1}
export ALYX_BASE_ARGS="+map startup -novid -nowindow -console -vconsole ${fidelity} +vr_symmetric_fov 0"
print "Alyx: ${describe}${describe_steam:-}"
if [[ ${ALYX_DRY_RUN:-0} == 1 ]]; then
  print "fidelity table:"; cat "${csv}"
  env | grep -E '^(OXR_|ALYX_|MWXR_|WINEMSYNC|STEAM_COMPAT|SteamAppId)' | sort
  print "would run: ${repo}/scripts/run-in-process-alyx.zsh $*"
  exit 0
fi
"${repo}/scripts/run-in-process-alyx.zsh" "$@"

#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Start a Windows Steam game from the rig's prefix on the macOS Steam client
# (docs/steam-games.md): its launch entry from Valve's app info
# (tools/steam_games/steam_games.py), the macOS Steam client through lsteamclient and the
# mwxr-steam.exe stand-in (docs/lsteamclient.md), OpenXR through the in-process
# wineopenxr and OpenVR through xrizer. Half-Life: Alyx (546560) goes to
# run-alyx-xrizer.zsh, with its adaptive fidelity table.
#
# Usage: [GAME_MODE=vr|default] run-windows-game.zsh APPID [extra game arguments...]
#   GAME_MODE      the app's VR launch entry where it has one (default), or its default one
#   GAME_DRY_RUN=1 print the launch without starting the game
#
# Required (a rig wrapper sets them): MWXR_IN_PROCESS_PREFIX, MWXR_IN_PROCESS_WINE (a
# runtime with lsteamclient, scripts/install-lsteamclient.zsh), MWXR_XRIZER_ROOT,
# MWXR_NATIVE_RUNTIME_JSON. Optional: MWXR_IN_PROCESS_BUILD, GAME_LOG_DIR.
# macOS Steam must be running and signed in, and Windows Steam not running.
set -euo pipefail
repo=${0:A:h:h}
(( $# >= 1 )) || { print -u2 "usage: run-windows-game.zsh APPID [arguments...]"; exit 2; }
appid=$1; shift
: ${MWXR_IN_PROCESS_PREFIX:?Wine prefix with the game installed}
: ${MWXR_IN_PROCESS_WINE:?Wine runtime with lsteamclient}
: ${MWXR_XRIZER_ROOT:?xrizer provision}
: ${MWXR_NATIVE_RUNTIME_JSON:?x86_64 Monado runtime manifest}

if [[ ${appid} == 546560 ]]; then
  ALYX_STEAM=macos exec "${repo}/scripts/run-alyx-xrizer.zsh" "$@"
fi

mac_steam="$HOME/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS"
stand_in=${ALYX_STEAM_STAND_IN:-${repo}/build-in-process/lsteamclient/mwxr-steam.exe}
[[ -f ${MWXR_IN_PROCESS_WINE}/lib/wine/x86_64-windows/lsteamclient.dll &&
   -f ${MWXR_IN_PROCESS_PREFIX}/drive_c/windows/system32/lsteamclient.dll ]] || {
  print -u2 "No lsteamclient in ${MWXR_IN_PROCESS_WINE} or the prefix: run scripts/install-lsteamclient.zsh"; exit 1; }
[[ -f ${stand_in} ]] || { print -u2 "Need ${stand_in} (scripts/build-lsteamclient.zsh)"; exit 1; }

spec=$(MWXR_IN_PROCESS_PREFIX=${MWXR_IN_PROCESS_PREFIX} \
  python3 "${repo}/tools/steam_games/steam_games.py" launch-spec "${appid}" --mode "${GAME_MODE:-vr}" --json)
field() { python3 -c 'import json,sys; print(json.loads(sys.argv[1])[sys.argv[2]])' "${spec}" "$1"; }
name=$(field name) exe=$(field exe) arguments=$(field arguments) workdir=$(field workdir) entry=$(field type)
[[ -f ${exe} ]] || { print -u2 "${name}: ${exe} is missing"; exit 1; }

wine=${MWXR_IN_PROCESS_WINE}/bin/wine
export WINEPREFIX=${MWXR_IN_PROCESS_PREFIX} WINEMSYNC=${WINEMSYNC:-1}
export DYLD_FALLBACK_LIBRARY_PATH=${MWXR_IN_PROCESS_WINE}/lib/wine/x86_64-unix:${MWXR_IN_PROCESS_WINE}/lib
exe_windows=$(WINEDEBUG=-all "${wine}" winepath -w "${exe}" 2>/dev/null | tr -d '\r')
print -r -- "${name} (${appid}): ${entry} launch, ${exe_windows} ${arguments}"
if [[ ${GAME_DRY_RUN:-0} == 1 ]]; then
  print -r -- "working directory: ${workdir}"
  exit 0
fi
pgrep -x steam_osx >/dev/null || { print -u2 'Start macOS Steam first'; exit 1; }
pgrep -f 'steam\.exe' >/dev/null && { print -u2 'Quit Windows Steam first: one account cannot be signed in to both'; exit 1; }

# OpenVR games reach xrizer through the prefix's OpenVR path files, which SteamVR also
# uses: point them at xrizer, and put them back on exit.
keep=$(mktemp -d "${TMPDIR:-/tmp}/run-windows-game.XXXXXX")
saved=()
for f in "${MWXR_IN_PROCESS_PREFIX}"/drive_c/users/*/AppData/Local/openvr/openvrpaths.vrpath(N); do
  cp -p "$f" "${keep}/${#saved}"; saved+=("$f")
done
restore() {
  local i
  for (( i = 1; i <= ${#saved}; i++ )); do cp -p "${keep}/$((i - 1))" "${saved[$i]}"; done
  rm -rf "${keep}"
}
trap restore EXIT
xrizer_windows="Z:${MWXR_XRIZER_ROOT//\//\\}"
for profile in "${MWXR_IN_PROCESS_PREFIX}"/drive_c/users/*(/N); do
  [[ ${profile:t} == Public ]] && continue
  mkdir -p "${profile}/AppData/Local/openvr"
  print -r -- "{\"jsonid\": \"vrpathreg\", \"runtime\": [\"${xrizer_windows//\\/\\\\}\"], \"version\": 1}" \
    > "${profile}/AppData/Local/openvr/openvrpaths.vrpath"
done

logs=${GAME_LOG_DIR:-$HOME/Library/Logs/macos-wine-xr/${appid}-$(date +%Y%m%d-%H%M%S)}
mkdir -p "${logs}"
print "Logs: ${logs}"
export MWXR_LSTEAMCLIENT=1 STEAM_COMPAT_CLIENT_INSTALL_PATH=${mac_steam}
export SteamAppId=${appid} SteamGameId=${appid}
export VR_OVERRIDE=${xrizer_windows} XDG_STATE_HOME="Z:${logs//\//\\}"
export RUST_LOG=${RUST_LOG:-xrizer=info,openvr=warn}
cd "${workdir}"
"${repo}/scripts/run-in-process-openxr.zsh" "${stand_in}" -- "${exe_windows}" ${=arguments} "$@" 2>&1 |
  tee "${logs}/game.log"
exit ${pipestatus[1]}

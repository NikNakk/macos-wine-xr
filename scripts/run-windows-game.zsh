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
# Usage: [GAME_MODE=vr|default] [GAME_PREFIX=pergame|rig] run-windows-game.zsh APPID [arguments...]
#   GAME_MODE      the app's VR launch entry where it has one (default), or its default one
#   GAME_PREFIX    pergame: the game's own prefix (scripts/game-prefix.zsh), the default for
#                  games in the shared library; rig: MWXR_IN_PROCESS_PREFIX, the default for
#                  games installed inside it
#   GAME_DRY_RUN=1 print the launch without starting the game
# Stopping this script (SIGTERM, SIGINT or SIGHUP, as a launcher's Stop does) closes the
# game: the whole prefix's wineserver for a per-game prefix, the game's processes in the
# rig prefix.
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
[[ -f ${MWXR_IN_PROCESS_WINE}/lib/wine/x86_64-windows/lsteamclient.dll ]] || {
  print -u2 "No lsteamclient in ${MWXR_IN_PROCESS_WINE}: run scripts/install-lsteamclient.zsh"; exit 1; }
[[ -f ${stand_in} ]] || { print -u2 "Need ${stand_in} (scripts/build-lsteamclient.zsh)"; exit 1; }

spec=$(python3 "${repo}/tools/steam_games/steam_games.py" launch-spec "${appid}" --mode "${GAME_MODE:-vr}" --json)
field() { python3 -c 'import json,sys; print(json.loads(sys.argv[1])[sys.argv[2]])' "${spec}" "$1"; }
name=$(field name) exe=$(field exe) arguments=$(field arguments) workdir=$(field workdir) entry=$(field type)
library=$(field library)
[[ -f ${exe} ]] || { print -u2 "${name}: ${exe} is missing"; exit 1; }

prefix_kind=${GAME_PREFIX:-$([[ ${library} == shared ]] && print pergame || print rig)}
case ${prefix_kind} in
  pergame) MWXR_IN_PROCESS_PREFIX=$("${repo}/scripts/game-prefix.zsh" ensure "${appid}") ;;
  rig) ;;
  *) print -u2 "GAME_PREFIX must be pergame or rig"; exit 2 ;;
esac
export MWXR_IN_PROCESS_PREFIX
[[ -f ${MWXR_IN_PROCESS_PREFIX}/drive_c/windows/system32/lsteamclient.dll ]] || {
  print -u2 "No lsteamclient in ${MWXR_IN_PROCESS_PREFIX}: run scripts/install-lsteamclient.zsh"; exit 1; }

# steam_api loads the Windows steamclient64.dll named in the registry; the redirect never
# runs it, so one copy serves every prefix: MWXR_STEAM_CLIENT_DIR (default
# ~/Windows/library/steam-client), taken once from the rig prefix's Windows Steam.
steam_client=${MWXR_STEAM_CLIENT_DIR:-${MWXR_GAME_LIBRARY:-$HOME/Windows/library}/steam-client}
if [[ ! -f ${steam_client}/steamclient64.dll ]]; then
  rig_steam="${MWXR_RIG_PREFIX:-$HOME/Windows/prefix}/drive_c/Program Files (x86)/Steam"
  [[ -f ${rig_steam}/steamclient64.dll ]] || {
    print -u2 "No Windows steamclient64.dll in ${steam_client} (or ${rig_steam} to copy it from)"; exit 1; }
  mkdir -p "${steam_client}"
  cp -p "${rig_steam}/steamclient64.dll" "${rig_steam}/steamclient.dll" "${steam_client}/"
  print -u2 "Copied Windows Steam's steamclient DLLs to ${steam_client}"
fi

wine=${MWXR_IN_PROCESS_WINE}/bin/wine
export WINEPREFIX=${MWXR_IN_PROCESS_PREFIX} WINEMSYNC=${WINEMSYNC:-1}
export DYLD_FALLBACK_LIBRARY_PATH=${MWXR_IN_PROCESS_WINE}/lib/wine/x86_64-unix:${MWXR_IN_PROCESS_WINE}/lib
exe_windows=$(WINEDEBUG=-all "${wine}" winepath -w "${exe}" 2>/dev/null | tr -d '\r')
print -r -- "${name} (${appid}): ${entry} launch, ${exe_windows} ${arguments}"
print -r -- "prefix (${prefix_kind}): ${MWXR_IN_PROCESS_PREFIX}"
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
# The game runs as a background job so a stop signal is handled at once rather than after
# the game exits.
stop_game() {
  trap - TERM INT HUP
  print -u2 "Stopping ${name}"
  if [[ ${prefix_kind} == pergame ]]; then
    "${MWXR_IN_PROCESS_WINE}/bin/wineserver" -k 2>/dev/null || true
  else
    WINEDEBUG=-all "${wine}" taskkill /f /im "${exe:t}" >/dev/null 2>&1 || true
    WINEDEBUG=-all "${wine}" taskkill /f /im mwxr-steam.exe >/dev/null 2>&1 || true
  fi
  wait 2>/dev/null || true
  exit 143
}
trap stop_game TERM INT HUP
steam_client_windows=$(WINEDEBUG=-all "${wine}" winepath -w "${steam_client}" 2>/dev/null | tr -d '\r')
"${repo}/scripts/run-in-process-openxr.zsh" "${stand_in}" --steam-dir "${steam_client_windows}" -- \
  "${exe_windows}" ${=arguments} "$@" > "${logs}/game.log" 2>&1 &
game_pid=$!
wait ${game_pid}

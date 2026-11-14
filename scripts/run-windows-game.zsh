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
#   GAME_EXE       the game's exe (unix path) instead of its launch entry, and GAME_WORKDIR
#                  its working folder: set by the Steam Play tool (docs/steam-play.md), for
#                  games macOS Steam installed itself; these get their own prefix
#   GAME_SETUP_SCRIPT  a Steam install script (evaluatorscript_<appid>.vdf) to apply to the
#                  game's prefix instead of starting it, also from the Steam Play tool
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

# Steam Cloud finds a Steam Play game's Windows save folders under
# STEAM_COMPAT_DATA_PATH/pfx/drive_c/users/steamuser (docs/steam-play.md, Steam Cloud):
# link pfx to the game's own prefix, and steamuser to the prefix's Wine user. An existing
# pfx that is not a link is left alone.
link_compat_data() {  # prefix
  local data=${STEAM_COMPAT_DATA_PATH:-} users=$1/drive_c/users user
  [[ -n ${data} && -d ${users} ]] || return 0
  mkdir -p "${data}"
  if [[ -L ${data}/pfx || ! -e ${data}/pfx ]]; then
    [[ $(readlink "${data}/pfx" 2>/dev/null) == "$1" ]] || ln -sfn "$1" "${data}/pfx"
  fi
  if [[ ! -e ${users}/steamuser ]]; then
    for user in "${users}"/*(/N:t); do
      [[ ${user} == (Public|steamuser) ]] || { ln -s "${user}" "${users}/steamuser"; break; }
    done
  fi
}

if [[ -n ${GAME_SETUP_SCRIPT:-} ]]; then
  # A Steam install script for the game (docs/steam-play.md, Setup scripts): its Registry
  # steps go into the game's own prefix; installers are reported and skipped. Steam calls
  # this when the install finishes and before each start, so an unchanged result is not
  # imported again. Steam syncs Cloud saves after this call and before the start, so the
  # prefix and its Cloud links are made here.
  # On uninstall Steam sends the script again with mode 3; nothing of it is applied, and
  # the game's prefix is neither made nor removed.
  if python3 "${repo}/tools/steam_play/install_script.py" "${GAME_SETUP_SCRIPT}" --uninstall; then
    print "Uninstall script for ${appid}: nothing to do; its prefix is kept"
    exit 0
  fi
  work=$(mktemp -d "${TMPDIR:-/tmp}/steam-setup.XXXXXX")
  trap 'rm -rf "${work}"' EXIT
  # Half-Life: Alyx's Wine route runs in the rig prefix, not a prefix of its own, and keeps
  # its saves in its game folder.
  if [[ ${appid} == 546560 ]]; then
    prefix=${MWXR_IN_PROCESS_PREFIX}
  else
    prefix=$("${repo}/scripts/game-prefix.zsh" ensure "${appid}")
    link_compat_data "${prefix}"
  fi
  python3 "${repo}/tools/steam_play/install_script.py" "${GAME_SETUP_SCRIPT}" --reg "${work}/setup.reg"
  [[ -f ${work}/setup.reg ]] || exit 0
  stamp=${prefix}/.mwxr-setup-${appid}.sha1
  digest=$(shasum "${work}/setup.reg" | cut -d' ' -f1)
  if [[ -f ${stamp} && $(<"${stamp}") == "${digest}" ]]; then
    print "Setup registry values for ${appid} already in ${prefix}"
    exit 0
  fi
  export WINEPREFIX=${prefix} WINEDEBUG=-all WINEMSYNC=${WINEMSYNC:-1}
  export DYLD_FALLBACK_LIBRARY_PATH=${MWXR_IN_PROCESS_WINE}/lib/wine/x86_64-unix:${MWXR_IN_PROCESS_WINE}/lib
  "${MWXR_IN_PROCESS_WINE}/bin/wine" reg import "$(WINEDEBUG=-all "${MWXR_IN_PROCESS_WINE}/bin/wine" winepath -w "${work}/setup.reg" | tr -d '\r')"
  "${MWXR_IN_PROCESS_WINE}/bin/wineserver" -w  # the registry is saved when the server exits
  print -r -- "${digest}" > "${stamp}"
  print "Imported setup registry values for ${appid} into ${prefix}"
  exit 0
fi

if [[ ${appid} == 546560 ]]; then
  # MWXR_ALYX=native (from Steam's Launch Options): the native ARM64 build instead.
  if [[ ${MWXR_ALYX:-wine} == native ]]; then
    exec "${repo}/scripts/run-alyx-native.zsh" "$@"
  fi
  # From Steam Play, the copy Steam installed (STEAM_COMPAT_INSTALL_PATH) rather than the
  # rig prefix's; the rig prefix still holds the saves and settings.
  if [[ -n ${GAME_EXE:-} && -n ${STEAM_COMPAT_INSTALL_PATH:-} ]]; then
    export ALYX_GAME_ROOT=${ALYX_GAME_ROOT:-${STEAM_COMPAT_INSTALL_PATH}}
  fi
  ALYX_STEAM=macos exec "${repo}/scripts/run-alyx-xrizer.zsh" "$@"
fi

mac_steam="$HOME/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS"
stand_in=${ALYX_STEAM_STAND_IN:-${repo}/build-in-process/lsteamclient/mwxr-steam.exe}
[[ -f ${MWXR_IN_PROCESS_WINE}/lib/wine/x86_64-windows/lsteamclient.dll ]] || {
  print -u2 "No lsteamclient in ${MWXR_IN_PROCESS_WINE}: run scripts/install-lsteamclient.zsh"; exit 1; }
[[ -f ${stand_in} ]] || { print -u2 "Need ${stand_in} (scripts/build-lsteamclient.zsh)"; exit 1; }

if [[ -n ${GAME_EXE:-} ]]; then
  # Started by macOS Steam's Steam Play (tools/steam_play): Steam installed the game in its
  # own library and chose the launch entry, whose arguments come as this script's.
  name=$(python3 "${repo}/tools/steam_games/steam_games.py" info "${appid}" --json 2>/dev/null |
    python3 -c 'import json,sys; print(json.load(sys.stdin)["name"])' 2>/dev/null) || name="App ${appid}"
  exe=${GAME_EXE} arguments='' workdir=${GAME_WORKDIR:-${GAME_EXE:h}} entry=steam-play library=steam
else
  spec=$(python3 "${repo}/tools/steam_games/steam_games.py" launch-spec "${appid}" --mode "${GAME_MODE:-vr}" --json)
  field() { python3 -c 'import json,sys; print(json.loads(sys.argv[1])[sys.argv[2]])' "${spec}" "$1"; }
  name=$(field name) exe=$(field exe) arguments=$(field arguments) workdir=$(field workdir) entry=$(field type)
  library=$(field library)
fi
[[ -f ${exe} ]] || { print -u2 "${name}: ${exe} is missing"; exit 1; }
# Reject unsupported launcher architectures before touching the prefix or
# starting Wine, and keep the normal Steam entry rather than guessing a game EXE.
python3 "${repo}/tools/steam_games/check_launch.py" \
 --exe "${exe}" --wine-runtime "${MWXR_IN_PROCESS_WINE}" --name "${name}" --steam-bridge

prefix_kind=${GAME_PREFIX:-$([[ ${library} == (shared|steam) ]] && print pergame || print rig)}
case ${prefix_kind} in
  pergame) MWXR_IN_PROCESS_PREFIX=$("${repo}/scripts/game-prefix.zsh" ensure "${appid}")
           link_compat_data "${MWXR_IN_PROCESS_PREFIX}" ;;
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
# The 32-bit launcher and its 64-bit child need different Steam client DLLs.
for steam_dll in steamclient64.dll steamclient.dll; do
  if [[ ! -f ${steam_client}/${steam_dll} ]]; then
    rig_steam="${MWXR_RIG_PREFIX:-$HOME/Windows/prefix}/drive_c/Program Files (x86)/Steam"
    [[ -f ${rig_steam}/${steam_dll} ]] || {
      print -u2 "No Windows ${steam_dll} in ${steam_client} (or ${rig_steam} to copy it from)"; exit 1; }
    mkdir -p "${steam_client}"
    cp -p "${rig_steam}/${steam_dll}" "${steam_client}/"
    print -u2 "Copied Windows Steam's ${steam_dll} to ${steam_client}"
  fi
done

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
# A game Steam started (GAME_EXE) has Steam running by definition, and pgrep run under
# Steam does not list steam_osx.
[[ -n ${GAME_EXE:-} ]] || pgrep -x steam_osx >/dev/null || { print -u2 'Start macOS Steam first'; exit 1; }
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
# Wine does not pass XDG_* variables to Windows programs; xrizer also reads this one.
export XRIZER_STATE_HOME=${XDG_STATE_HOME}
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
# UEVR's settings live in one folder shared by every prefix (tools/steam_play/uevr_profile.py).
[[ ${MWXR_UEVR:-0} == 1 ]] && python3 "${repo}/tools/steam_play/uevr_profile.py" link "${MWXR_IN_PROCESS_PREFIX}"
"${repo}/scripts/run-in-process-openxr.zsh" "${stand_in}" --steam-dir "${steam_client_windows}" -- \
  "${exe_windows}" ${=arguments} "$@" > "${logs}/game.log" 2>&1 &
game_pid=$!
# MWXR_UEVR=1 (Steam Launch Options): inject UEVR into an Unreal Engine game once its window
# is up (docs/uevr.md). MWXR_UEVR_EXE names the process when Steam starts a bootstrap exe.
if [[ ${MWXR_UEVR:-0} == 1 ]]; then
  uevr_dir=${MWXR_UEVR_DIR:-$HOME/Windows/uevr/current}
  injector=${MWXR_UEVR_INJECTOR:-${repo}/build-in-process/uevr/mwxr-uevr-inject.exe}
  if [[ -f ${uevr_dir}/UEVRBackend.dll && -f ${injector} ]]; then
    uevr_args=(--exe "${MWXR_UEVR_EXE:-${exe:t}}" --runtime "${MWXR_UEVR_RUNTIME:-openxr}"
      --delay "${MWXR_UEVR_DELAY:-5}"
      --dir "$(WINEDEBUG=-all "${wine}" winepath -w "${uevr_dir:A}" 2>/dev/null | tr -d '\r')")
    [[ ${MWXR_UEVR_NULLIFY:-0} == 1 ]] && uevr_args+=(--nullify-plugins)
    print "UEVR: ${uevr_dir:A} (${MWXR_UEVR_RUNTIME:-openxr}), log ${logs}/uevr.log"
    WINEDEBUG=-all "${wine}" "${injector}" "${uevr_args[@]}" > "${logs}/uevr.log" 2>&1 &
  else
    print -u2 "MWXR_UEVR=1: need ${uevr_dir}/UEVRBackend.dll and ${injector} (scripts/build-uevr-injector.zsh)"
  fi
fi
wait ${game_pid}

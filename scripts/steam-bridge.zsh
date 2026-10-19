# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Sourced by launchers (MWXR_STEAM_BRIDGE=1): games reach the Mac Steam client
# through the Steam bridge instead of a Windows Steam client in the prefix.
# See docs/steam-bridge.md. Needs ${repo}.
#
#   steam_bridge_start <wine> <prefix> <log dir>   install, start the stand-in
#   steam_bridge_stop                              stop it; registry put back
#
# MWXR_STEAM_BRIDGE_BUILD  built by scripts/build-steam-bridge.zsh
#                          (default ${repo}/build-steam-bridge)
# MWXR_STEAM_CLIENT        another steamclient.dylib (default: the Mac Steam app's)
steam_bridge_root=${MWXR_STEAM_BRIDGE_BUILD:-${repo}/build-steam-bridge}
steam_bridge_started=0

steam_bridge_start() {
  local wine=$1 prefix=$2 logs=$3 i
  [[ -f ${steam_bridge_root}/x86_64-unix/steamclient64.so && -f ${steam_bridge_root}/steam_standin.exe ]] || {
    print -u2 "Build the Steam bridge first: scripts/build-steam-bridge.zsh (${steam_bridge_root})"; exit 1
  }
  pgrep -f 'Steam.AppBundle/Steam/Contents/MacOS/steam_osx' >/dev/null || {
    print -u2 "Start Steam on the Mac and sign in first: the Steam bridge uses its session"; exit 1
  }
  # steam_api64.dll loads the path the stand-in registers; Wine maps the
  # builtin there to WINEDLLPATH's copy and its native half.
  cp "${steam_bridge_root}/x86_64-windows/steamclient64.dll" "${prefix}/drive_c/windows/system32/steamclient64.dll"
  export MWXR_EXTRA_WINEDLLPATH=${steam_bridge_root}${MWXR_EXTRA_WINEDLLPATH:+:${MWXR_EXTRA_WINEDLLPATH}}
  WINEPREFIX=${prefix} WINEDEBUG=-all "${wine}" "${steam_bridge_root}/steam_standin.exe" \
    'C:\windows\system32\steamclient64.dll' > "${logs}/steam-standin.log" 2>&1 &!
  for i in {1..60}; do
    grep -q '^steam_standin: pid' "${logs}/steam-standin.log" 2>/dev/null && break
    sleep 0.5
  done
  grep -q '^steam_standin: pid' "${logs}/steam-standin.log" 2>/dev/null || {
    print -u2 "The Steam stand-in did not start; see ${logs}/steam-standin.log"; exit 1
  }
  steam_bridge_wine=${wine} steam_bridge_prefix=${prefix} steam_bridge_started=1
  print "Steam bridge: games use the Mac Steam client ($(head -1 "${logs}/steam-standin.log"))"
}

steam_bridge_stop() {
  [[ ${steam_bridge_started} == 1 ]] || return 0
  WINEPREFIX=${steam_bridge_prefix} WINEDEBUG=-all "${steam_bridge_wine}" \
    "${steam_bridge_root}/steam_standin.exe" --stop >/dev/null 2>&1 || true
  steam_bridge_started=0
}

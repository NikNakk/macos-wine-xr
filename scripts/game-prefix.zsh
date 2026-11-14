#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# One Wine prefix per game, as Proton keeps one per app (docs/steam-games.md).
#
# Shared things live once, outside the prefixes: the Wine runtime with DXMT and
# lsteamclient, wineopenxr (the in-process build), xrizer, and the game library
# (~/Windows/library). What each prefix needs a copy of, Wine's system32 (about 0.9 GB of
# built-in DLLs) and registry, comes from a template prefix as an APFS clone, so blocks
# are shared until a game changes them. (Symlinks to the runtime would save the same
# space but are unsafe: Wine's own prefix updates write through them into the runtime.)
# du counts each clone in full; diskutil apfs and Finder's "Get Info" do not.
#
# Usage: game-prefix.zsh template           make (or remake) the template prefix
#        game-prefix.zsh ensure APPID       print the game's prefix, cloning it if new
#        game-prefix.zsh upgrade PREFIX     bring any prefix (the rig's, for one) up to the
#                                           runtime: its 32-bit side, Wine's update and
#                                           its built-ins shared with the runtime
#        game-prefix.zsh list               the game prefixes
# Environment: MWXR_IN_PROCESS_WINE (the runtime the template is made with),
# MWXR_GAME_PREFIXES (default ~/Windows/prefixes).
set -euo pipefail
repo=${0:A:h:h}
root=${MWXR_GAME_PREFIXES:-$HOME/Windows/prefixes}
template=${root}/template
cmd=${1:-}

wine_env() {
  : ${MWXR_IN_PROCESS_WINE:?Wine runtime to make the template with}
  export WINEPREFIX=$1 WINEARCH=win64 WINEDEBUG=-all WINEMSYNC=${WINEMSYNC:-1}
  export DYLD_FALLBACK_LIBRARY_PATH=${MWXR_IN_PROCESS_WINE}/lib/wine/x86_64-unix:${MWXR_IN_PROCESS_WINE}/lib
}

# The runtime's own DLLs that a prefix's system32 must also hold (Wine finds a built-in
# only through a file there, and CrossOver's prefix setup does not place DXMT's dxgi.dll):
# synced on every ensure, as APFS clones of the runtime's files, so a DXMT or
# lsteamclient update reaches every game prefix.
SYNCED=(d3d10core.dll d3d11.dll dxgi.dll winemetal.dll lsteamclient.dll ntdll.dll)
# wineboot -u without Wine Mono and Gecko, stopped after MWXR_PREFIX_UPDATE_TIMEOUT seconds
# (default 300). In a prefix with Microsoft's .NET (Assetto Corsa's), registering the
# 32-bit mscoree.dll never returns, even with it disabled (setupapi loads it by path).
# Wine records the update before it runs, so a stopped one is not tried again at launch.
run_update() {  # prefix
  local limit=${MWXR_PREFIX_UPDATE_TIMEOUT:-300} waited=0 pid
  ( wine_env "$1"
    WINEDLLOVERRIDES="mscoree=;mshtml=" "${MWXR_IN_PROCESS_WINE}/bin/wine" wineboot -u
    "${MWXR_IN_PROCESS_WINE}/bin/wineserver" -w ) &
  pid=$!
  while kill -0 ${pid} 2>/dev/null; do
    if (( waited++ >= limit )); then
      print -u2 "Prefix update of $1 still running after ${limit}s; stopping it and carrying on"
      WINEPREFIX=$1 "${MWXR_IN_PROCESS_WINE}/bin/wineserver" -k 2>/dev/null || true
      break
    fi
    sleep 1
  done
  wait ${pid} 2>/dev/null || true
}
# A prefix made by a 64-bit-only runtime has no syswow64. The WoW64 runtime runs its 64-bit
# programs unchanged; Wine's prefix update adds the 32-bit side. The prefix is kept first,
# as an APFS clone, in <root>/.backups.
upgrade_to_wow64() {  # prefix
  local backup=${root}/.backups/${1:t}-before-wow64-$(date +%Y%m%d-%H%M%S)
  mkdir -p "${root}/.backups"
  WINEPREFIX=$1 "${MWXR_IN_PROCESS_WINE}/bin/wineserver" -k 2>/dev/null || true
  cp -cpR "$1" "${backup}"
  print -u2 "Updating $1 for 32-bit programs (WoW64); the previous prefix is ${backup}"
  run_update "$1"
  [[ -f $1/drive_c/windows/syswow64/ntdll.dll ]] || {
    print -u2 "Prefix $1 still has no WoW64 system DLLs after the update"; return 1; }
}
# Wine updates a prefix by itself when its runtime's wine.inf changes, at the first
# program started there, and that update offers Wine Mono and Gecko in a dialog the game
# launch never shows: winepath waited on it for good. Do the update here instead, without
# them (as the template is made), so a runtime update cannot stall a launch.
update_prefix() {  # prefix
  local inf=${MWXR_IN_PROCESS_WINE}/share/wine/wine.inf
  [[ -f ${inf} ]] || return 0
  local want=$(stat -f %m "${inf}") have=$(tr -d "\r\n" < "$1/.update-timestamp" 2>/dev/null || true)
  [[ ${have} == (${want}|disable) ]] && return 0
  print -u2 "Updating $1 for ${MWXR_IN_PROCESS_WINE:t}"
  run_update "$1"
}
sync_runtime_dlls() {  # prefix
  local f src dst
  if [[ -d ${MWXR_IN_PROCESS_WINE}/lib/wine/i386-windows &&
        ! -f $1/drive_c/windows/syswow64/ntdll.dll ]]; then
    upgrade_to_wow64 "$1" || return 1
  fi
  update_prefix "$1"
  for f in ${SYNCED}; do
    src=${MWXR_IN_PROCESS_WINE}/lib/wine/x86_64-windows/${f} dst=$1/drive_c/windows/system32/${f}
    [[ -f ${src} ]] || continue
    cmp -s "${src}" "${dst}" 2>/dev/null && continue
    cp -c "${src}" "${dst}.new" && mv "${dst}.new" "${dst}"
  done
  if [[ -f ${MWXR_IN_PROCESS_WINE}/lib/wine/i386-windows/lsteamclient.dll ]]; then
    for f in ntdll.dll lsteamclient.dll; do
      src=${MWXR_IN_PROCESS_WINE}/lib/wine/i386-windows/${f} dst=$1/drive_c/windows/syswow64/${f}
      cmp -s "${src}" "${dst}" 2>/dev/null && continue
      cp -c "${src}" "${dst}.new" && mv "${dst}.new" "${dst}"
    done
  fi
  share_builtins "$1"
}
# Every other Wine built-in in the prefix as an APFS clone of the runtime's, so a Wine
# update does not leave each prefix with its own 1.4 GB copy (share_prefix_dlls.py).
share_builtins() {  # prefix
  python3 "${repo}/tools/steam_games/share_prefix_dlls.py" "$1" "${MWXR_IN_PROCESS_WINE}" ||
    print -u2 "Could not share $1's Wine DLLs with ${MWXR_IN_PROCESS_WINE}; carrying on"
}

make_template() {
  wine_env "${template}.part"
  rm -rf "${template}.part"
  mkdir -p "${root}"
  print -u2 "Making the game prefix template with ${MWXR_IN_PROCESS_WINE}"
  WINEDLLOVERRIDES="mscoree=;mshtml=" "${MWXR_IN_PROCESS_WINE}/bin/wine" wineboot --init
  "${MWXR_IN_PROCESS_WINE}/bin/wineserver" -w   # let it write the registry before cloning
  sync_runtime_dlls "${template}.part"
  print -r -- "runtime=${MWXR_IN_PROCESS_WINE}" > "${template}.part/.mwxr-template"
  print -r -- "made=$(date '+%Y-%m-%d %H:%M:%S')" >> "${template}.part/.mwxr-template"
  rm -rf "${template}"
  mv "${template}.part" "${template}"
}

case ${cmd} in
  template)
    make_template
    print -r -- "${template}" ;;
  ensure)
    appid=${2:?usage: game-prefix.zsh ensure APPID}
    [[ ${appid} == <-> ]] || { print -u2 "APPID must be a number"; exit 2; }
    prefix=${root}/${appid}
    if [[ ! -f ${prefix}/system.reg ]]; then
      [[ -f ${template}/system.reg ]] || make_template
      # nothing may be running in the template while it is cloned
      if [[ -n ${MWXR_IN_PROCESS_WINE:-} ]]; then
        WINEPREFIX=${template} "${MWXR_IN_PROCESS_WINE}/bin/wineserver" -k 2>/dev/null || true
      fi
      rm -rf "${prefix}"
      cp -cpR "${template}" "${prefix}"
      rm -f "${prefix}/.mwxr-template"
      print -r -- "appid=${appid} cloned=$(date '+%Y-%m-%d %H:%M:%S')" > "${prefix}/.mwxr-game-prefix"
      print -u2 "New prefix for ${appid}: ${prefix}"
    fi
    [[ -n ${MWXR_IN_PROCESS_WINE:-} ]] && sync_runtime_dlls "${prefix}"
    print -r -- "${prefix}" ;;
  upgrade)
    prefix=${2:?usage: game-prefix.zsh upgrade PREFIX}
    [[ -f ${prefix}/system.reg ]] || { print -u2 "${prefix} is not a Wine prefix"; exit 2; }
    : ${MWXR_IN_PROCESS_WINE:?Wine runtime to upgrade the prefix for}
    if [[ -d ${MWXR_IN_PROCESS_WINE}/lib/wine/i386-windows &&
          ! -f ${prefix}/drive_c/windows/syswow64/ntdll.dll ]]; then
      upgrade_to_wow64 "${prefix}"
    fi
    update_prefix "${prefix}"
    share_builtins "${prefix}"
    print -r -- "${prefix}" ;;
  list)
    for p in "${root}"/<->(/N); do
      print -r -- "${p:t}  $(du -sh "${p}" 2>/dev/null | cut -f1) (clone of the template; du counts shared blocks)"
    done ;;
  *) print -u2 "usage: game-prefix.zsh template | ensure APPID | upgrade PREFIX | list"; exit 2 ;;
esac

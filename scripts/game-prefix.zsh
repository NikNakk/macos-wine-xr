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
#        game-prefix.zsh list               the game prefixes
# Environment: MWXR_IN_PROCESS_WINE (the runtime the template is made with),
# MWXR_GAME_PREFIXES (default ~/Windows/prefixes).
set -euo pipefail
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
SYNCED=(d3d10core.dll d3d11.dll dxgi.dll winemetal.dll lsteamclient.dll)
sync_runtime_dlls() {  # prefix
  local f src dst
  for f in ${SYNCED}; do
    src=${MWXR_IN_PROCESS_WINE}/lib/wine/x86_64-windows/${f} dst=$1/drive_c/windows/system32/${f}
    [[ -f ${src} ]] || continue
    cmp -s "${src}" "${dst}" 2>/dev/null && continue
    cp -c "${src}" "${dst}.new" && mv "${dst}.new" "${dst}"
  done
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
  list)
    for p in "${root}"/<->(/N); do
      print -r -- "${p:t}  $(du -sh "${p}" 2>/dev/null | cut -f1) (clone of the template; du counts shared blocks)"
    done ;;
  *) print -u2 "usage: game-prefix.zsh template | ensure APPID | list"; exit 2 ;;
esac

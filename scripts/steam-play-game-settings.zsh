#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# Source from a rig wrapper, before assigning its default runtime. Explicit
# environment overrides remain authoritative. $1 is the Steam app ID.
#
# Games with a 32-bit Windows executable get the WoW64 runtime and their own prefix root
# (docs/steam-play.md, WoW64). From Steam Play, the game's install folder
# (STEAM_COMPAT_INSTALL_PATH) and exe (GAME_EXE) decide it, so its setup call and its
# launch agree (tools/steam_play/game_arch.py). MWXR_WOW64=1 or 0, also from the game's
# Launch Options, overrides that. Assetto Corsa (launcher 32-bit, race engine 64-bit)
# uses WoW64 when started outside Steam Play too.
_mwxr_settings_app=${1:-}
_mwxr_wow64=${MWXR_WOW64:-}
if [[ -z ${_mwxr_wow64} && -n ${_mwxr_settings_app} &&
      ( -n ${STEAM_COMPAT_INSTALL_PATH:-} || -n ${GAME_EXE:-} ) ]]; then
  _mwxr_args=(--appid "${_mwxr_settings_app}")
  [[ -n ${STEAM_COMPAT_INSTALL_PATH:-} ]] && _mwxr_args+=(--install-path "${STEAM_COMPAT_INSTALL_PATH}")
  [[ -n ${GAME_EXE:-} ]] && _mwxr_args+=(--exe "${GAME_EXE}")
  _mwxr_arch=$(python3 "${${(%):-%x}:A:h:h}/tools/steam_play/game_arch.py" "${_mwxr_args[@]}" 2>/dev/null) ||
    _mwxr_arch=unknown
  [[ ${_mwxr_arch} == 32 ]] && _mwxr_wow64=1
fi
[[ -z ${_mwxr_wow64} && ${_mwxr_settings_app} == 244210 ]] && _mwxr_wow64=1
if [[ ${_mwxr_wow64} == 1 ]]; then
  export MWXR_IN_PROCESS_WINE=${MWXR_IN_PROCESS_WINE:-$HOME/Code/wine-crossover/runtime-wow64}
  export MWXR_GAME_PREFIXES=${MWXR_GAME_PREFIXES:-$HOME/Windows/prefixes-wow64}
  export GAME_PREFIX=${GAME_PREFIX:-pergame}
fi
unset _mwxr_settings_app _mwxr_wow64 _mwxr_arch _mwxr_args

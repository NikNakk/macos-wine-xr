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
# Google Earth VR asks for an OpenGL 3.2 compatibility context, which winemac's OpenGL
# does not offer: it runs on Zink over KosmicKrisp (docs/zink-kosmickrisp.md).
# MWXR_OPENGL=wine, also from the game's Launch Options, keeps Wine's OpenGL.
[[ ${_mwxr_settings_app} == 348250 ]] && export MWXR_OPENGL=${MWXR_OPENGL:-zink}
# Windows Vulkan games run on KosmicKrisp through wineopenxr's Vulkan path
# (docs/vulkan-bridge.md), with the private Wine whose FreeType name is fixed (readable
# dialogs) and xrizer with the queue-family fix. The Talos Principle VR needs
# "+gfx_strAPI Vulkan" in its Launch Options (otherwise it uses D3D11 on DXMT).
# MWXR_VULKAN=off, also from the Launch Options, keeps the ordinary setup.
if [[ ${_mwxr_settings_app} == (552440|275850) && ${MWXR_VULKAN:-} != off ]]; then
  _mwxr_root=${MWXR_FOVEATION_ROOT:-${${(%):-%x}:A:h:h}/build-in-process}
  export MWXR_VULKAN=kosmickrisp
  export MWXR_KK_ROOT=${MWXR_KK_ROOT:-${_mwxr_root}/zink-fov}
  export MWXR_IN_PROCESS_WINE=${MWXR_IN_PROCESS_WINE:-${_mwxr_root}/vulkan-wine}
  export MWXR_XRIZER_ROOT=${MWXR_XRIZER_ROOT:-${_mwxr_root}/vulkan-xrizer}
  export MWXR_IN_PROCESS_BUILD=${MWXR_IN_PROCESS_BUILD:-${_mwxr_root}/vk-fov-gate}
  # No Man's Sky stops on a "driver does not support wide lines" box; the private
  # KosmicKrisp claims them (drawn one pixel wide).
  [[ ${_mwxr_settings_app} == 275850 ]] && export KK_FAKE_WIDE_LINES=${KK_FAKE_WIDE_LINES:-1}
  # NMS renders single-sampled, at a render scale (about 59%), and reads and writes its
  # eye targets from compute: with MWXR_FOVEATION those are foveated too, and compute
  # passes that only write them run one invocation per foveated texel.
  if [[ ${_mwxr_settings_app} == 275850 ]]; then
    export KK_IMPLICIT_SINGLE=${KK_IMPLICIT_SINGLE:-1} KK_IMPLICIT_SCALED=${KK_IMPLICIT_SCALED:-1}
    export KK_IMPLICIT_STORAGE=${KK_IMPLICIT_STORAGE:-1} KK_IMPLICIT_COMPACT=${KK_IMPLICIT_COMPACT:-1}
  fi
  # Talos renders single-sampled (and some effects at half size): with MWXR_FOVEATION,
  # KosmicKrisp foveates those targets too (docs/kk-implicit-foveation.md).
  if [[ ${_mwxr_settings_app} == 552440 ]]; then
    export KK_IMPLICIT_SINGLE=${KK_IMPLICIT_SINGLE:-1} KK_IMPLICIT_HALF=${KK_IMPLICIT_HALF:-1}
  fi
  # MWXR_KK_PASS_TIMING=1 from the Launch Options (macOS Steam passes only MWXR_ words to
  # the tool as settings): KosmicKrisp's per-pass GPU timing in the game log.
  [[ ${MWXR_KK_PASS_TIMING:-} == 1 ]] && export KK_PASS_TIMING=1
  unset _mwxr_root
fi
[[ ${MWXR_VULKAN:-} == off ]] && unset MWXR_VULKAN
# MWXR_FOVEATION=fixed|gaze[:numbers] (docs/foveation.md), also from the game's Launch
# Options: the runtime copy that carries the foveation DXMT, unless the caller chose one.
# scripts/openxr-launch-settings.zsh checks the value and sets the rest.
if [[ -n ${MWXR_FOVEATION:-} && ${MWXR_FOVEATION} != off && ${MWXR_VULKAN:-} != kosmickrisp ]]; then
  _mwxr_fov=${MWXR_FOVEATION_ROOT:-${${(%):-%x}:A:h:h}/build-in-process}
  if [[ ${_mwxr_wow64} == 1 ]]; then
    export MWXR_IN_PROCESS_WINE=${MWXR_IN_PROCESS_WINE:-${_mwxr_fov}/wine-foveation-wow64}
  else
    export MWXR_IN_PROCESS_WINE=${MWXR_IN_PROCESS_WINE:-${_mwxr_fov}/wine-foveation}
  fi
  unset _mwxr_fov
fi
if [[ ${_mwxr_wow64} == 1 ]]; then
  export MWXR_IN_PROCESS_WINE=${MWXR_IN_PROCESS_WINE:-$HOME/Code/wine-crossover/runtime-wow64}
  export MWXR_GAME_PREFIXES=${MWXR_GAME_PREFIXES:-$HOME/Windows/prefixes-wow64}
  export GAME_PREFIX=${GAME_PREFIX:-pergame}
fi
unset _mwxr_settings_app _mwxr_wow64 _mwxr_arch _mwxr_args

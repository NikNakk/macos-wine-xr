#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# Source from a rig wrapper, before assigning its default runtime. Explicit
# environment overrides remain authoritative. $1 is the Steam app ID.
#
# Every game uses the one WoW64 runtime (~/Code/wine-crossover/runtime-unified, set by the
# rig wrapper) and the one prefix root: 64-bit programs run in it as they do in a
# 64-bit-only Wine, and 32-bit ones (launchers such as Assetto Corsa's) run too
# (docs/steam-play.md, WoW64). What follows are per-game exceptions.
_mwxr_settings_app=${1:-}
# Google Earth VR asks for an OpenGL 3.2 compatibility context, which winemac's OpenGL
# does not offer: it runs on Zink over KosmicKrisp (docs/zink-kosmickrisp.md).
# MWXR_OPENGL=wine, also from the game's Launch Options, keeps Wine's OpenGL.
[[ ${_mwxr_settings_app} == 348250 ]] && export MWXR_OPENGL=${MWXR_OPENGL:-zink}
# Windows Vulkan games run on KosmicKrisp through wineopenxr's Vulkan path
# (docs/vulkan-bridge.md), with xrizer with the queue-family fix. (They used a private
# Wine for its FreeType name and NMS's fiber fix; the unified runtime has both.) The
# Talos Principle VR needs "+gfx_strAPI Vulkan" in its Launch Options (otherwise it uses
# D3D11 on DXMT).
# MWXR_VULKAN=off, also from the Launch Options, keeps the ordinary setup.
if [[ ${_mwxr_settings_app} == (552440|275850) && ${MWXR_VULKAN:-} != off ]]; then
  _mwxr_root=${MWXR_FOVEATION_ROOT:-${${(%):-%x}:A:h:h}/build-in-process}
  export MWXR_VULKAN=kosmickrisp
  export MWXR_KK_ROOT=${MWXR_KK_ROOT:-${_mwxr_root}/zink-fov}
  export MWXR_XRIZER_ROOT=${MWXR_XRIZER_ROOT:-${_mwxr_root}/vulkan-xrizer}
  export MWXR_IN_PROCESS_BUILD=${MWXR_IN_PROCESS_BUILD:-${_mwxr_root}/vk-fov-gate}
  # No Man's Sky stops on a "driver does not support wide lines" box; the private
  # KosmicKrisp claims them (drawn one pixel wide).
  [[ ${_mwxr_settings_app} == 275850 ]] && export KK_FAKE_WIDE_LINES=${KK_FAKE_WIDE_LINES:-1}
  # NMS renders single-sampled, at a render scale (about 59%), and reads and writes its
  # eye targets from compute: with MWXR_FOVEATION those are foveated too. Compute
  # compaction (KK_IMPLICIT_COMPACT) stays off: in the headset some compacted passes left
  # everything outside the physical rectangle unwritten (docs/kk-implicit-foveation.md).
  if [[ ${_mwxr_settings_app} == 275850 ]]; then
    export KK_IMPLICIT_SINGLE=${KK_IMPLICIT_SINGLE:-1} KK_IMPLICIT_SCALED=${KK_IMPLICIT_SCALED:-1}
    export KK_IMPLICIT_STORAGE=${KK_IMPLICIT_STORAGE:-1} KK_IMPLICIT_COMPACT=${KK_IMPLICIT_COMPACT:-0}
  fi
  # Talos renders single-sampled (and some effects at half size): with MWXR_FOVEATION,
  # KosmicKrisp foveates those targets too (docs/kk-implicit-foveation.md).
  if [[ ${_mwxr_settings_app} == 552440 ]]; then
    export KK_IMPLICIT_SINGLE=${KK_IMPLICIT_SINGLE:-1} KK_IMPLICIT_HALF=${KK_IMPLICIT_HALF:-1}
  fi
  # The game's desktop (spectator) window presents through KosmicKrisp; in FIFO mode each
  # present can wait for the Mac display's refresh and hold up the VR frame. Immediate
  # presentation never waits (the window may tear). MWXR_SPECTATOR_VSYNC=1 keeps FIFO.
  if [[ ${MWXR_SPECTATOR_VSYNC:-} != 1 ]]; then
    export MESA_VK_WSI_PRESENT_MODE=${MESA_VK_WSI_PRESENT_MODE:-immediate}
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
  export MWXR_IN_PROCESS_WINE=${MWXR_IN_PROCESS_WINE:-${_mwxr_fov}/wine-foveation-unified}
  unset _mwxr_fov
fi
unset _mwxr_settings_app

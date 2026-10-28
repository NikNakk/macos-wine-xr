#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# Source from a rig wrapper, before assigning its default runtime. Explicit
# environment overrides remain authoritative. $1 is the Steam app ID.
# Assetto Corsa's launcher/redistributables are 32-bit; the race engine is Win64.
if [[ ${1:-} == 244210 ]]; then
  export MWXR_IN_PROCESS_WINE=${MWXR_IN_PROCESS_WINE:-$HOME/Code/wine-crossover/runtime-wow64}
  export MWXR_GAME_PREFIXES=${MWXR_GAME_PREFIXES:-$HOME/Windows/prefixes-wow64}
  export GAME_PREFIX=${GAME_PREFIX:-pergame}
fi

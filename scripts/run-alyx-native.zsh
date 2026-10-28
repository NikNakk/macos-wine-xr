#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Half-Life: Alyx's native ARM64 build (~/Code/alyx-native) started from macOS Steam's Play
# button, instead of the Windows build through Wine (docs/steam-play.md, Native
# Half-Life: Alyx). run-windows-game.zsh sends Alyx here when MWXR_ALYX=native, which the
# Steam Play tool takes from the game's Launch Options.
#
# The native build runs from its own prepared game folder
# (~/Code/alyx-native/workspace/alyx-native-game-root), not the copy Steam installed, so
# Steam updates do not reach it and its saves are its own. The launcher already passes
# Alyx's usual -vr -steam ... +map startup; other arguments from Steam go to the engine.
#
# Environment: ALYX_NATIVE_LAUNCHER (default ~/Code/alyx-native/run-alyx),
# MWXR_ALYX_OPTIMIZED=1 (the launcher's --optimized xrizer build),
# ALYX_NATIVE_RUNS (where each run's folder goes, default ~/Code/alyx-native/runs),
# ALYX_NATIVE_TIMEOUT (seconds before the launcher stops the game, default 12 hours).
# Stopping this script (SIGTERM, SIGHUP or SIGINT) stops the game.
set -euo pipefail
launcher=${ALYX_NATIVE_LAUNCHER:-$HOME/Code/alyx-native/run-alyx}
runs=${ALYX_NATIVE_RUNS:-$HOME/Code/alyx-native/runs}
[[ -x ${launcher} ]] || { print -u2 "No native Alyx launcher at ${launcher}"; exit 1; }

# Steam's own arguments for the Windows build; the native launcher adds its equivalents.
# Steam passes them first, then the game's Launch Options.
standard=(-vr -steam -noasserts -nopassiveasserts +map startup)
extra=("$@")
[[ ${(j: :)extra[1,${#standard}]} == ${(j: :)standard} ]] && extra=("${(@)extra[${#standard}+1,-1]}")

output=${runs}/steam-$(date +%Y%m%d-%H%M%S)
launch=(--output "${output}" --timeout "${ALYX_NATIVE_TIMEOUT:-43200}")
[[ ${MWXR_ALYX_OPTIMIZED:-0} == 1 ]] && launch=(--optimized "${launch[@]}")
(( ${#extra} )) && launch+=(-- "${extra[@]}")
print -r -- "Native Half-Life: Alyx: ${launcher} ${launch[*]}"

# The launcher puts the game in its own process group and stops it on SIGINT (or its
# timeout), so a stop from Steam is passed on as SIGINT rather than ending the launcher alone.
"${launcher}" "${launch[@]}" &
pid=$!
trap 'kill -INT ${pid} 2>/dev/null' TERM HUP INT
set +e
code=0
while kill -0 ${pid} 2>/dev/null; do  # a trap interrupts wait; wait again for the real end
  wait ${pid}
  code=$?
done
print -r -- "Native Half-Life: Alyx exited with ${code}; logs in ${output}"
exit ${code}

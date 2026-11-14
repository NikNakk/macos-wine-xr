#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# Where the EXD experiments find the game, prefix and Wine, and put their runs; sourced by
# scripts/run-exd-*.zsh (docs/exd-headset-performance.md, Measuring on Steam's EXD).
#
#   exd_game    the game, used read-only: macOS Steam's install (MWXR_EXD_GAME)
#   exd_prefix  the experiment prefix, an APFS clone of the game's Steam Play prefix with
#               the settings templates the runs switch between (MWXR_EXD_PREFIX); never
#               the prefix Steam plays the game in
#   exd_wine    the Wine runtime: the foveation runtime, DXMT patches 0001-0025 with DXMT's
#               nvapi64/nvngx for the DLSS runs (MWXR_EXD_WINE)
#   exd_runs    where run folders go (MWXR_EXD_RUNS)
# MWXR_EXD_EXPERIMENT_ROOT=build-in-process/exd-ngx (or another private copy) uses that
# copy's game/, prefix/ and wine/ and puts runs beside them, as before 2026-11-14.
# Also sets exd_build to the Steam build ID of the game when it is Steam's.
: ${repo:?source from a script that sets repo}
if [[ -n ${MWXR_EXD_EXPERIMENT_ROOT:-} ]]; then
 exd_game=${MWXR_EXD_EXPERIMENT_ROOT}/game exd_prefix=${MWXR_EXD_EXPERIMENT_ROOT}/prefix
 exd_wine=${MWXR_EXD_EXPERIMENT_ROOT}/wine exd_runs=${MWXR_EXD_EXPERIMENT_ROOT}
else
 exd_game=${MWXR_EXD_GAME:-$HOME/Library/Application Support/Steam/steamapps/common/EXD}
 exd_prefix=${MWXR_EXD_PREFIX:-${repo}/build-in-process/exd-steam/prefix}
 exd_wine=${MWXR_EXD_WINE:-${repo}/build-in-process/wine-foveation-unified}
 exd_runs=${MWXR_EXD_RUNS:-${repo}/build-in-process/exd-steam/runs}
fi
# Runs edit the prefix's settings (and restore them), so it must be an experiment copy.
[[ -f ${exd_game}/EXD.exe && -f ${exd_prefix}/system.reg && -x ${exd_wine}/bin/wine ]] || {
 print -u2 "EXD experiment paths incomplete: game ${exd_game}, prefix ${exd_prefix}, wine ${exd_wine}"; exit 2; }
[[ ${exd_prefix:A} != $HOME/Windows/prefixes/* && ${exd_prefix:A} != $HOME/Windows/prefix ]] || {
 print -u2 "${exd_prefix} is a prefix games are played in; use an experiment copy (build-in-process/exd-steam/prefix)"; exit 2; }
exd_build=''
_exd_manifest="$HOME/Library/Application Support/Steam/steamapps/appmanifest_3962020.acf"
if [[ ${exd_game:A} == "$HOME/Library/Application Support/Steam/steamapps/common/EXD" && -f ${_exd_manifest} ]]; then
 exd_build=$(sed -n 's/^[[:space:]]*"buildid"[[:space:]]*"\([0-9]*\)".*/\1/p' "${_exd_manifest}" | head -1)
fi
unset _exd_manifest
mkdir -p "${exd_runs}"

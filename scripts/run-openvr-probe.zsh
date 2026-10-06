#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Run tools/openvr_probe as the scene application of an already running
# SteamVR (started by run-steamvr-mwxr.zsh). It replaces Home until it exits.
#
#   MWXR_STEAMVR_ROOT  the Wine/DXMT root with Steam and SteamVR in prefix/
#   arguments          passed to openvr_probe.exe, for example --seconds 60 or
#                      --explicit-timing
set -euo pipefail
repo=${0:A:h:h}
: ${MWXR_STEAMVR_ROOT:?Wine 11.10/current-DXMT root with Steam and SteamVR in its prefix}
root=${MWXR_STEAMVR_ROOT}
probe_dir=${MWXR_OPENVR_PROBE:-${repo}/build-in-process/openvr-probe}
steamvr="${root}/prefix/drive_c/Program Files (x86)/Steam/steamapps/common/SteamVR"
[[ -f ${probe_dir}/openvr_probe.exe ]] || { print -u2 "Build the probe first: ${probe_dir}"; exit 1; }
pgrep -f 'vrserver.exe' >/dev/null || { print -u2 "Start SteamVR first (run-steamvr-mwxr.zsh)"; exit 1; }
# Valve's runtime-matched openvr_api.dll, loaded from beside the executable.
cp "${steamvr}/bin/win64/openvr_api.dll" "${probe_dir}/"
log=${root}/openvr-probe-$(date +%Y%m%d-%H%M%S).log
print "Log: ${log}"
export DXMT_LOG_PATH=${DXMT_LOG_PATH:-Z:${root}/dxmt-logs}
WINEPREFIX=${root}/prefix WINEDEBUG=-all "${root}/bin/wine-current-dxmt" "${probe_dir}/openvr_probe.exe" \
  --log "Z:${log//\//\\}" "$@"

#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=${0:A:h:h}
: ${MWXR_IN_PROCESS_WINE:?Private paired runtime with explicit attachment diagnostic}
out=${1:-${repo}/build-in-process/depth-foveation-validation}
mkdir -p "${out}"
out=${out:A}
export DXMT_RATE_MAP_ATTACHMENT_PROBE=1 DXMT_SCENE_TRACE=1 DXMT_SCENE_TRACE_LIMIT=2000
logs=()
for size in 512 1024; do
 for samples in 2 4; do
  logfile=${out}/depth-${samples}x-${size}.log
  "${repo}/scripts/run-dxmt-depth-foveation-probe.zsh" ${samples} ${size} > "${logfile}" 2>&1
  logs+=("${logfile}")
 done
done
python3 "${repo}/tools/analyze_dxmt_depth_probe.py" "${logs[@]}" --csv "${out}/summary.csv" > "${out}/summary.json"
set +e
DXMT_RATE_MAP_ATTACHMENT_PROBE=0 "${repo}/scripts/run-dxmt-depth-foveation-probe.zsh" 2 512 > "${out}/diagnostic-disabled.log" 2>&1
negative_status=$?
set -e
[[ ${negative_status} == 1 ]] && rg -q 'FAIL.*ImportMetalTexture' "${out}/diagnostic-disabled.log"
print "PASS: controls and expected mapped-depth counterexamples checked; summary at ${out}/summary.json"

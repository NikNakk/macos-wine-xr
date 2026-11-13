#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=${0:A:h:h}
: ${MWXR_IN_PROCESS_WINE:?Private paired runtime with explicit attachment diagnostic}
out=${1:-${repo}/build-in-process/resolve-foveation-validation}
mkdir -p "${out}"
out=${out:A}
export DXMT_RATE_MAP_ATTACHMENT_PROBE=1 DXMT_SCENE_TRACE=1 DXMT_SCENE_TRACE_LIMIT=2000
logs=()
for order in normal reverse; do
 extra=()
 [[ ${order} == reverse ]] && extra=(--reverse)
 for samples in 2 4; do
  for rate in 0.25 0.5 0.75 1; do
   logfile=${out}/resolve-${samples}x-${rate}-${order}.log
   "${repo}/scripts/run-dxmt-depth-foveation-probe.zsh" ${samples} 1024 --resolve-first ${rate} "${extra[@]}" > "${logfile}" 2>&1
   logs+=("${logfile}")
  done
 done
done
python3 "${repo}/tools/analyze_dxmt_resolve_probe.py" "${logs[@]}" --csv "${out}/summary.csv" > "${out}/summary.json"
print "PASS: resolve-first rate sweep, normal/reverse order, full-rate identity and exact-depth controls."

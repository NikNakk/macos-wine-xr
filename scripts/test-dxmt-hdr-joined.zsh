#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=${0:A:h:h}
: ${MWXR_IN_PROCESS_WINE:?Private paired DXMT runtime and rebuilt depth probe}
out=${1:-${repo}/build-in-process/hdr-joined-validation}
mkdir -p "${out}"
export DXMT_TBDR_AUDIT=0 DXMT_SCENE_TRACE=0 DXMT_NGX_TRACE=0
logs=()
for samples in 2 4; do
 for rate in 1 0.5 0.75; do
  for trial in 1 2 3; do
   [[ ${rate} == 1 && ${trial} == 3 ]] && continue
   extra=();[[ ${trial} == 2 ]] && extra=(--reverse)
   log=${out}/hdr-joined-${samples}x-${rate}-trial${trial}.log
   "${repo}/scripts/run-dxmt-depth-foveation-probe.zsh" ${samples} 1024 --joined-hdr ${rate} "${extra[@]}" > "${log}" 2>&1
   logs+=("${log}")
  done
 done
done
python3 "${repo}/tools/analyze_dxmt_hdr_joined.py" "${logs[@]}" > "${out}/summary.json"
print 'PASS: full HDR/stencil sequences, original/coverage replay equivalence, exact depth/stencil and consumer controls.'

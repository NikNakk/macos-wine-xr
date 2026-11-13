#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=${0:A:h:h}
: ${MWXR_IN_PROCESS_WINE:?Private paired runtime with the combined-stencil diagnostic patch}
out=${1:-${repo}/build-in-process/hdr-stencil-validation}
mkdir -p "${out}"
export DXMT_TBDR_AUDIT=1 DXMT_TBDR_AUDIT_DETAIL=0 DXMT_TBDR_AUDIT_START_FRAME=0 DXMT_TBDR_AUDIT_FRAME_COUNT=0
logs=()
for samples in 2 4; do
 for rate in 1 0.5 0.75; do
  log=${out}/hdr-${samples}x-${rate}.log
  "${repo}/scripts/run-dxmt-depth-foveation-probe.zsh" ${samples} 512 --hdr-stencil ${rate} > "${log}" 2>&1
  logs+=("${log}")
 done
done
python3 "${repo}/tools/analyze_dxmt_hdr_stencil.py" "${logs[@]}" > "${out}/summary.json"
print 'PASS: HDR, coverage and stencil controls; naive replay failures reproduced and stencil-only replay restores the consumer.'

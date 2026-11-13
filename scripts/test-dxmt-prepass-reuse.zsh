#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=${0:A:h:h}
: ${MWXR_IN_PROCESS_WINE:?Private paired runtime containing the explicit depth-only diagnostic patch}
out=${1:-${repo}/build-in-process/prepass-reuse-validation}
mkdir -p "${out}"
export DXMT_TBDR_AUDIT=1 DXMT_TBDR_AUDIT_DETAIL=0 DXMT_TBDR_AUDIT_START_FRAME=0 DXMT_TBDR_AUDIT_FRAME_COUNT=0
logs=()
for samples in 2 4; do
 for rate in 1 0.5 0.75; do
  log=${out}/prepass-${samples}x-${rate}-1024.log
  "${repo}/scripts/run-dxmt-depth-foveation-probe.zsh" ${samples} 1024 --prepass-reuse ${rate} > "${log}" 2>&1
  logs+=("${log}")
 done
done
python3 "${repo}/tools/analyze_dxmt_prepass_reuse.py" "${logs[@]}" > "${out}/summary.json"
print 'PASS: identity/mapped-prepass/preservation controls; reduced-rate depth reuse produces visibility counterexamples.'

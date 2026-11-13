#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# Requires the rebuilt warp-read probe and a private paired runtime/prefix.
set -euo pipefail
repo=${0:A:h:h}
: ${MWXR_IN_PROCESS_WINE:?Private paired runtime}
out=${1:-${repo}/build-in-process/selective-read-regression}
mkdir -p "${out}"
export DXMT_RATE_MAP_POSITION=1 DXMT_RATE_MAP_READ=1 DXMT_TBDR_AUDIT=1
unset DXMT_RATE_MAP_DIAGNOSTIC_SHADER_MASK DXMT_STAY_WARPED
run=${repo}/scripts/run-dxmt-warp-read-probe.zsh
for enabled in 0 1; do
 export DXMT_RATE_MAP_SELECTIVE_READ=${enabled}
 "${run}" > "${out}/mapped-${enabled}.log" 2>&1
 "${run}" --unfoveated > "${out}/ordinary-${enabled}.log" 2>&1
 "${run}" --late-read > "${out}/late-${enabled}.log" 2>&1
 "${run}" --unfoveated --late-read --split-pass > "${out}/merge-${enabled}.log" 2>&1
 if DXMT_RATE_MAP_READ=0 "${run}" --late-read > "${out}/negative-${enabled}.log" 2>&1; then
  print -u2 "Untranslated control passed unexpectedly"; exit 1
 fi
done
python3 - "${out}" <<'PY'
from pathlib import Path
import json,re,sys
out=Path(sys.argv[1]);results=[]
for enabled in [0,1]:
 for case,ordinary,capable,mapped in [('mapped',1,3,2),('ordinary',4,0,0),('late',2,3,2),('merge',6,0,0)]:
  text=(out/f'{case}-{enabled}.log').read_text()
  assert 'PASS mode=' in text and 'Device error at frame' not in text,(case,enabled)
  rows=[dict(re.findall(r'(\w+)=(\S+)',line)) for line in text.splitlines() if 'TBDR cb ' in line]
  counts={k:sum(int(row.get(k,0)) for row in rows) for k in ['read_ordinary','read_capable','mapped_render','render_before','render_after']}
  assert counts['mapped_render']==mapped,(case,enabled,counts)
  assert counts['read_ordinary']==(ordinary if enabled else 0),(case,enabled,counts)
  assert counts['read_capable']==(capable if enabled else 0),(case,enabled,counts)
  if case=='merge':assert counts['render_before']==6 and counts['render_after']==5,counts
  results.append(dict(case=case,enabled=bool(enabled),counters=counts,pixels='PASS'))
 text=(out/f'negative-{enabled}.log').read_text()
 assert 'FAIL mode=1' in text and 'WRONG' in text,enabled
(out/'summary.json').write_text(json.dumps(results,indent=2)+'\n')
print('PASS: selection off/on pixels; late mapped binding; merged passes; untranslated negative controls.')
PY

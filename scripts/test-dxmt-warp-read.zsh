#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# Stay-warped read translation: mapped chain, untranslated negative control,
# and an unmapped identity control. Requires the private paired runtime.
set -euo pipefail
repo=${0:A:h:h}
: ${MWXR_IN_PROCESS_WINE:?Private paired PE/Unix runtime}
out=${1:-${repo}/build-in-process/warp-read-validation}
mkdir -p "${out}"
export DXMT_RATE_MAP_POSITION=1 DXMT_SCENE_TRACE=1 DXMT_SCENE_TRACE_LIMIT=1000
run="${repo}/scripts/run-dxmt-warp-read-probe.zsh"
DXMT_RATE_MAP_READ=1 "${run}" > "${out}/mapped.log" 2>&1
# Expected failure: the same mapped passes without read translation.
if DXMT_RATE_MAP_READ=0 "${run}" > "${out}/untranslated.log" 2>&1; then
 print -u2 "Negative control passed unexpectedly"; exit 1
fi
DXMT_RATE_MAP_READ=1 "${run}" --unfoveated > "${out}/unfoveated.log" 2>&1
python3 - "${out}" <<'PY'
import json,re,sys
from pathlib import Path
root=Path(sys.argv[1]);summary={}
for name,mapped,translated,passed in [('mapped',2,3,True),('untranslated',2,0,False),('unfoveated',0,0,True)]:
 text=(root/(name+'.log')).read_text()
 rows=[dict(re.findall(r'(\w+)=(\S+)',line)) for line in re.findall(r'WARP_READ[^\n]+',text)]
 assert len(rows)==8,(name,len(rows))
 maps=sum(int(v,16)!=0 for v in re.findall(r'SCENE render[^\n]* map=([\da-f]+)',text))
 reads=len(re.findall(r'SCENE rate_map_read [^\n]*translated=1',text))
 assert maps==mapped and reads==translated,(name,maps,reads)
 assert bool(re.search(r'^PASS ',text,re.M))==passed,(name,'verdict')
 for row in rows:
  for key in ('centre_max_px','outer_max_px','mean_px'):row[key]=float(row[key])
  for key in ('over_8px','checked'):row[key]=int(row[key])
 if name=='untranslated':assert max(r['outer_max_px'] for r in rows)>100
 if name=='unfoveated':assert max(r['centre_max_px'] for r in rows)<0.05
 summary[name]=dict(mapped_passes=maps,translated_reads=reads,rows=rows)
(root/'summary.json').write_text(json.dumps(summary,indent=1)+'\n')
m=summary['mapped']['rows'];u=summary['untranslated']['rows']
print('PASS: mapped chain within tolerance; untranslated control fails; unmapped identity exact.')
print('Mapped worst error px: centre %.3f, periphery %.3f. Untranslated worst: %.0f px.'%(
 max(r['centre_max_px'] for r in m),max(r['outer_max_px'] for r in m),max(r['outer_max_px'] for r in u)))
PY

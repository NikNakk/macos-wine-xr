#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# Stay-warped MSAA/stencil chain against the ordinary baseline: every scene pass
# is rasterised once with the rate map and read through DXMT read translation.
set -euo pipefail
repo=${0:A:h:h}
: ${MWXR_IN_PROCESS_WINE:?Private paired runtime with patch 0015}
out=${1:-${repo}/build-in-process/stay-warped-validation}
mkdir -p "${out}"
export DXMT_TBDR_AUDIT=0 DXMT_SCENE_TRACE=0 DXMT_NGX_TRACE=0 DXMT_RATE_MAP_READ=1
run="${repo}/scripts/run-dxmt-depth-foveation-probe.zsh"
# Structure: six mapped passes and six translated reads per stay-warped sequence
# (MSAA colour, snapshot, MSAA depth, resolved colour + resolved depth, final image).
DXMT_SCENE_TRACE=1 DXMT_SCENE_TRACE_LIMIT=600 "${run}" 2 1024 --msaa-source 0.5 --stay-warped > "${out}/trace.log" 2>&1
python3 - "${out}/trace.log" <<'PY'
import re,sys
text=open(sys.argv[1]).read()
rows=re.findall(r'SCENE render encoder=(\d+)[^\n]* map=([\da-f]+)',text)
reads=[int(e) for e in re.findall(r'SCENE rate_map_read encoder=(\d+) stage=\d+ slot=\d+ translated=1',text)]
runs=[];current=[]
for encoder,m in rows:
 if int(m,16):current.append(int(encoder))
 elif current:runs.append(current);current=[]
full=[r for r in runs if len(r)==6]
assert full,'no six-pass mapped sequence in trace'
first=full[0];inside=[e for e in reads if first[0]<=e<=first[-1]+5]
assert len(inside)==6,inside
PY
# Expected failure: identical mapped passes with read translation disabled.
if DXMT_RATE_MAP_READ=0 "${run}" 2 1024 --msaa-source 0.5 --stay-warped > "${out}/untranslated.log" 2>&1; then
 print -u2 "Untranslated control passed unexpectedly"; exit 1
fi
grep -q 'STAY_WARPED_CONTROL' "${out}/untranslated.log"
logs=()
for samples in 2 4; do
 for rate in 1 0.5 0.75; do
  for trial in 1 2 3; do
   [[ ${rate} == 1 && ${trial} == 3 ]] && continue
   extra=();[[ ${trial} == 2 ]] && extra=(--reverse)
   log=${out}/stay-${samples}x-${rate}-trial${trial}.log
   "${repo}/scripts/run-dxmt-depth-foveation-probe.zsh" ${samples} 1024 --msaa-source ${rate} --stay-warped "${extra[@]}" > "${log}" 2>&1
   logs+=("${log}")
  done
 done
done
python3 - "${out}" "${logs[@]}" <<'PY'
import json,re,statistics,sys
from pathlib import Path
rows=[]
for name in sys.argv[2:]:
 text=Path(name).read_text()
 final=re.search(r'^STAY_WARPED size[^\n]+',text,re.M);assert final,name
 f=dict(re.findall(r'(\w+)=([\d.]+)',final[0]))
 controls=[dict(re.findall(r'(\w+)=([\d.]+)',l)) for l in re.findall(r'STAY_WARPED_CONTROL[^\n]+',text)]
 assert len(controls)==2 and all(int(c['center_bad'])==0 for c in controls),name
 gpu={0:{},1:{}}
 for m,r,w,g in re.findall(r'HDR_JOINED_SAMPLE mode=(\d) round=(\d+) warmup=(\d) gpu_ms=([\d.]+)',text):
  if w=='0':gpu[int(m)][int(r)]=float(g)
 assert len(gpu[0])==len(gpu[1])==int(f['measured']),name
 paired=statistics.median(100*(1-gpu[1][r]/gpu[0][r]) for r in gpu[0])
 rows.append(dict(samples=int(f['samples']),rate=float(f['rate']),reverse=int(f['reverse']),
  baseline_gpu_ms=float(f['baseline_gpu_ms']),stay_warped_gpu_ms=float(f['stay_warped_gpu_ms']),
  paired_reduction_percent=round(paired,2),final_bad=int(controls[1]['final_bad']),mae=float(controls[1]['mae'])))
 if float(f['rate'])==1:assert rows[-1]['final_bad']==0,name
Path(sys.argv[1],'summary.json').write_text(json.dumps(rows,indent=1)+'\n')
for r in rows:print(r)
print('PASS: stay-warped chain; centre exact within tolerance in every case.')
PY

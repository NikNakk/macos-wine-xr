#!/bin/zsh
# Copyright 2026 Nick Kennedy; SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=${0:A:h:h}
: ${MWXR_IN_PROCESS_WINE:?Private runtime with bounded pass-capture patch}
out=${1:-${repo}/build-in-process/pass-replay-validation}
mkdir -p "${out}"
export MWXR_REPLAY_PROBE_EXE=${repo}/build-in-process/replay-probe/dxmt_pass_replay_probe.exe
export DXMT_REPLAY_CAPTURE=1 DXMT_BASE_FOVEATION=0
for cache in 0 1; do
 for samples in 1 2; do
  for format in rgba8 hdr; do
   extra=();[[ ${format} == hdr ]] && extra=(--hdr)
   DXMT_REPLAY_CACHE_INPUTS=${cache} "${repo}/scripts/run-dxmt-replay-probe.zsh" ${samples} "${extra[@]}" > "${out}/pass-${cache}-${samples}x-${format}.log" 2>&1
  done
 done
done
# Separate processes and alternating cache order; no concurrent GPU tests.
for trial in 1 2 3; do
 order=(0 1);[[ ${trial} == 2 ]] && order=(1 0)
 for cache in "${order[@]}"; do
  DXMT_REPLAY_CACHE_INPUTS=${cache} "${repo}/scripts/run-dxmt-replay-probe.zsh" 2 --hdr --timing > "${out}/timing-${cache}-trial${trial}.log" 2>&1
 done
done
python3 - "${out}" <<'PY'
import json,re,sys
from pathlib import Path
root=Path(sys.argv[1]);rows=[];timings=[]
for cache in [0,1]:
 for n in [1,2]:
  for fmt in ['rgba8','hdr']:
   p=root/f'pass-{cache}-{n}x-{fmt}.log';text=p.read_text();match=re.search(r'PASS_REPLAY[^\n]+PASS',text);assert match,(p,text[-1000:])
   row={k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',match[0])}
   assert row['samples']==n and row['cache']==cache and row['hdr']==int(fmt=='hdr')
   assert row['draws']==4 and row['indexed_draws']==2 and row['input_versions']==4
   assert row['sample_comparisons']==128*128*n*3 and row['prepass_samples']>100
   assert row['negative_cases']==13 and row['abandoned_scope']==1
   for flag in ['unbound_slots','seed_reset','caller_release','restored_draw','collapsed_negative']:assert row[flag]==1
   if n==2:assert row['distinct_sample_pixels']>0
   if fmt=='hdr':assert row['above_one_samples']>100
   rows.append(row)
for n in [1,2]:
 for hdr in [0,1]:assert len({r['nominal_bytes'] for r in rows if r['samples']==n and r['hdr']==hdr})==1
for cache in [0,1]:
 for trial in [1,2,3]:
  p=root/f'timing-{cache}-trial{trial}.log';text=p.read_text();match=re.search(r'PASS_TIMING[^\n]+PASS',text);assert match,(p,text[-1000:])
  row={k:float(v) for k,v in re.findall(r'(\w+)=([\d.-]+)',match[0])};row['trial']=trial
  assert row['cache']==cache and row['correctness']==1 and row['measured_pairs']==17
  for metric in ['baseline_gpu_ms','captured_original_gpu_ms','cached_replay_gpu_ms','baseline_cpu_ms','capture_cpu_ms']:assert row[metric]>0
  timings.append(row)
assert max(t['nominal_bytes'] for t in timings if t['cache']==1)<min(t['nominal_bytes'] for t in timings if t['cache']==0)
(root/'summary.json').write_text(json.dumps(dict(cases=rows,timings=timings),indent=2)+'\n')
print('PASS: 8 conventional pass identity cases, input-version invalidation, indexed/coverage/depth/stencil controls, 13 rejection controls per case and abandoned-scope lifetime; 6 correctness-checked timing trials.')
PY

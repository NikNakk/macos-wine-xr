#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# Standalone native experiment; does not launch Wine, EXD or a headset runtime.
set -euo pipefail
repo=${0:A:h:h}
out=${1:-${repo}/build-in-process/metal-depth-resolve-validation}
mkdir -p "${out}"
clang -O2 -Wall -Wextra -fno-objc-arc "${repo}/tests/native/metal_depth_resolve_probe.m" \
 -framework Foundation -framework Metal -o "${out}/probe"
for validation in 1 0; do
 for samples in 2 4; do
  for trial in 1 2 3; do
   MTL_DEBUG_LAYER=${validation} "${out}/probe" ${samples} > "${out}/msaa${samples}-validation${validation}-trial${trial}.log" 2>&1
  done
 done
done
python3 - "${out}" <<'PY'
from pathlib import Path
import re,json,statistics,sys
root=Path(sys.argv[1]);rows=[]
for path in sorted(root.glob('msaa*-validation*-trial*.log')):
 validation=int(re.search(r'-validation(\d)',path.name)[1]);trial=int(re.search(r'-trial(\d)',path.name)[1])
 matches=re.findall(r'DEPTH_RESOLVE ([^\n]+)',path.read_text());assert len(matches)==4,path
 for line in matches:
  row=dict(re.findall(r'(\w+)=([^\s]+)',line));row.update(validation=validation,trial=trial)
  assert row['pass']=='1' and row['source_wrong']=='0' and row['stencil_wrong']=='0',row
  assert (int(row['depth_wrong'])==0)==(row['control']=='0'),row
  rows.append(row)
assert len(rows)==48,len(rows)
summary=[]
for samples in ['2','4']:
 cases={mode:[float(r['gpu_chain_ms']) for r in rows if r['validation']==0 and r['samples']==samples and r['mode']==mode] for mode in ['shader','fused-max','separate-max']}
 paired={mode:[a-b for a,b in zip(cases['shader'],cases[mode])] for mode in ['fused-max','separate-max']}
 summary.append(dict(samples=int(samples),case_trial_medians_ms=cases,paired_saving_ms=paired,median_paired_saving_ms={k:statistics.median(v) for k,v in paired.items()}))
(root/'summary.json').write_text(json.dumps(dict(rows=rows,performance=summary,limitations=['Standalone native Metal chain, not EXD or DXMT end-to-end timings.','GPU command-buffer duration divided by eight seed+resolve repetitions; CPU waits/readback excluded.','Cases rotate per batch; 10 warmup batches and 15 measured batches per case, three trials.','No source/store elimination: later MSAA use remains valid.']),indent=2)+'\n')
print('PASS: all depth/stencil/source checks at 2x/4x; sample0 negative controls detected.')
print(json.dumps(summary,indent=2))
PY

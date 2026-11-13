#!/bin/zsh
# Copyright 2026 Nick Kennedy; SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=${0:A:h:h}
: ${MWXR_IN_PROCESS_WINE:?Private runtime containing the replay ownership patch}
out=${1:-${repo}/build-in-process/replay-validation}
mkdir -p "${out}"
export DXMT_BASE_FOVEATION=0 DXMT_REPLAY_CAPTURE=1
for samples in 1 2; do
 for format in rgba8 hdr; do
  extra=();[[ ${format} == hdr ]] && extra=(--hdr)
  "${repo}/scripts/run-dxmt-replay-probe.zsh" ${samples} "${extra[@]}" > "${out}/${samples}x-${format}.log" 2>&1
 done
done
DXMT_REPLAY_CAPTURE=0 "${repo}/scripts/run-dxmt-replay-probe.zsh" --disabled > "${out}/disabled.log" 2>&1
python3 - "${out}" <<'PY'
import json,re,sys
from pathlib import Path
root=Path(sys.argv[1]);rows=[]
for samples in [1,2]:
 for fmt in ['rgba8','hdr']:
  text=(root/f'{samples}x-{fmt}.log').read_text()
  line=re.search(r'REPLAY[^\n]+PASS',text);assert line,text[-1000:]
  row={k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',line[0])}
  assert row['samples']==samples and row['hdr']==int(fmt=='hdr')
  assert row['sample_comparisons']==128*128*samples*4 and row['negative_cases']==4
  assert row['shaded']+row['preserved']==128*128*samples
  assert row['nominal_bytes']<8*1024*1024
  if samples==2:assert row['distinct_sample_pixels']>0
  if fmt=='hdr':assert row['above_one_samples']>1000
  for field in ['input_mutation','queued_mutation','caller_release','seed_reset','context_restore','restored_draw','ordinary_output_unchanged']:
   assert row[field]==1
  rows.append(row)
assert 'REPLAY disabled=1 normal_draw=1' in (root/'disabled.log').read_text()
(root/'summary.json').write_text(json.dumps(dict(cases=rows,disabled_normal_draw=True),indent=2)+'\n')
print('PASS: owned replay, four cases, per-sample colour/depth/stencil identity, mutations/releases, four rejection controls and disabled ordinary rendering.')
PY

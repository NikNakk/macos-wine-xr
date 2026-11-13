#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# Requires a private patched runtime and the built ordinary D3D11 probe.
set -euo pipefail
repo=/Users/nickkennedy/Code/macos-wine-xr
: ${MWXR_IN_PROCESS_WINE:?Private paired PE/Unix runtime}
out=${1:-${repo}/build-in-process/base-foveation-validation}
mkdir -p "${out}"
export DXMT_RATE_MAP_POSITION=1 DXMT_SCENE_TRACE=1 DXMT_SCENE_TRACE_LIMIT=1000
DXMT_BASE_FOVEATION=0 "/tmp/run-base-selective-read.zsh" > "${out}/baseline.log" 2>&1
export DXMT_BASE_FOVEATION_VS=$(sed -n 's/^DXMT_BASE_FOVEATION_VS=//p' "${out}/baseline.log" | tr -d '\r')
export DXMT_BASE_FOVEATION_PS=$(sed -n 's/^DXMT_BASE_FOVEATION_PS=//p' "${out}/baseline.log" | tr -d '\r')
[[ ${#DXMT_BASE_FOVEATION_VS} == 40 && ${#DXMT_BASE_FOVEATION_PS} == 40 ]]
export DXMT_BASE_FOVEATION=1 DXMT_BASE_FOVEATION_SIZE=2048x2048
"/tmp/run-base-selective-read.zsh" > "${out}/enabled.log" 2>&1
for mode in two-draws no-uav depth msaa no-clear; do
 "/tmp/run-base-selective-read.zsh" --${mode} > "${out}/${mode}.log" 2>&1
done
DXMT_BASE_FOVEATION_PS=0000000000000000000000000000000000000000 \
 "/tmp/run-base-selective-read.zsh" > "${out}/unselected.log" 2>&1
DXMT_RATE_MAP_POSITION=0 "/tmp/run-base-selective-read.zsh" > "${out}/position-disabled.log" 2>&1
DXMT_BASE_FOVEATION=0 DXMT_RATE_MAP_POSITION=0 \
 "/tmp/run-base-selective-read.zsh" > "${out}/default.log" 2>&1
python3 - "${out}" <<'PY'
import csv,json,re,sys
from pathlib import Path
root=Path(sys.argv[1]);rows=[]
for name in ['baseline','enabled','two-draws','no-uav','depth','msaa','no-clear','unselected','position-disabled','default']:
 text=(root/(name+'.log')).read_text();line=re.search(r'ORDINARY_D3D11[^\n]+',text)
 if not line:raise SystemExit('Missing result: '+name)
 fields=dict(re.findall(r'(\w+)=([\d.]+)',line[0]))
 mapped=sum(int(value,16)!=0 for value in re.findall(r'SCENE render[^\n]* map=([\da-f]+)',text))
 unwarps=len(re.findall(r'SCENE base_foveation_unwarp ',text))
 assert int(fields['wrong'])==0 and int(fields['pixels'])==4194304,(name,fields)
 assert mapped==(24 if name=='enabled' else 0),(name,mapped)
 assert unwarps==(24 if name=='enabled' else 0),(name,unwarps)
 rows.append(dict(case=name,gpu_ms=float(fields['scene_and_reconstruction_gpu_ms']),
                  cpu_record_ms=float(fields['cpu_record_ms']),mapped_passes=mapped,unwarps=unwarps,wrong=0))
(root/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
with (root/'summary.csv').open('w') as f:
 writer=csv.DictWriter(f,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
print('PASS: automatic pass/reconstruction plus 8 fallback/default controls; 4,194,304 pixels checked per run.')
print('Synthetic GPU baseline / foveated including reconstruction: %.6f / %.6f ms'%(rows[0]['gpu_ms'],rows[1]['gpu_ms']))
PY

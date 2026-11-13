#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Per-resource read specialisation on the stay-warped MSAA/HDR/stencil chain:
six cases (2x/4x MSAA, rates 1, 0.5, 0.75) must keep the centre exact and prune ordinary slots.

Requires the private runtime and prefix under build-in-process/exd-warp with DXMT
patches 0001-0020, and the rebuilt probes. Usage: scripts/test-dxmt-resource-read-msaa.py [OUTPUT_DIRECTORY]
"""
from pathlib import Path
import os,subprocess,shutil,json,re,sys
r=Path(__file__).resolve().parent.parent;b=r/'build-in-process';out=Path(sys.argv[1]) if len(sys.argv)>1 else b/'resource-read-msaa-validation';out.mkdir(parents=True,exist_ok=True)
root=b/'depth-foveation-probe';prefix=b/'exd-warp/prefix';wine=b/'exd-warp/wine';shutil.copy2(root/'x86_64-windows/wine_depth_foveation_probe.dll',prefix/'drive_c/windows/system32/wine_depth_foveation_probe.dll')
env={k:v for k,v in os.environ.items() if not k.startswith(('DXMT_','MWXR_','MTL_'))};env.update(WINEPREFIX=str(prefix),WINEARCH='win64',WINEDEBUG='-all',WINESERVER=str(wine/'bin/wineserver'),WINEDLLPATH=str(root),DYLD_FALLBACK_LIBRARY_PATH=str(wine/'lib/wine/x86_64-unix')+':'+str(wine/'lib'),MTL_DEBUG_LAYER='1',DXMT_RATE_MAP_POSITION='1',DXMT_RATE_MAP_READ='1',DXMT_RATE_MAP_ATTACHMENT_PROBE='1',DXMT_RATE_MAP_RESOURCE_READ='1',DXMT_TBDR_AUDIT='1')
results=[]
for samples in [2,4]:
 for rate in ['1','0.5','0.75']:
  name=f'msaa-{samples}-rate-{rate}'
  with (out/(name+'.log')).open('w') as f:subprocess.run([str(wine/'bin/wine'),str(root/'dxmt_depth_foveation_probe.exe'),str(samples),'1024','--msaa-source',rate,'--stay-warped'],cwd=root,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=60,check=True)
  s=(out/(name+'.log')).read_text();controls=[dict(re.findall(r'(\w+)=([\d.]+)',line)) for line in s.splitlines() if line.startswith('STAY_WARPED_CONTROL')];assert len(controls)==2 and all(int(x['center_bad'])==0 for x in controls),name
  if rate=='1':assert int(controls[1]['final_bad'])==0,name
  rows=[dict(re.findall(r'(\w+)=(\S+)',line)) for line in s.splitlines() if 'TBDR cb ' in line]
  counts={k:sum(int(x.get(k,0)) for x in rows) for k in ['read_mask_draws','read_mask_kept','read_mask_pruned','read_mask_mixed','read_mask_fallback','mapped_render']};assert counts['read_mask_draws']>0 and counts['read_mask_kept']>0 and counts['read_mask_pruned']>0 and counts['read_mask_fallback']==0,name
  results.append(dict(case=name,controls=controls,counters=counts));print('PASS '+name+' '+json.dumps(counts),flush=True)
(out/'summary.json').write_text(json.dumps(results,indent=2)+'\n')

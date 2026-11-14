#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Per-resource read specialisation: pixel fixture matrix with the option off and on,
including late, reversed, mixed and swapped bindings, a merged pass and a negative control.

Requires the foveation runtime (build-in-process/wine-foveation-unified, DXMT patches
0001-0025), the test prefix build-in-process/prefix-warp, and the rebuilt probes. Usage: scripts/test-dxmt-resource-read.py [OUTPUT_DIRECTORY]
"""
from pathlib import Path
import os,subprocess,json,re,sys
r=Path(__file__).resolve().parent.parent;b=r/'build-in-process';out=Path(sys.argv[1]) if len(sys.argv)>1 else b/'resource-read-validation';out.mkdir(parents=True,exist_ok=True)
cache=b/'resource-read-fixture-cache';cache.mkdir(exist_ok=True)
cases=[('ordinary',['--unfoveated']),('mapped',[]),('late',['--late-read']),('reverse',['--late-read','--reverse-read']),('mixed',['--mixed-reads']),('swap',['--mixed-swap']),('merge',['--unfoveated','--late-read','--split-pass'])]
base={k:v for k,v in os.environ.items() if not k.startswith(('DXMT_','MWXR_','MTL_'))}
base.update(MWXR_IN_PROCESS_WINE=str(b/'wine-foveation-unified'),MWXR_WARP_READ_PREFIX=str(b/'prefix-warp'),DXMT_RATE_MAP_POSITION='1',DXMT_RATE_MAP_READ='1',DXMT_RATE_MAP_SELECTIVE_READ='0',DXMT_TBDR_AUDIT='1',DXMT_SHADER_CACHE_PATH=str(cache))
results=[]
for enabled in ['0','1']:
 env={**base,'DXMT_RATE_MAP_RESOURCE_READ':enabled}
 for name,args in cases:
  log=out/(name+'-'+enabled+'.log')
  with log.open('w') as f:
   subprocess.run([str(r/'scripts/run-dxmt-warp-read-probe.zsh')]+args,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=40,check=True)
  text=log.read_text();assert 'PASS mode=' in text and 'Device error at frame' not in text
  rows=[dict(re.findall(r'(\w+)=(\S+)',s)) for s in text.splitlines() if 'TBDR cb ' in s]
  counts={k:sum(int(x.get(k,0)) for x in rows) for k in ['read_mask_draws','read_mask_kept','read_mask_pruned','read_mask_mixed','read_mask_fallback','render_before','render_after','mapped_render']}
  if enabled=='1':
   assert counts['read_mask_draws']>0 and counts['read_mask_fallback']==0,(name,counts)
   if name in ['mixed','swap']:assert counts['read_mask_mixed']==(1 if name=='mixed' else 2),(name,counts)
   if name in ['late','reverse']:assert counts['read_mask_kept']>0 and counts['read_mask_pruned']>0,(name,counts)
  if name=='merge':assert counts['render_before']==6 and counts['render_after']==5,(name,counts)
  results.append(dict(case=name,enabled=int(enabled),counters=counts));print('PASS '+name+' '+enabled+' '+json.dumps(counts),flush=True)
 log=out/('negative-'+enabled+'.log')
 with log.open('w') as f:
  p=subprocess.run([str(r/'scripts/run-dxmt-warp-read-probe.zsh'),'--mixed-swap'],env={**env,'DXMT_RATE_MAP_READ':'0'},stdout=f,stderr=subprocess.STDOUT,timeout=40)
 assert p.returncode!=0 and 'FAIL mode=1' in log.read_text()
 print('PASS negative '+enabled,flush=True)
(out/'summary.json').write_text(json.dumps(results,indent=2)+'\n')

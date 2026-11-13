#!/usr/bin/env python3
"""Conservative DXBC disassembly facts; absence of flags is not a replay safety proof."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re

def analyze(text):
    text=text.replace('\0','')
    lines=[re.sub(r'^\s*\d+:\s*','',s).strip().lower() for s in text.splitlines()]
    ops=[s.split()[0] for s in lines if s and not s.startswith('//')]
    counts=Counter(ops)
    depth=any(re.search(r'^dcl_output\S*\s+odepth',s) for s in lines)
    coverage=any(re.search(r'^dcl_output\S*\s+(?:omask|ocoverage)',s) for s in lines)
    uav=any(s.startswith('dcl_uav') for s in lines)
    discard=sum(v for k,v in counts.items() if k.startswith('discard'))
    writes={k:v for k,v in counts.items() if k.startswith(('store_','atomic_','imm_atomic_'))}
    return dict(profile=next((s for s in lines if re.match(r'^[vphdgc]s_\d',s)),'unknown'),
                instruction_count=sum(v for k,v in counts.items() if not k.startswith(('dcl_','vs_','ps_','gs_','hs_','ds_','cs_'))),
                discard_instructions=discard,depth_output=depth,coverage_output=coverage,uav_declared=uav,
                write_instructions=writes,texture_instructions={k:v for k,v in counts.items() if k.startswith(('sample','ld'))},
                unresolved_replay_conditions=['Resource/constant versioning, draw/query/stream-output effects and primitive state must also be preserved.'])

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path);a=p.parse_args()
    rows={}
    for path in sorted(a.directory.glob('*.dxbc.asm')):
        sha=path.name.split('.')[0]
        if re.fullmatch('[a-f0-9]{40}',sha):rows[sha]=analyze(path.read_text(errors='replace'))
    print(json.dumps(rows,indent=2))
if __name__=='__main__':main()

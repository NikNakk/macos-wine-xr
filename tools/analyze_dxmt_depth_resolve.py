#!/usr/bin/env python3
"""Find depth-resolve candidates by shader/state and allocation provenance; never authorises replacement."""
import argparse
from collections import defaultdict,Counter
import json
from pathlib import Path
import re

def fields(text):
    return dict(re.findall(r'(\w+)=([^\s]+)',text))

def analyze(lines):
    pipelines=defaultdict(list);srvs=defaultdict(list);attachments=[];renders={}
    for line in lines:
        m=re.search(r'TBDR (pipeline|srv|attachment|render) (.*)',line)
        if not m:continue
        kind,tail=m.groups();row=fields(tail);encoder=int(row['encoder']);row['encoder']=encoder;row['frame']=int(row['frame'])
        if kind=='pipeline':pipelines[encoder].append(row)
        elif kind=='srv':srvs[encoder].append(row)
        elif kind=='attachment':attachments.append(row)
        else:renders[encoder]=dict(row,label=tail.split('label=',1)[-1])
    candidates=[]
    for encoder,rows in sorted(pipelines.items()):
        unique={(r['vs'],r['ps']) for r in rows}
        if len(unique)!=1:continue
        p=rows[-1]
        if not all(p.get(k)==v for k,v in dict(colors='0',samples='1',depth_enable='1',depth_write='1',depth_func='8',stencil_enable='0').items()):continue
        label=renders.get(encoder,{}).get('label','')
        if 'draws=1 d1 ' not in label:continue
        dst=[r for r in attachments if r['encoder']==encoder and r['role'] in {'depth','depth_stencil'} and r['samples']=='1']
        src=[r for r in srvs[encoder] if r['stage']=='1' and r['slot']=='0']
        if len(dst)!=1 or not src:continue
        dst=dst[0];src=src[-1]
        # A depth SRV may be a distinct Metal texture view; join the resource allocation.
        writers=[r for r in attachments if r['frame']==p['frame'] and r['encoder']<encoder and r['resource']==src['resource'] and r['allocation']==src['allocation'] and int(r['samples'])>1]
        if not writers:continue
        writer=max(writers,key=lambda r:r['encoder'])
        later=[r for r in attachments if r['frame']==p['frame'] and r['encoder']>encoder and r['resource']==src['resource'] and r['allocation']==src['allocation']]
        candidates.append(dict(frame=p['frame'],encoder=encoder,vs=p['vs'],ps=p['ps'],source=writer,destination=dst,source_srv=src,
                               same_dimensions=all(writer[k]==dst[k] for k in ['width','height','layers']),later_source_attachment_encoders=sorted({r['encoder'] for r in later}),label=label))
    counts=Counter((r['vs'],r['ps']) for r in candidates)
    return dict(candidates=candidates,shader_pairs=[dict(vs=k[0],ps=k[1],observations=v) for k,v in counts.items()],
                limitations=['Candidate identification only: shader hashes and state do not establish coverage or one-to-one coordinates.',
                             'Pipeline rows snapshot state on ordinary PSO setup, not every subsequent state mutation.',
                             'Detail budget, frame filter and dirty-binding suppression can omit observations.',
                             'Source preservation is required when later attachments/consumers use its allocation.'])

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('log',type=Path);a=p.parse_args()
    with a.log.open(errors='replace') as file:print(json.dumps(analyze(file),indent=2))
if __name__=='__main__':main()

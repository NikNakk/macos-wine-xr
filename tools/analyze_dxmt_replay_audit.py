#!/usr/bin/env python3
"""Join draw-level state, attachment provenance and private shader facts for an explicit scene selector."""
import argparse
from collections import Counter,defaultdict
import json
from pathlib import Path
import re

def fields(text):return dict(re.findall(r'(\w+)=([^\s]+)',text))
def count(rows,keys):
    return [dict(zip(keys,k),observations=v) for k,v in Counter(tuple(r.get(key,'unknown') for key in keys) for r in rows).most_common()]
def stencil_reads(row,mask):
    return row.get('stencil_enable')=='1' and int(row['stencil_read'])&mask and any(row[k] not in {'1','8'} for k in ['front_func','back_func'])
def stencil_writes(row):
    return row.get('stencil_enable')=='1' and int(row['stencil_write']) and not int(row.get('dsv_flags','0'))&2 and any(row[k]!='1' for k in ['front_fail','front_zfail','front_pass','back_fail','back_zfail','back_pass'])
def analyze(lines,shader_facts,width,height,color_format):
    draws=defaultdict(list);attachments=[];srvs=[];labels={}
    for line in lines:
        m=re.search(r'TBDR (drawstate|attachment|render|srv) (.*)',line)
        if not m:continue
        kind,tail=m.groups();r=fields(tail);r['encoder']=int(r['encoder']);r['frame']=int(r['frame'])
        if kind=='drawstate':draws[r['encoder']].append(r)
        elif kind=='attachment':attachments.append(r)
        elif kind=='srv':srvs.append(r)
        else:labels[r['encoder']]=tail.split('label=',1)[-1]
    selected=[];used=set()
    for color in attachments:
        if not (color['role']=='color' and color['width']==str(width) and color['height']==str(height) and color['format']==str(color_format) and color['samples']=='2' and color['load']=='2'):continue
        encoder=color['encoder'];main=draws[encoder]
        depth=next((r for r in attachments if r['encoder']==encoder and r['role']=='depth_stencil'),None)
        if not depth or not main:continue
        same=[r for r in attachments if r['frame']==color['frame'] and r['resource']==depth['resource'] and r['allocation']==depth['allocation']]
        pre=[r for r in same if r['encoder']<encoder and r['load']=='2' and draws[r['encoder']] and all(s['colors']=='0' for s in draws[r['encoder']])]
        if not pre:continue
        pre=max(pre,key=lambda r:r['encoder']);pre_draws=draws[pre['encoder']]
        for enc,items in [(encoder,main),(pre['encoder'],pre_draws)]:
            expected=re.search(r'draws=(\d+)',labels.get(enc,''))
            if not expected or len(items)!=int(expected[1]) or any('dsv_flags' not in r for r in items):
                raise ValueError('Incomplete per-draw snapshots for encoder '+str(enc))
        mask=0
        for r in main:
            if stencil_writes(r):mask|=int(r['stencil_write'])
        consumers=[]
        for a in same:
            if a['encoder']<=encoder:continue
            dependent=[r for r in draws[a['encoder']] if stencil_reads(r,mask)]
            if dependent:consumers.append(dict(encoder=a['encoder'],label=labels.get(a['encoder']),draws=len(dependent),states=count(dependent,['stencil_read','stencil_ref','front_func','back_func','dsv_flags'])))
        for r in main+pre_draws:
            used.update(r[k] for k in ['vs','ps','gs','hs','ds'] if r[k]!='none')
        color_uses=[]
        for kind,items in [('attachment',attachments),('srv',srvs)]:
            for row in items:
                if row['frame']==color['frame'] and row['encoder']>encoder and row['resource']==color['resource'] and row['allocation']==color['allocation']:
                    color_uses.append(dict(kind=kind,label=labels.get(row['encoder']),**row))
        selected.append(dict(frame=color['frame'],main_encoder=encoder,prepass_encoder=pre['encoder'],main_label=labels.get(encoder),prepass_label=labels.get(pre['encoder']),color_attachment=color,depth_attachment=depth,
          prepass_draws=len(pre_draws),prepass_ps_null=sum(r['ps']=='none' for r in pre_draws),prepass_ps_coverage=sum(r['coverage']!='0' for r in pre_draws),main_draws=len(main),main_ps_coverage=sum(r['coverage']!='0' for r in main),
          prepass_shaders=count(pre_draws,['vs','ps','gs','hs','ds']),main_shaders=count(main,['vs','ps','gs','hs','ds']),
          prepass_states=count(pre_draws,['depth_enable','depth_write','depth_func','stencil_enable','coverage','uavs','so','dsv_flags']),
          main_stencil_states=count(main,['stencil_enable','stencil_read','stencil_write','stencil_ref','front_func','front_fail','front_zfail','front_pass','back_func','back_fail','back_zfail','back_pass','dsv_flags']),
          configured_stencil_write_mask=mask,later_stencil_consumers=consumers,later_color_uses=sorted(color_uses,key=lambda r:r["encoder"]),
          unusual_stages_or_effect_bindings=sum(r['uavs']!='0' or r['so']!='0' or any(r[k]!='none' for k in ['gs','hs','ds']) for r in pre_draws+main)))
    return dict(selector=dict(width=width,height=height,color_format=color_format,samples=2),frames=selected,shader_facts={s:shader_facts.get(s,{'missing':True}) for s in sorted(used)},
                limitations=['Two-dimensional scene selector is explicit, not a production pass recogniser.','PreDraw state is captured, not proof of primitive/resource lifetime replay safety or instruction execution.','Configured stencil writes may be suppressed by per-sample coverage/depth; later consumers demonstrate the dependency, not its visual purpose.','No GS/HS/DS/UAV/stream-output binding is not a proof of safe replay.','Original shader bytecode and assembly are excluded; facts record declarations/opcodes conservatively.'])
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('log',type=Path);p.add_argument('shader_facts',type=Path);p.add_argument('--size',required=True);p.add_argument('--color-format',type=int,required=True);a=p.parse_args();w,h=map(int,a.size.split('x'))
    with a.log.open(errors='replace') as f:r=analyze(f,json.loads(a.shader_facts.read_text()),w,h,a.color_format)
    print(json.dumps(r,indent=2))
if __name__=='__main__':main()

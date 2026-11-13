#!/usr/bin/env python3
"""Validate HDR/stencil controls; stale-stencil and null-mask failures are expected counterexamples."""
import argparse,json,re
from pathlib import Path

def analyze(text):
    lines=re.findall(r'^HDR_STENCIL (.*)$',text,re.M)
    if len(lines)!=1:raise ValueError('Missing HDR/stencil result')
    r={k:float(v) if '.' in v else int(v) for k,v in re.findall(r'(\w+)=([\d.]+)',lines[0])}
    for k in ['identity_bad','mapped_bad','retained_depth_bad','restored_stencil_bad','restored_color_depth_bad','fullrate_bad']:
        if r[k]:raise ValueError('Control failed: '+k)
    if not(r['peak_hdr']>1 and r['resolved_peak_hdr']>1 and r['dropped_mask_bad']>0 and r['stale_stencil_samples']>0 and r['marker_reference']>0 and r['marker_stale']==0 and r['marker_restored']==r['marker_reference']):raise ValueError('Missing counterexample or restoration')
    mapped=sum(map(int,re.findall(r'TBDR cb[^\n]* mapped_render=(\d+)',text)))
    if mapped!=3:raise ValueError('Expected three actually mapped render passes, got '+str(mapped))
    return dict(result=r,actually_mapped_passes=mapped)
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('logs',type=Path,nargs='+');a=p.parse_args()
    print(json.dumps(dict(cases=[dict(log=str(path),**analyze(path.read_text(errors='replace'))) for path in a.logs],limitations=['Correctness-only fixture, not EXD performance or transparent replay.','Exact stencil-only replay adds full-resolution geometry and coverage work; its cost is unmeasured.','No material reconstruction from mapped stencil or HDR foveated image-quality claim.']),indent=2))
if __name__=='__main__':main()

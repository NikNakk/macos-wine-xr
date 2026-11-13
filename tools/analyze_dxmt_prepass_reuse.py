#!/usr/bin/env python3
"""Validate prepass-reuse controls and report reduced-rate counterexamples, not compatibility."""
import argparse
import json
from pathlib import Path
import re

def fields(line):
    return {k:float(v) if '.' in v else int(v) for k,v in re.findall(r'(\w+)=([\d.]+)',line)}

def analyze(path):
    text=Path(path).read_text(errors='replace')
    controls=[fields(s) for s in text.splitlines() if s.startswith('PREPASS_CONTROL ')]
    rows=[fields(s) for s in text.splitlines() if s.startswith('PREPASS_REUSE ')]
    if len(controls)!=1 or len(rows)!=3 or {r['filter'] for r in rows}!={0,1,2}:raise ValueError('Incomplete gate: '+str(path))
    c=controls[0]
    if any(c[k] for k in ['full_color_bad','mapped_color_bad','readonly_depth_bad']):raise ValueError('Reference control failed: '+str(path))
    for r in rows:
        if any(r[k] for k in ['readonly_depth_bad','source_depth_bad']):raise ValueError('Depth preservation failed: '+str(path))
        if r['rate']==1 and r['color_bad']:raise ValueError('Identity failed: '+str(path))
        if r['rate']<1 and not r['color_bad']:raise ValueError('Expected visibility counterexample absent: '+str(path))
        r['color_bad_percent']=100*r['color_bad']/r['checked_samples']
        r['nonblack_color_bad']=r['color_bad']-r['holes']
    mapped=sum(int(v) for v in re.findall(r'TBDR cb[^\n]* mapped_render=(\d+)',text))
    if mapped!=144:raise ValueError(f'Expected 144 actually mapped D3D passes, got {mapped}: {path}')
    return dict(log=str(path),control=c,rows=rows,actually_mapped_d3d_passes=mapped)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('logs',type=Path,nargs='+');a=p.parse_args()
    print(json.dumps(dict(cases=[analyze(log) for log in a.logs],limitations=['Correctness gate only: no end-to-end GPU time or EXD speedup is claimed.','Reduced-rate errors are expected counterexamples; they do not pass compatibility.','Colour comparison is per sample in the active physical layout against a matching mapped prepass, before unwarp.','D32Float and synthetic depth-invariant geometry; EXD combined stencil/HDR/eye layout are not tested.','center_bad is a central physical ROI, not proof of logical gaze-region correctness.']),indent=2))
if __name__=='__main__':main()

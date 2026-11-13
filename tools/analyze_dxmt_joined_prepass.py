#!/usr/bin/env python3
"""Validate joined prepass timing/controls; timings are one complete GPU sequence per sample."""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import re
import statistics

def fields(line):
    return {k:float(v) if '.' in v else int(v) for k,v in re.findall(r'(\w+)=([-\d.]+)',line)}

def stats(values):
    values=sorted(values)
    return dict(min=values[0],median=statistics.median(values),p10=values[round((len(values)-1)*.1)],p90=values[round((len(values)-1)*.9)],max=values[-1])

def analyze_text(text):
    controls=[fields(l) for l in text.splitlines() if l.startswith('JOINED_CONTROL ')]
    totals=[fields(l) for l in text.splitlines() if l.startswith('JOINED_PREPASS ')]
    samples=[fields(l) for l in text.splitlines() if l.startswith('JOINED_SAMPLE ')]
    if len(controls)!=2 or {r['after'] for r in controls}!={0,1} or len(totals)!=1:raise ValueError('Missing joined controls/result')
    result=totals[0]
    for c in controls:
        if any(c[k] for k in ['baseline_bad','depth_bad','mapped_bad','center_bad']):raise ValueError('Joined correctness failed')
        if c['rate']==1 and c['color_bad']:raise ValueError('Full-rate identity failed')
    if any(controls[0][k]!=controls[1][k] for k in ['baseline_bad','depth_bad','mapped_bad','color_bad','center_bad']):raise ValueError('Output changed across timing rounds')
    if result['native_reference_bad']:raise ValueError('Native reconstruction reference mismatch')
    if len(samples)!=130 or len({(s['mode'],s['round']) for s in samples})!=130:raise ValueError('Missing/duplicate GPU timing samples')
    measured=defaultdict(dict)
    for s in samples:
        if s['mode'] not in [0,1] or not 0<=s['round']<65 or s['warmup']!=int(s['round']<24) or s['gpu_ms']<=0:raise ValueError('Invalid timing/warmup')
        if not s['warmup']:measured[s['mode']][s['round']]=s
    if any(len(measured[m])!=41 for m in [0,1]):raise ValueError('Incomplete measured window')
    gpu={str(m):stats([s['gpu_ms'] for s in measured[m].values()]) for m in [0,1]}
    cpu={str(m):stats([s['cpu_record_ms'] for s in measured[m].values()]) for m in [0,1]}
    for m,key in [(0,'baseline_gpu_ms'),(1,'foveated_gpu_ms')]:
        if abs(gpu[str(m)]['median']-result[key])>2e-6:raise ValueError('Reported GPU median differs from samples')
    paired=stats([measured[0][i]['gpu_ms']-measured[1][i]['gpu_ms'] for i in measured[0]])
    return dict(result=result,controls=controls,gpu_ms=gpu,cpu_record_ms=cpu,paired_saving_ms=paired,
                savings_percent=100*(result['baseline_gpu_ms']-result['foveated_gpu_ms'])/result['baseline_gpu_ms'],
                final_color_bad_percent=100*controls[-1]['color_bad']/(result['size']**2),samples=samples)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('logs',type=Path,nargs='+');a=p.parse_args()
    cases=[]
    for path in a.logs:
        item=analyze_text(path.read_text(errors='replace'));item['log']=str(path);cases.append(item)
    print(json.dumps(dict(cases=cases,limitations=['Synthetic mono D3D11 fixture; no EXD/headset FPS or transparent interception claim.','GPU timestamps bracket ordinary prepass, extra mapped prepass, shading, hardware MSAA resolve, reconstruction and conventional consumer.','GPU timestamp/barrier overhead remains; compilation/allocation/readback and CPU waits are outside timed sequences.','CPU record time covers immediate calls and may include queue backpressure, not total CPU frame time.','Restricted no-discard, no-custom-depth, trivial vertex shaders and D32Float/RGBA8; no EXD stencil/HDR/stereo validation.','Physical mapped MSAA source remains mapped; final conventional output does not repair later source uses.']),indent=2))
if __name__=='__main__':main()

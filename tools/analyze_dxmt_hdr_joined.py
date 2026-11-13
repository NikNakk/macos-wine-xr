#!/usr/bin/env python3
"""Validate complete HDR/stencil foveation timings and controls, not component-median sums."""
import argparse,json,re,statistics
from pathlib import Path

def fields(text):return {k:float(v) if '.' in v else int(v) for k,v in re.findall(r'(\w+)=([-\d.]+)',text)}
def stats(values):
    v=sorted(values);return dict(min=v[0],p10=v[round((len(v)-1)*.1)],median=statistics.median(v),p90=v[round((len(v)-1)*.9)],max=v[-1])
def analyze(text):
    controls=[fields(s) for s in text.splitlines() if s.startswith('HDR_JOINED_CONTROL ')]
    samples=[fields(s) for s in text.splitlines() if s.startswith('HDR_JOINED_SAMPLE ')]
    summaries=[fields(s) for s in text.splitlines() if s.startswith('HDR_JOINED ')]
    if len(summaries)!=1:raise ValueError('Missing HDR result')
    result=summaries[0];modes=2 if result.get('source_materialized') else 3
    if len(controls)!=2*modes or {(r['mode'],r['after']) for r in controls}!={(m,a) for m in range(modes) for a in range(2)}:raise ValueError('Missing HDR controls')
    for c in controls:
        if any(c[k] for k in ['depth_bad','stencil_bad','mapped_bad','timed_consumer_bad']) or c['consumer_samples']!=c['expected_consumer_samples'] or c['consumer_samples']<=0 or c['peak_hdr']<=1:raise ValueError('HDR correctness failed')
        if (c['mode']==0 or c['rate']==1) and c['final_bad']:raise ValueError('Baseline/full-rate identity failed')
    for mode in range(modes):
        before=next(c for c in controls if c['mode']==mode and not c['after']);after=next(c for c in controls if c['mode']==mode and c['after'])
        if any(before[k]!=after[k] for k in ['final_bad','center_bad','consumer_samples','mae']):raise ValueError('Output changed across timing rounds')
    if result['replay_output_bad']:raise ValueError('Replay modes differ')
    if len(samples)!=65*modes or len({(s['mode'],s['round']) for s in samples})!=65*modes:raise ValueError('Missing/duplicate GPU intervals')
    measured={m:{} for m in range(modes)}
    for s in samples:
        if s['mode'] not in measured or not 0<=s['round']<65 or s['warmup']!=int(s['round']<24) or s['gpu_ms']<=0:raise ValueError('Invalid timestamp/warmup')
        if not s['warmup']:measured[s['mode']][s['round']]=s
    if any(len(v)!=41 for v in measured.values()):raise ValueError('Incomplete measured window')
    gpu={str(m):stats([r['gpu_ms'] for r in measured[m].values()]) for m in measured}
    cpu={str(m):stats([r['cpu_record_ms'] for r in measured[m].values()]) for m in measured}
    for m,key in ([(0,'baseline_gpu_ms'),(1,'full_replay_gpu_ms')]+([(2,'coverage_replay_gpu_ms')] if modes==3 else [])):
        if abs(gpu[str(m)]['median']-result[key])>2e-6:raise ValueError('Median inconsistent with raw samples')
    pairs={str(m):stats([measured[0][i]['gpu_ms']-measured[m][i]['gpu_ms'] for i in measured[0]]) for m in range(1,modes)}
    contrast=stats([measured[1][i]['gpu_ms']-measured[2][i]['gpu_ms'] for i in measured[0]]) if modes==3 else None
    paired_percent={str(m):stats([100*(measured[0][i]['gpu_ms']-measured[m][i]['gpu_ms'])/measured[0][i]['gpu_ms'] for i in measured[0]]) for m in range(1,modes)}
    source_controls=[];negative=None
    if modes==2:
        source_controls=[fields(s) for s in text.splitlines() if s.startswith('MSAA_SOURCE_CONTROL ')]
        negatives=[fields(s) for s in text.splitlines() if s.startswith('MSAA_SOURCE_NEGATIVE ')]
        if len(source_controls)!=4 or {(c['mode'],c['after']) for c in source_controls}!={(m,a) for m in range(2) for a in range(2)} or len(negatives)!=1:raise ValueError('Missing MSAA source controls')
        for c in source_controls:
            if any(c[k] for k in ['source_bad','alias_bad','snapshot_bad','load_bad']) or not(c['modified_samples']>0 and c['preserved_samples']>0):raise ValueError('MSAA source reuse failed')
        negative=negatives[0]
        if not(negative['broadcast_bad']>0 and negative['plane_witness']>0):raise ValueError('MSAA plane-collapse counterexample missing')
    return dict(result=result,controls=controls,gpu_ms=gpu,cpu_record_ms=cpu,paired_saving_ms=pairs,paired_saving_percent=paired_percent,full_minus_coverage_ms=contrast,source_controls=source_controls,source_negative=negative,samples=samples)
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('logs',nargs='+',type=Path);a=p.parse_args()
    print(json.dumps(dict(cases=[dict(log=str(path),**analyze(path.read_text(errors='replace'))) for path in a.logs],limitations=['Synthetic mono HDR/combined-stencil fixture with sampled alpha, discard/coverage and cheap VS; not EXD FPS.','One timestamp interval includes all prepasses/replay, native HDR resolves, reconstruction and stencil-tested colour consumer.','Colour-write-disabled original shader is compared with a hand-written coverage-only control, not an implemented game-shader transformation.','CPU record durations include possible queue backpressure; no pure CPU-cost or resident-memory claim.','Materialisation validates one owned single-mip texture and its views; generic aliases/renames/exports, game draw capture and stereo remain unresolved.']),indent=2))
if __name__=='__main__':main()

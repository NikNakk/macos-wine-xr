# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Join the existing client traces; no hardware run or source modification."""
import bisect, collections, csv, gzip, json
from pathlib import Path
root=Path(__file__).resolve().parent
summary=json.loads((root/'timing-summary.json').read_text());lo=summary['start_monotonic_ns'];hi=summary['end_monotonic_ns']
def rows(name):
    with gzip.open(root/f'monado_psvr2_77379_{name}.csv.gz','rt',newline='') as f:return list(csv.DictReader(f))
def stats(values):
    v=sorted(values)
    return {'n':len(v),**{k:v[round((len(v)-1)*q)] if v else None for k,q in [('median',.5),('p95',.95),('max',1)]}}
p=sorted([r for r in rows('presented') if lo<=int(r['presented_monotonic_ns'])<=hi],key=lambda r:int(r['presented_monotonic_ns']))
req={r['frame_id']:r for r in rows('present')};complete={r['frame_id']:r for r in rows('present_complete')};scheduled={r['frame_id']:r for r in rows('present_scheduled')};src={r['system_frame_id']:r for r in rows('reprojection_source')}
worker={r['frame_id']:r for r in rows('present_worker') if r['event']=='submitted'}
pipeline={}
for r in rows('frame_pipeline'):pipeline.setdefault(r['frame_id'],{})[r['event']]=r
joined=[]
for r in p:
    f=r['frame_id'];w=worker[f];q=req[f];c=complete[f];s=src[f];physical=int(r['presented_monotonic_ns']);pi=pipeline[f]
    joined.append({'frame_id':int(f),'physical_ns':physical,'source_frame_id':int(s['client_frame_id']),
        'pose_target_ns':int(s['system_display_time_ns']),'source_predicted_ns':int(s['client_display_time_ns']),
        'enqueue_to_worker_ms':(int(w['worker_start_ns'])-int(w['enqueue_ns']))/1e6,
        'worker_to_commit_ms':(int(q['after_commit_ns'])-int(w['worker_start_ns']))/1e6,
        'commit_to_scheduled_ms':(int(scheduled[f]['scheduled_callback_ns'])-int(q['after_commit_ns']))/1e6,
        'commit_to_completion_ms':int(c['commit_to_completion_ns'])/1e6,
        'completion_to_physical_ms':(physical-int(c['completion_handler_ns']))/1e6,
        'physical_after_pose_target_ms':(physical-int(s['system_display_time_ns']))/1e6,
        'renderer_cpu_ms':(int(pi['layer_commit_after_renderer']['event_ns'])-int(pi['layer_commit_before_renderer']['event_ns']))/1e6})
metrics={k:stats([r[k] for r in joined]) for k in joined[0] if k.endswith('_ms')}
metrics['drawable_prefetch_wait_ms']=stats([int(r['drawable_wait_ns'])/1e6 for r in rows('drawable_prefetch') if r['event']=='slot_ready' and lo<=int(r['event_ns'])<=hi])
metrics['client_semaphore_wait_ms']=stats([int(r['duration_ns'])/1e6 for r in rows('client_gpu') if r['event']=='semaphore_ready' and lo<=int(r['event_ns'])<=hi])
metrics['app_reported_gpu_ms']=stats([int(r['gpu_actual_ns'])/1e6 for r in rows('app_pacing') if r['event']=='gpu_done' and lo<=int(r['event_ns'])<=hi])
metrics['app_draw_actual_ms']=stats([int(r['draw_actual_ns'])/1e6 for r in rows('app_pacing') if r['event']=='delivered' and lo<=int(r['event_ns'])<=hi])
steady_rt=[r for r in rows('compositor_rt') if lo<=int(r['sample_ns'])<=hi]
rt=rows('compositor_rt');transitions=[r for r in rt if r['policy_transition']=='1']
ca=[r for r in rows('ca_callback') if lo<=int(r['callback_entry_ns'])<=hi];ct=sorted(int(r['callback_entry_ns']) for r in ca)
source_groups={'new':[],'reused':[]}
for a,b in zip(joined,joined[1:]):source_groups['new' if a['source_frame_id']!=b['source_frame_id'] else 'reused'].append(b['physical_after_pose_target_ms'])
result={'window':{'start_ns':lo,'end_ns':hi},'joined_frames':len(joined),'stages_ms':metrics,
    'ca_callback_interval_ms':stats([(b-a)/1e6 for a,b in zip(ct,ct[1:])]),
    'compositor_scheduler_samples':dict(collections.Counter((r['basic_policy']+'/'+r['cur_priority']) for r in steady_rt)),
    'scheduler_transition_rows':transitions,
    'pose_target_reversals':sum(b['pose_target_ns']<a['pose_target_ns'] for a,b in zip(joined,joined[1:])),
    'source_predicted_time_reversals':sum(b['source_predicted_ns']<a['source_predicted_ns'] for a,b in zip(joined,joined[1:])),
    'pose_target_lateness_by_source_transition_ms':{k:stats(v) for k,v in source_groups.items()},
    'scope':'Client traces only; no server policy/sensor traces or destination warp poses. App reported GPU includes readiness latency, not isolated hardware GPU execution. Scheduler thread ID is cached globally in source and must not be used to prove thread continuity. No proof of alternating image/pose mismatch.'}
(root/'lifecycle-summary.json').write_text(json.dumps(result,indent=2)+'\n')
with gzip.open(root/'lifecycle-joined.csv.gz','wt',newline='') as f:
    writer=csv.DictWriter(f,fieldnames=list(joined[0]));writer.writeheader();writer.writerows(joined)
print(json.dumps(result,indent=2))

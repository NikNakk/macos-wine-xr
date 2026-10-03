# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Analyze archived Underture PID 77379 traces; stdlib only."""
import csv, gzip, json, math
from pathlib import Path
root = Path(__file__).resolve().parent

def rows(name):
    with gzip.open(root / f'monado_psvr2_77379_{name}.csv.gz', 'rt', newline='') as f:
        return list(csv.DictReader(f))

def stats(values):
    v = sorted(values)
    return {'n': len(v), **{k: v[round((len(v)-1)*q)] if v else None for k,q in [('median',.5),('p95',.95),('p99',.99),('max',1)]}}

source = [r for r in rows('reprojection_source') if r['source_valid']=='1' and r['focused']=='1']
start = int(source[0]['system_display_time_ns']) + 2_000_000_000
end = int(source[-1]['system_display_time_ns']) - 1_000_000_000
physical = sorted([r for r in rows('presented') if start <= int(r['presented_monotonic_ns']) <= end], key=lambda r:int(r['presented_monotonic_ns']))
pt = [int(r['presented_monotonic_ns']) for r in physical]
intervals = [(b-a)/1e6 for a,b in zip(pt,pt[1:])]
app = [r for r in rows('app_pacing') if r['event']=='delivered' and start <= int(r['event_ns']) <= end]
at = sorted(int(r['event_ns']) for r in app)
by_frame = {r['system_frame_id']:r for r in source}
physical_source = [by_frame[r['frame_id']] for r in physical if r['frame_id'] in by_frame]
source_changes = sum(a['client_frame_id']!=b['client_frame_id'] for a,b in zip(physical_source,physical_source[1:]))
complete = {r['frame_id']:r for r in rows('present_complete')}
request = {r['frame_id']:r for r in rows('present')}
gpu_duration, gpu_to_physical = [], []
for r in physical:
    c, req = complete.get(r['frame_id']), request.get(r['frame_id'])
    if not c or not req: continue
    gpu_end_s = float(c['gpu_end_time_s'])
    gpu_end_ns = round(gpu_end_s*1e9) + int(r['presented_monotonic_ns']) - round(float(r['presented_time_host_s'])*1e9)
    if gpu_end_s > 0 and int(req['after_commit_ns'])-2_000_000 <= gpu_end_ns <= int(c['completion_handler_ns'])+2_000_000:
        gpu_to_physical.append((int(r['presented_monotonic_ns'])-gpu_end_ns)/1e6)
    gpu_duration.append((gpu_end_s-float(c['gpu_start_time_s']))*1000)
summary = {
    'pid':77379, 'identity':'OpenComposite_Underture, verified against service client descriptions',
    'window':'Focused valid source range, excluding first 2 seconds and last 1 second',
    'start_monotonic_ns':start,'end_monotonic_ns':end,'duration_s':(end-start)/1e9,
    'physical_presentations':len(physical),'physical_rate_hz':(len(pt)-1)*1e9/(pt[-1]-pt[0]),
    'physical_intervals_ms':stats(intervals),
    'physical_intervals_over_1_5_refresh':sum(x>12.512562 for x in intervals),
    'application_deliveries':len(app),'application_delivery_rate_hz':(len(at)-1)*1e9/(at[-1]-at[0]),
    'application_delivery_intervals_ms':stats([(b-a)/1e6 for a,b in zip(at,at[1:])]),
    'physical_compositor_frame_id_reversals':sum(int(b['frame_id'])<int(a['frame_id']) for a,b in zip(physical,physical[1:])),
    'physical_source_frame_id_reversals':sum(int(b['client_frame_id'])<int(a['client_frame_id']) for a,b in zip(physical_source,physical_source[1:])),
    'physical_source_changes':source_changes,
    'physical_source_reuse_fraction':1-source_changes/(len(physical_source)-1),
    'physical_minus_compositor_pose_target_ms':stats([(int(r['presented_monotonic_ns'])-int(by_frame[r['frame_id']]['system_display_time_ns']))/1e6 for r in physical if r['frame_id'] in by_frame]),
    'source_age_at_physical_ms':stats([(int(r['presented_monotonic_ns'])-int(by_frame[r['frame_id']]['client_display_time_ns']))/1e6 for r in physical if r['frame_id'] in by_frame]),
    'compositor_gpu_duration_ms':stats(gpu_duration),'gpu_end_to_physical_ms':stats(gpu_to_physical),
    'limits':'No external pose ground truth or image-pixel validation during this moving run. No conclusion about subjective backward motion mechanism, end-to-end app FPS, or overhead attributable to Wine. Source IDs do not prove pose/image correspondence.'
}
(root/'timing-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary,indent=2))

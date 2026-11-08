"""Summarize Monado delivered-frame cadence, excluding startup (nanosecond CSVs)."""
import csv
import json
import math
import statistics
import sys
from pathlib import Path

p = Path(sys.argv[1])
rows = list(csv.DictReader(p.open()))
delivered = [r for r in rows if r['event'] == 'delivered']
if not delivered:
    raise SystemExit('No delivered frames recorded yet')
origin = int(delivered[0]['event_ns'])
start = origin + 30_000_000_000
end = start + 60_000_000_000
sample = [r for r in delivered if start <= int(r['event_ns']) < end]
if len(sample) < 2:
    raise SystemExit('Waiting for 30-second warmup')
times = [int(r['event_ns']) for r in sample]
intervals = [(b-a)/1e6 for a,b in zip(times,times[1:])]
def pct(values, q):
    return sorted(values)[max(0, math.ceil(q*len(values))-1)] if values else None
frames = {(r['session_id'],r['client_frame_id']) for r in sample}
gpu = [int(r['gpu_actual_ns'])/1e6 for r in rows if r['event']=='gpu_done'
       and (r['session_id'],r['client_frame_id']) in frames and int(r['gpu_actual_ns'])>0]
result = dict(source=str(p), warmup_seconds=30, requested_sample_seconds=60,
    observed_sample_seconds=(times[-1]-times[0])/1e9, frames=len(sample),
    delivered_fps=(len(sample)-1)*1e9/(times[-1]-times[0]),
    interval_ms_median=statistics.median(intervals), interval_ms_p95=pct(intervals,.95),
    interval_ms_p99=pct(intervals,.99), interval_ms_max=max(intervals),
    gaps_over_25ms=sum(x>25 for x in intervals), gaps_over_50ms=sum(x>50 for x in intervals),
    gpu_actual_ms_median=statistics.median(gpu) if gpu else None,
    gpu_actual_ms_p95=pct(gpu,.95), gpu_samples=len(gpu),
    window_complete=int(delivered[-1]['event_ns'])>=end)
print(json.dumps(result,indent=2))

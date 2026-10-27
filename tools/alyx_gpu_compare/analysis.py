# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Analysis for the matched Alyx graphics-path comparison.

Two sources per run:

- A Metal System Trace (xctrace). GPU busy time per process is the union of
  that process's GPU intervals, so overlapping vertex, fragment and compute
  work is counted once. This is elapsed GPU time, as in the 2026-10-14 Home
  trace, not a hardware occupancy counter.
- Monado's app pacing trace (PSVR2_TIMING_TRACE=1, monado_psvr2_<pid>_app_pacing.csv),
  written by the service for each client session. Its times are
  CLOCK_UPTIME_RAW nanoseconds, the clock the runner records.

GPU ms per frame is the game's GPU busy ms per second divided by its delivered
frames per second over the same capture.
"""
import csv
import json
import statistics
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path


def union_ns(intervals):
    """Total length of the union of (start, end) intervals."""
    total = 0
    current_start = current_end = None
    for start, end in sorted(intervals):
        if end <= start:
            continue
        if current_end is None or start > current_end:
            if current_end is not None:
                total += current_end - current_start
            current_start, current_end = start, end
        else:
            current_end = max(current_end, end)
    if current_end is not None:
        total += current_end - current_start
    return total


def xctrace_rows(trace, schema):
    """Rows of one xctrace table as {mnemonic: (fmt, text)}, resolving refs."""
    xml = subprocess.run(
        ['xcrun', 'xctrace', 'export', '--input', str(trace), '--xpath',
         f'/trace-toc/run[@number="1"]/data/table[@schema="{schema}"]'],
        capture_output=True, check=True).stdout
    yield from parse_xctrace_xml(xml)


def parse_xctrace_xml(xml):
    root = ET.fromstring(xml)
    node = root.find('node')
    if node is None:
        return
    cols = [c.findtext('mnemonic') for c in node.find('schema').findall('col')]
    ids = {}

    def resolve(el):
        if 'ref' in el.attrib:
            return ids[el.attrib['ref']]
        if 'id' in el.attrib:
            ids[el.attrib['id']] = el
        for child in el:
            resolve(child)
        return el

    for row in node.findall('row'):
        out = {}
        for col, el in zip(cols, list(row)):
            el = resolve(el)
            if el.tag == 'sentinel':
                continue
            out[col] = (el.attrib.get('fmt', ''), el.text)
        yield out


def gpu_busy_by_process(rows):
    """{process label: {'busy_ns', 'intervals', 'first_ns', 'last_ns'}} from metal-gpu-intervals rows."""
    per = {}
    for row in rows:
        process = row.get('process', ('', ''))[0] or 'unknown'
        try:
            start = int(row['start'][1])
            duration = int(row['duration'][1])
        except (KeyError, TypeError, ValueError):
            continue
        per.setdefault(process, []).append((start, start + duration))
    out = {}
    for process, intervals in per.items():
        out[process] = dict(busy_ns=union_ns(intervals), intervals=len(intervals),
                            first_ns=min(s for s, _ in intervals), last_ns=max(e for _, e in intervals))
    return out


def trace_span_ns(per_process):
    if not per_process:
        return 0
    return max(p['last_ns'] for p in per_process.values()) - min(p['first_ns'] for p in per_process.values())


def match_process(per_process, needle):
    """Busy totals summed over every process whose label contains needle."""
    hits = {k: v for k, v in per_process.items() if needle.lower() in k.lower()}
    return hits, sum(v['busy_ns'] for v in hits.values())


def trace_toc_window(toc_xml):
    """(start, seconds) of run 1 from `xctrace export --toc`: a datetime and a float."""
    import datetime
    run = ET.fromstring(toc_xml).find('.//run')
    if run is None:
        return None
    start = run.findtext('.//start-date')
    duration = run.findtext('.//duration')
    if not start or not duration:
        return None
    start = start.rstrip('Z')
    return datetime.datetime.fromisoformat(start), float(duration)


def window_from_toc(toc_xml, clock_pair, trim_s=0.5):
    """The trace's recording window in CLOCK_UPTIME_RAW ns, or None.

    clock_pair is (wall time.time(), uptime ns) taken just before xctrace started.
    xctrace labels start-date with Z; it is tried as UTC and then as local time,
    and accepted only if it falls between 2 s before and 300 s after clock_pair.
    """
    import datetime
    parsed = trace_toc_window(toc_xml)
    if parsed is None or not clock_pair:
        return None
    start, seconds = parsed
    wall, uptime = clock_pair
    for tz in (datetime.timezone.utc, None):
        stamp = start.replace(tzinfo=tz).timestamp() if tz else start.timestamp()
        delta = stamp - wall
        if -2 <= delta <= 300:
            begin = uptime + int((delta + trim_s) * 1e9)
            return [begin, begin + int((seconds - 2 * trim_s) * 1e9)]
    return None


INTERVALS_FILE = 'gpu-intervals.csv.gz'
FIXED_WINDOW_S = (100, 150)
TRACE_INFO_FILE = 'trace-info.json'


def condense_trace(run_dir, clock_pair, keep_trace=False):
    """Keep what analysis needs from metal.trace, then delete it unless keep_trace.

    A Metal System Trace of all processes grows by about 40 MB a second. The analysis
    only needs each GPU interval (start, duration, process) and the recording window,
    which compress to a few MB. Returns a list of problems.
    """
    import gzip
    import shutil
    run_dir = Path(run_dir)
    trace = run_dir / 'metal.trace'
    if not trace.exists():
        return ['no metal.trace']
    problems = []
    try:
        rows = 0
        with gzip.open(run_dir / INTERVALS_FILE, 'wt', newline='') as f:
            w = csv.writer(f)
            w.writerow(['start_ns', 'duration_ns', 'process', 'channel'])
            for row in xctrace_rows(trace, 'metal-gpu-intervals'):
                try:
                    w.writerow([int(row['start'][1]), int(row['duration'][1]),
                                row.get('process', ('', ''))[0], row.get('channel-name', ('', ''))[0]])
                    rows += 1
                except (KeyError, TypeError, ValueError):
                    continue
        toc = subprocess.run(['xcrun', 'xctrace', 'export', '--input', str(trace), '--toc'],
                             capture_output=True, check=True).stdout
        info = dict(intervals=rows, window_uptime_ns=window_from_toc(toc, clock_pair),
                    trace_bytes=sum(p.stat().st_size for p in trace.rglob('*') if p.is_file()))
        parsed = trace_toc_window(toc)
        if parsed:
            info.update(start=parsed[0].isoformat(), seconds=parsed[1])
        (run_dir / TRACE_INFO_FILE).write_text(json.dumps(info, indent=2) + '\n')
        if rows == 0:
            problems.append('the trace has no GPU intervals')
    except subprocess.CalledProcessError as e:
        return [f'xctrace export failed: {e}']
    if not keep_trace and not problems:
        shutil.rmtree(trace)
    return problems


def read_intervals_file(path):
    """Rows of a condensed interval file in the shape gpu_busy_by_process expects."""
    import gzip
    with gzip.open(path, 'rt', newline='') as f:
        for row in csv.DictReader(f):
            yield {'start': ('', row['start_ns']), 'duration': ('', row['duration_ns']),
                   'process': (row['process'], ''), 'channel-name': (row['channel'], '')}


def read_app_pacing(path):
    import gzip
    opener = gzip.open if str(path).endswith('.gz') else open
    with opener(path, 'rt', newline='') as f:
        for row in csv.DictReader(f):
            try:
                yield {k: (v if k == 'event' else int(v)) for k, v in row.items()}
            except (TypeError, ValueError):
                continue  # a row cut off when the service stopped


def submitted_sizes(paths, window_start_ns, window_end_ns):
    """{'WxH': layers} of projection source sizes Monado composited inside the window."""
    import gzip
    counts = {}
    for path in paths:
        opener = gzip.open if str(path).endswith('.gz') else open
        with opener(path, 'rt', newline='') as f:
            for row in csv.DictReader(f):
                try:
                    when, w, h = int(row['event_ns']), int(row['source_width']), int(row['source_height'])
                except (KeyError, TypeError, ValueError):
                    continue
                if w and h and window_start_ns <= when <= window_end_ns:
                    counts[f'{w}x{h}'] = counts.get(f'{w}x{h}', 0) + 1
    return dict(sorted(counts.items(), key=lambda kv: -kv[1]))


def percentile(values, q):
    if not values:
        return None
    values = sorted(values)
    position = (len(values) - 1) * q
    low = int(position)
    high = min(low + 1, len(values) - 1)
    return values[low] + (values[high] - values[low]) * (position - low)


def pacing_summary(rows, window_start_ns, window_end_ns, refresh_ns):
    """Frame timing of the busiest session inside [window_start_ns, window_end_ns]."""
    delivered = {}
    gpu = {}
    for row in rows:
        if not window_start_ns <= row['event_ns'] <= window_end_ns:
            continue
        if row['event'] == 'delivered':
            delivered.setdefault(row['session_id'], []).append(row)
        elif row['event'] == 'gpu_done':
            gpu.setdefault(row['session_id'], []).append(row['gpu_actual_ns'])
    if not delivered:
        return None
    session = max(delivered, key=lambda s: len(delivered[s]))
    frames = sorted(delivered[session], key=lambda r: r['event_ns'])
    seconds = (window_end_ns - window_start_ns) / 1e9
    intervals = [(b['event_ns'] - a['event_ns']) / 1e6 for a, b in zip(frames, frames[1:])]
    display = [b['display_time_ns'] - a['display_time_ns'] for a, b in zip(frames, frames[1:])
               if a['display_time_ns'] > 0 and b['display_time_ns'] > 0]
    refreshes = {}
    for d in display:
        n = max(1, round(d / refresh_ns))
        refreshes[n] = refreshes.get(n, 0) + 1
    gpu_ms = [g / 1e6 for g in gpu.get(session, []) if g > 0]
    return dict(session_id=session, frames=len(frames), seconds=seconds,
                fps=len(frames) / seconds if seconds > 0 else None,
                interval_ms_p10=percentile(intervals, .1), interval_ms_p50=percentile(intervals, .5),
                interval_ms_p90=percentile(intervals, .9),
                gaps_over_50ms=sum(1 for i in intervals if i > 50),
                refreshes_per_frame=dict(sorted(refreshes.items())),
                monado_gpu_actual_ms_p50=percentile(gpu_ms, .5), monado_gpu_actual_ms_p90=percentile(gpu_ms, .9),
                sessions=sorted(delivered))


def analyze_run(run_dir, process_needle, refresh_hz):
    """Summary dict for one run directory written by the runner."""
    run_dir = Path(run_dir)
    record = json.loads((run_dir / 'run.json').read_text())
    out = dict(name=record['name'], variant=record['variant'], order=record['order'],
               thermal=record.get('thermal'), problems=list(record.get('problems', [])))
    trace = run_dir / 'metal.trace'
    condensed = run_dir / INTERVALS_FILE
    info_path = run_dir / TRACE_INFO_FILE
    info = json.loads(info_path.read_text()) if info_path.exists() else {}
    if condensed.exists() or trace.exists():
        try:
            per = gpu_busy_by_process(read_intervals_file(condensed) if condensed.exists()
                                      else xctrace_rows(trace, 'metal-gpu-intervals'))
        except subprocess.CalledProcessError as e:
            per = {}
            out['problems'].append(f'xctrace export failed: {e}')
        span = trace_span_ns(per)
        hits, busy = match_process(per, process_needle)
        out['trace_seconds'] = span / 1e9
        out['game_processes'] = sorted(hits)
        out['game_gpu_busy_ms_per_s'] = busy / span * 1e3 if span else None
        top = sorted(per.items(), key=lambda kv: -kv[1]['busy_ns'])[:6]
        out['top_processes'] = [dict(process=k, gpu_busy_ms_per_s=v['busy_ns'] / span * 1e3 if span else None,
                                     intervals=v['intervals']) for k, v in top]
        if not hits:
            out['problems'].append(f'no process in the Metal trace matches {process_needle!r}')
    else:
        out['problems'].append('no metal.trace')
    pacing_files = sorted((run_dir / 'timing').glob('monado_psvr2_*_app_pacing.csv*'))
    window = info.get('window_uptime_ns')
    if window is None and trace.exists() and record.get('clock_pair'):
        try:
            toc = subprocess.run(['xcrun', 'xctrace', 'export', '--input', str(trace), '--toc'],
                                 capture_output=True, check=True).stdout
            window = window_from_toc(toc, record['clock_pair'])
        except subprocess.CalledProcessError:
            pass
    if window is None:
        window = record.get('capture_uptime_ns')
        if window:
            out['problems'].append('pacing window from the runner, not the trace (wider than the trace)')
    out['pacing_window_s'] = (window[1] - window[0]) / 1e9 if window else None
    # A second, trace-independent measure: frame timing over a fixed span after the
    # first delivered frame (default 100-150 s, before a capture starts at 150 s).
    out['fixed_window'] = None
    pacing_all = sorted((run_dir / 'timing').glob('monado_psvr2_*_app_pacing.csv*'))
    for f in pacing_all:
        rows = list(read_app_pacing(f))
        delivered = [r['event_ns'] for r in rows if r['event'] == 'delivered']
        if not delivered:
            continue
        t0 = delivered[0]
        fixed = pacing_summary(rows, t0 + int(FIXED_WINDOW_S[0] * 1e9), t0 + int(FIXED_WINDOW_S[1] * 1e9),
                               1e9 / refresh_hz)
        if fixed and (out['fixed_window'] is None or fixed['frames'] > out['fixed_window']['frames']):
            out['fixed_window'] = fixed
    if pacing_files and window:
        # Clients may write their own (empty) traces beside the service's; session ids are
        # per process, so keep files apart and take the one with the most frames.
        summaries = [pacing_summary(read_app_pacing(f), window[0], window[1], 1e9 / refresh_hz)
                     for f in pacing_files]
        summaries = [s for s in summaries if s]
        out['pacing'] = max(summaries, key=lambda s: s['frames']) if summaries else None
        if out['pacing'] is None:
            out['problems'].append('no delivered frames in the capture window')
    else:
        out['problems'].append('no app pacing trace or capture window')
    composition = sorted((run_dir / 'timing').glob('monado_psvr2_*_composition.csv*'))
    out['submitted_sizes'] = submitted_sizes(composition, window[0], window[1]) if composition and window else {}
    if len(out['submitted_sizes']) > 1:
        out['problems'].append('the submitted size changed during the capture')
    out['render_scale'] = record.get('render_scale')
    out['map'] = record.get('map')
    fps = (out.get('pacing') or {}).get('fps')
    busy = out.get('game_gpu_busy_ms_per_s')
    out['game_gpu_ms_per_frame'] = busy / fps if busy is not None and fps else None
    # Per-pixel cost uses the game's own render size; the submitted (swapchain) size is
    # the same for every run and only shows that the runtime recommendation matched.
    out['size'] = record.get('internal_size') or next(iter(out['submitted_sizes']), None)
    out['size_source'] = 'game' if record.get('internal_size') else 'swapchain'
    if out['size'] and out['game_gpu_ms_per_frame'] is not None:
        w, h = (int(x) for x in out['size'].split('x'))
        # Both eyes. Not strictly linear in pixels; for comparing runs whose sizes differ slightly.
        out['gpu_ms_per_megapixel'] = out['game_gpu_ms_per_frame'] / (2 * w * h / 1e6)
    out['swapchain_sizes'] = record.get('swapchain_sizes', [])
    return out


def summarize(results):
    """Per variant medians across repetitions."""
    maps = {r.get('map') for r in results}
    if len(maps) > 1:
        for r in results:
            r['problems'].append('runs loaded different maps: ' + ', '.join(sorted(str(m) for m in maps)))
    by_variant = {}
    for r in results:
        by_variant.setdefault(r['variant'], []).append(r)

    def med(rs, *keys):
        vals = []
        for r in rs:
            v = r
            for k in keys:
                v = v.get(k) if isinstance(v, dict) else None
            if v is not None:
                vals.append(v)
        return statistics.median(vals) if vals else None

    def sizes(rs):
        return sorted({r['size'] for r in rs if r.get('size')})

    return {variant: dict(runs=len(rs), sizes=sizes(rs),
                          gpu_ms_per_megapixel=med(rs, 'gpu_ms_per_megapixel'),
                          game_gpu_ms_per_frame=med(rs, 'game_gpu_ms_per_frame'),
                          game_gpu_busy_ms_per_s=med(rs, 'game_gpu_busy_ms_per_s'),
                          fps=med(rs, 'pacing', 'fps'),
                          interval_ms_p50=med(rs, 'pacing', 'interval_ms_p50'),
                          interval_ms_p90=med(rs, 'pacing', 'interval_ms_p90'),
                          gaps_over_50ms=med(rs, 'pacing', 'gaps_over_50ms'),
                          fixed_window_fps=med(rs, 'fixed_window', 'fps'),
                          fixed_window_interval_ms_p50=med(rs, 'fixed_window', 'interval_ms_p50'))
            for variant, rs in by_variant.items()}


def markdown(summary, results):
    def f(v, digits=2):
        return '—' if v is None else f'{v:.{digits}f}'

    lines = ['| Variant | Runs | Render size per eye | Game GPU ms/frame | GPU ms per Mpixel | Game GPU busy ms/s | Frames/s | '
             'Interval p50 / p90 ms | Gaps > 50 ms | Frames/s, 100-150 s |',
             '| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
    for variant, s in summary.items():
        lines.append(f"| {variant} | {s['runs']} | {', '.join(s['sizes']) or '—'} | {f(s['game_gpu_ms_per_frame'])} | "
                     f"{f(s['gpu_ms_per_megapixel'], 3)} | {f(s['game_gpu_busy_ms_per_s'], 0)} | "
                     f"{f(s['fps'], 1)} | {f(s['interval_ms_p50'], 1)} / {f(s['interval_ms_p90'], 1)} | "
                     f"{f(s['gaps_over_50ms'], 0)} | {f(s['fixed_window_fps'], 1)} |")
    lines += ['', 'Per run, in run order:', '',
              '| # | Run | Variant | Map | Scale | Render size (swapchain) | Thermal start/end | GPU ms/frame | Frames/s | Problems |',
              '| ---: | --- | --- | --- | ---: | --- | --- | ---: | ---: | --- |']
    for r in sorted(results, key=lambda r: r['order']):
        thermal = r.get('thermal') or {}
        sizes = f"{r.get('size') or '?'} ({', '.join((r.get('submitted_sizes') or {}).keys()) or '—'})"
        lines.append(f"| {r['order']} | {r['name']} | {r['variant']} | {r.get('map') or '?'} | {r.get('render_scale') or '—'} | {sizes} | "
                     f"{thermal.get('start', '?')}/{thermal.get('end', '?')} | "
                     f"{f(r.get('game_gpu_ms_per_frame'))} | {f((r.get('pacing') or {}).get('fps'), 1)} | "
                     f"{'; '.join(r['problems']) or '—'} |")
    return '\n'.join(lines) + '\n'

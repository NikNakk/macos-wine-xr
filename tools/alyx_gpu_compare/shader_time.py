#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""GPU time per shader from a Metal System Trace's shader profiler.

usage: shader_time.py TRACE PROCESS_SUBSTRING [--top N]

Joins gpu-shader-profiler-interval samples (by program counter) to the
shader address ranges in metal-shader-profiler-shader-list. Needs a trace
recorded with Instruments' shader timeline on: the plain 'Metal System Trace'
template from xctrace samples almost nothing (21 samples in 8 s, 2026-10-26). Sampled time,
not exact: use it to rank shaders and to split work by shader type, with
`analysis.py`'s interval union as the total.
"""
import bisect
import collections
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analysis import xctrace_rows  # noqa: E402


def shader_ranges(trace, process):
    ranges = []
    for row in xctrace_rows(trace, 'metal-shader-profiler-shader-list'):
        if process not in (row.get('process') or ('', ''))[0]:
            continue
        try:
            start, end = int(row['pc-start'][1]), int(row['pc-end'][1])
        except (KeyError, TypeError, ValueError):
            continue
        name = row['name'][0].rsplit(' (', 1)[0]
        ranges.append((start, end, name, row.get('shader-type', ('?', ''))[0]))
    ranges.sort()
    return ranges


def shader_time(trace, process):
    ranges = shader_ranges(trace, process)
    starts = [r[0] for r in ranges]
    per = collections.Counter()
    kinds = {}
    unmatched = 0
    first = last = None
    for row in xctrace_rows(trace, 'gpu-shader-profiler-interval'):
        try:
            pc, start, duration = int(row['pc'][1]), int(row['start'][1]), int(row['duration'][1])
        except (KeyError, TypeError, ValueError):
            continue
        i = bisect.bisect_right(starts, pc) - 1
        if i < 0 or pc > ranges[i][1]:
            unmatched += duration  # another process's shader, or one outside the list
            continue
        _, _, name, kind = ranges[i]
        per[name] += duration
        kinds[name] = kind
        first = start if first is None else min(first, start)
        last = start + duration if last is None else max(last, start + duration)
    return per, kinds, unmatched, ((last - first) / 1e9 if first is not None else 0)


def main():
    trace, process = sys.argv[1], sys.argv[2]
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 25
    per, kinds, unmatched, span = shader_time(trace, process)
    total = sum(per.values())
    by_kind = collections.Counter()
    for name, t in per.items():
        by_kind[kinds[name]] += t
    print(f'{process}: {len(per)} shaders, sampled span {span:.1f} s, matched {total / 1e6:.0f} ms '
          f'(other processes or unknown {unmatched / 1e6:.0f} ms)')
    for kind, t in by_kind.most_common():
        print(f'  {kind:10} {t / total * 100:5.1f}%')
    print(f'{"share":>6}  {"type":10} shader')
    for name, t in per.most_common(top):
        print(f'{t / total * 100:5.1f}%  {kinds[name]:10} {name[:90]}')


if __name__ == '__main__':
    main()

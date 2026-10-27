#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Who waits for whom, from a Thread Activity recording.

usage: thread_waits.py TRACE [--top N] [--thread TID ...] [--json OUT]

Reads the thread-state table of an xctrace Thread Activity recording
(compare.py --trace-template threads): every interval each thread spent
running, blocked or runnable (ready but waiting for a core), and for each wake
the thread that made it runnable. Reports, per second of recording:

- each busy thread's running, runnable and blocked time;
- for each thread, its blocked time split by the thread that woke it, so a
  chain such as render thread -> waits for encoder thread shows up directly;
- for chosen threads (--thread), how long their blocked spells last.

The table has no backtraces, so it exports for Wine processes, where the
time-profile export crashes xctrace.
"""
import argparse
import collections
import json
import re
import subprocess
import sys
import xml.etree.ElementTree as ET


def thread_state_rows(stream):
    """Yields (start_ns, thread, state, duration_ns, woken_by thread or None)."""
    values = {}
    cols = []
    stack = []
    for event, el in ET.iterparse(stream, events=('start', 'end')):
        if event == 'start':
            stack.append(el)
            continue
        stack.pop()
        tag = el.tag
        if tag == 'mnemonic':
            cols.append(el.text)
            continue
        if tag == 'row':
            out = {}
            for col, child in zip(cols, list(el)):
                ref = child.attrib.get('ref')
                out[col] = values.get(ref) if ref else values.get(child.attrib.get('id'))
            el.clear()
            if stack:
                stack[-1].remove(el)
            yield (int(out.get('start') or 0), out.get('thread') or '?', out.get('state') or '?',
                   int(out.get('duration') or 0), out.get('made-runnable-by-thread'))
            continue
        if 'id' not in el.attrib:
            continue
        if tag in ('start-time', 'duration'):
            value = el.text
        else:
            value = el.attrib.get('fmt', el.text)
        values[el.attrib['id']] = value


def short(thread):
    """'dxmt-encode-thr (0x60f708)' from 'dxmt-encode-thr (0x60f708) (wine, pid: 22412)'."""
    return re.sub(r' \([^()]*, pid: \d+\)$', '', thread or '?')


def analyze(rows, pid=None):
    state_ns = collections.defaultdict(collections.Counter)
    woken = collections.defaultdict(collections.Counter)    # thread -> waker -> blocked ns ended by it
    wakes = collections.defaultdict(collections.Counter)    # thread -> waker -> count
    spells = collections.defaultdict(list)                  # thread -> blocked durations
    last_blocked = {}
    first = last = None
    for start, thread, state, duration, waker in rows:
        if pid and f'pid: {pid})' not in thread:
            continue
        t = short(thread)
        first = start if first is None else min(first, start)
        last = start + duration if last is None else max(last, start + duration)
        state_ns[t][state] += duration
        if state == 'Blocked':
            last_blocked[t] = duration
            spells[t].append(duration)
        elif state == 'Runnable' and t in last_blocked:
            # the wake that ended the blocked spell comes with the runnable interval after it
            w = short(waker) if waker else '(no thread: timer or interrupt)'
            woken[t][w] += last_blocked.pop(t)
            wakes[t][w] += 1
    return dict(span_s=(last - first) / 1e9 if first is not None else 0, state_ns=state_ns,
                woken=woken, wakes=wakes, spells=spells)


def report(a, top, threads):
    s = a['span_s'] or 1
    print(f'recording span {a["span_s"]:.1f} s; ms per second of recording')
    busy = sorted(a['state_ns'].items(), key=lambda kv: -kv[1]['Running'])[:top]
    print(f'\n{"running":>8} {"runnable":>8} {"blocked":>8}  thread')
    for t, c in busy:
        print(f'{c["Running"] / 1e6 / s:8.1f} {c["Runnable"] / 1e6 / s:8.1f} {c["Blocked"] / 1e6 / s:8.1f}  {t}')
    print('\nblocked time by the thread that ended it (ms/s, wakes/s)')
    for t, _ in busy:
        w = a['woken'].get(t)
        if not w:
            continue
        print(f'  {t}')
        for waker, ns in w.most_common(5):
            print(f'    {ns / 1e6 / s:7.1f} ms/s  {a["wakes"][t][waker] / s:6.1f}/s  woken by {waker}')
    for t in threads:
        match = [k for k in a['spells'] if t in k]
        for k in match:
            d = sorted(a['spells'][k])
            if not d:
                continue
            q = lambda p: d[min(len(d) - 1, int(len(d) * p))] / 1e6
            print(f'\nblocked spells of {k}: {len(d) / s:.0f}/s, median {q(.5):.2f} ms, p90 {q(.9):.2f}, '
                  f'p99 {q(.99):.2f}; over 1 ms: {sum(x for x in d if x > 1e6) / 1e6 / s:.1f} ms/s')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('trace')
    ap.add_argument('--pid', help='only this process (default: every process in the recording)')
    ap.add_argument('--top', type=int, default=20)
    ap.add_argument('--thread', action='append', default=[], help='thread name or tid substring')
    ap.add_argument('--json', help='also write the totals here')
    a = ap.parse_args()
    xc = subprocess.Popen(['xcrun', 'xctrace', 'export', '--input', a.trace, '--xpath',
                           '/trace-toc/run[@number="1"]/data/table[@schema="thread-state"]'],
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    result = analyze(thread_state_rows(xc.stdout), a.pid)
    if xc.wait() != 0:
        print(f'warning: xctrace export exited {xc.returncode}; incomplete', file=sys.stderr)
        return 3
    report(result, a.top, a.thread)
    if a.json:
        s = result['span_s'] or 1
        with open(a.json, 'w') as f:
            json.dump(dict(span_s=result['span_s'],
                           threads={t: {k: v / 1e6 / s for k, v in c.items()} for t, c in result['state_ns'].items()},
                           woken_by={t: {w: ns / 1e6 / s for w, ns in c.most_common(8)}
                                     for t, c in result['woken'].items()}), f, indent=1)


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""CPU time by process, thread and code from a Time Profiler recording.

usage: cpu_profile.py TRACE [--process SUBSTRING ...] [--vmmap FILE] [--top N] [--json OUT]

Reads the time-profile table of an xctrace Time Profiler recording
(compare.py --trace-template cpu) as a stream, since the export can run to
gigabytes of XML. Reports CPU ms per second of recording for each matching
process and thread, and for each busy thread the binaries and functions its
samples land in (the innermost frame). Wine's PE modules are not dyld
images, so their frames are bare addresses; with --vmmap (`vmmap -wide PID`
taken while the game ran) they are named by the file mapped there, such as
hlvr.exe or d3d11.dll.
"""
import argparse
import bisect
import collections
import gzip
import json
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

DEFAULT_PROCESSES = ('hlvr', 'wine', 'monado', 'vrcompositor', 'vrserver', 'WindowServer', 'steam')


def time_profile_rows(stream):
    """Yields (time_ns, thread, process, state, weight_ns, frames) per sample.

    frames: innermost first, each (function name, binary name or None)."""
    values = {}  # xctrace's id -> parsed value; later rows refer back with ref=
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
            yield (int(out.get('time') or 0), out.get('thread') or '?', out.get('process') or '?',
                   out.get('thread-state') or '', int(out.get('weight') or 0), out.get('stack') or ())
            continue
        if 'id' not in el.attrib:
            continue
        if tag == 'frame':
            binary = None
            for child in el:
                if child.tag == 'binary':
                    binary = values.get(child.attrib.get('ref') or child.attrib.get('id'))
            value = (el.attrib.get('name', '?'), binary, el.attrib.get('addr'))
        elif tag == 'binary':
            value = el.attrib.get('name')
        elif tag in ('tagged-backtrace', 'backtrace'):
            frames = []
            for child in el.iter('frame'):
                if child is el:
                    continue
                v = values.get(child.attrib.get('ref') or child.attrib.get('id'))
                if v is not None:
                    frames.append(v)
            value = tuple(frames)
        elif tag in ('weight', 'sample-time'):
            value = el.text
        else:
            value = el.attrib.get('fmt', el.text)
        values[el.attrib['id']] = value


def time_sample_rows(stream):
    """Yields (time_ns, thread, process, state, sample kind, user stack addresses innermost first).

    The time-sample table holds the raw samples without symbolicated backtraces. xctrace
    can export it when exporting time-profile crashes (as it does for Wine processes)."""
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
            thread = out.get('thread') or '?'
            m = re.search(r'\(([^()]*), pid: (\d+)\)$', thread)
            process = f'{m.group(1)} ({m.group(2)})' if m else '?'
            yield (int(out.get('time') or 0), thread, process, out.get('thread-state') or '',
                   out.get('sample-type') or '', out.get('cp-user-callstack') or ())
            continue
        if 'id' not in el.attrib:
            continue
        if tag == 'text-addresses':
            value = tuple(int(x) for x in (el.text or '').split())
        elif tag == 'kperf-bt':
            value = ()
            for child in el:
                if child.tag == 'text-addresses':
                    value = values.get(child.attrib.get('ref') or child.attrib.get('id')) or ()
        elif tag == 'sample-time':
            value = el.text
        else:
            value = el.attrib.get('fmt', el.text)
        values[el.attrib['id']] = value


def profile_samples(stream, needles, mapper, interval_ns):
    """profile() for the time-sample table: running timer samples, each worth interval_ns."""
    per_thread = collections.Counter()
    per_process = collections.Counter()
    binaries = collections.defaultdict(collections.Counter)
    functions = collections.defaultdict(collections.Counter)
    first = last = None
    total = 0
    for t, thread, process, state, kind, addrs in time_sample_rows(stream):
        first = t if first is None else min(first, t)
        last = t if last is None else max(last, t)
        if state != 'Running' or kind == 'Stackshot':
            continue
        total += interval_ns
        if needles and not any(n.lower() in process.lower() for n in needles):
            continue
        key = (process, thread_label(thread))
        per_thread[key] += interval_ns
        per_process[process] += interval_ns
        where = describe(addrs[0], mapper) if addrs else '(no stack)'
        module = next((mapper.name(a) for a in addrs if mapper.is_module(a)), None)
        binaries[key][where] += interval_ns
        functions[key][f'in {where}, inside {module}' if module and module != where else f'in {where}'] \
            += interval_ns
    span_s = (last - first) / 1e9 if first is not None else 0
    return dict(span_s=span_s, total_ns=total, per_process=per_process, per_thread=per_thread,
                binaries=binaries, functions=functions)


def describe(addr, mapper):
    name = mapper.name(addr)
    if name:
        return name
    if addr >= 0x7ff800000000:
        return '(system library)'  # the x86_64 dyld shared cache in a translated process
    return '(unknown)'


VMMAP_LINE = re.compile(r'^(\S.*?)\s+([0-9a-f]{6,})-([0-9a-f]{6,})\s+\[[^\]]*\]\s+\S+\s+SM=\S+\s*(.*)$')


def vmmap_lines(path):
    """(region type, start, end, path or '') for each region line of `vmmap -wide` output."""
    if not path or not os.path.exists(path):
        return
    opener = gzip.open if path.endswith('.gz') else open
    with opener(path, 'rt', errors='replace') as f:
        for line in f:
            m = VMMAP_LINE.match(line.rstrip())
            if m:
                yield m.group(1).strip(), int(m.group(2), 16), int(m.group(3), 16), m.group(4).strip()


def region_name(kind, path):
    if not path:
        return kind
    name = os.path.basename(path)
    return name[:-4] + ' (Rosetta AOT)' if name.endswith('.aot') else name


def read_vmmap(path):
    """Sorted (start, end, name) of every region: the mapped file's name, else the region type."""
    return sorted((start, end, region_name(kind, p)) for kind, start, end, p in vmmap_lines(path))


def pe_layout(path):
    """(SizeOfImage, [(VirtualAddress, mapped size)]) of a PE file, or None."""
    import struct
    try:
        with open(path, 'rb') as f:
            head = f.read(4096)
    except OSError:
        return None
    if head[:2] != b'MZ':
        return None
    pe = struct.unpack_from('<I', head, 0x3c)[0]
    if head[pe:pe + 4] != b'PE\0\0':
        return None
    sections, opt_size = struct.unpack_from('<HxxxxxxxxxxxxH', head, pe + 6)
    opt = pe + 24
    size_of_image, size_of_headers = struct.unpack_from('<II', head, opt + 56)
    out = [(0, size_of_headers)]  # the headers are mapped at the image base
    for i in range(sections):
        o = opt + opt_size + 40 * i
        if o + 40 > len(head):
            break
        vsize, va, raw_size = struct.unpack_from('<III', head, o + 8)
        out.append((va, max(vsize, raw_size)))
    return size_of_image, out


def pe_modules(path):
    """Sorted (start, end, name) of Wine-loaded PE images.

    Wine reads most PE sections into anonymous memory, so vmmap shows only a few
    file-mapped pieces of each image. Each piece sits at base + some section's RVA;
    the base most pieces agree on, with the image size from the file's header,
    gives the module's whole range."""
    pieces = collections.defaultdict(list)
    for kind, start, end, p in vmmap_lines(path):
        if p.lower().endswith(('.dll', '.exe')):
            pieces[p].append((start, end))
    out = []
    for p, regions in pieces.items():
        layout = pe_layout(p)
        if not layout:
            continue
        size_of_image, sections = layout
        votes = collections.Counter()
        for start, end in regions:
            for va, size in sections:
                base = start - va
                if base % 0x10000 or end - start > ((size + 0xfff) & ~0xfff):
                    continue
                # a piece the exact size of a section is better evidence; the header piece best
                votes[base] += 1 + (end - start == ((size + 0xfff) & ~0xfff)) + 2 * (va == 0)
        if votes:
            base = votes.most_common(1)[0][0]
            out.append((base, base + size_of_image, os.path.basename(p)))
    out.sort()
    # where two guesses still overlap, end the first where the next begins
    return [(a, min(b, out[i + 1][0]) if i + 1 < len(out) else b, n) for i, (a, b, n) in enumerate(out)]


class Mapper:
    """Names addresses: a PE module first, else the vmmap region they fall in."""

    def __init__(self, regions, modules=()):
        self.tables = [(list(modules), [m[0] for m in modules]), (regions, [r[0] for r in regions])]

    def name(self, addr):
        if not addr:
            return None
        try:
            a = int(addr, 16) if isinstance(addr, str) else addr
        except ValueError:
            return None
        for table, starts in self.tables:
            i = bisect.bisect_right(starts, a) - 1
            if i >= 0 and a < table[i][1]:
                return table[i][2]
        return None

    def is_module(self, addr):
        table, starts = self.tables[0]
        i = bisect.bisect_right(starts, addr) - 1
        return i >= 0 and addr < table[i][1]


def leaf(frames, mapper):
    """(binary, function) of the innermost frame; a bare address is named by its mapped file."""
    if not frames:
        return '(no stack)', ''
    name, binary, addr = frames[0]
    if binary:
        return binary, name
    mapped = mapper.name(addr)
    # name the code by the innermost frame that has a symbol, so a sample in an unnamed
    # PE function still shows which named code called it
    caller = next((f'{n} [{b}]' for n, b, _ in frames[1:] if b and not n.startswith('0x')), '')
    if mapped:
        where = mapped + ' (no symbols)'
    elif addr and int(addr, 16) >= 0x7ff800000000:
        where = '(system library, no symbols)'  # the x86_64 dyld shared cache in a translated process
    else:
        where = '(unknown)'
    return where, ('under ' + caller if caller else name)


def thread_label(thread):
    """'RenderThread (0x1f2e3)' from xctrace's 'RenderThread (0x1f2e3) (hlvr.exe, pid: 42)'."""
    return re.sub(r' \([^()]*, pid: \d+\)$', '', thread)


def profile(stream, needles, mapper=None):
    mapper = mapper or Mapper([])
    per_thread = collections.Counter()
    per_process = collections.Counter()
    binaries = collections.defaultdict(collections.Counter)
    functions = collections.defaultdict(collections.Counter)
    first = last = None
    total = 0
    for t, thread, process, state, weight, frames in time_profile_rows(stream):
        first = t if first is None else min(first, t)
        last = t if last is None else max(last, t)
        total += weight
        if needles and not any(n.lower() in process.lower() for n in needles):
            continue
        key = (process, thread_label(thread))
        per_thread[key] += weight
        per_process[process] += weight
        b, f = leaf(frames, mapper)
        binaries[key][b] += weight
        functions[key][f'{f} [{b}]'] += weight
    span_s = (last - first) / 1e9 if first is not None else 0
    return dict(span_s=span_s, total_ns=total, per_process=per_process, per_thread=per_thread,
                binaries=binaries, functions=functions)


def report(p, top):
    s = p['span_s'] or 1
    print(f'recording span {p["span_s"]:.1f} s; all processes {p["total_ns"] / 1e6 / s:.0f} CPU ms/s')
    print('\nCPU ms per second, by process')
    for proc, w in p['per_process'].most_common():
        print(f'{w / 1e6 / s:8.1f}  {proc}')
    print('\nbusiest threads (CPU ms per second; where the samples land)')
    for (proc, thread), w in p['per_thread'].most_common(top):
        print(f'{w / 1e6 / s:8.1f}  {thread}  [{proc}]')
        for b, bw in p['binaries'][(proc, thread)].most_common(4):
            print(f'           {bw / w * 100:5.1f}%  {b}')
        for fn, fw in p['functions'][(proc, thread)].most_common(3):
            print(f'                  {fw / w * 100:5.1f}%  {fn[:100]}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('trace')
    ap.add_argument('--process', action='append', help='process name substring (repeatable); default: '
                    + ', '.join(DEFAULT_PROCESSES))
    ap.add_argument('--all', action='store_true', help='every process')
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--vmmap', help='`vmmap -wide PID` output (optionally .gz) to name PE module addresses')
    ap.add_argument('--json', help='also write the per-thread totals here')
    ap.add_argument('--table', choices=('time-profile', 'time-sample'), default='time-profile',
                    help='time-sample: raw samples named through --vmmap only; for Wine processes, where '
                         'exporting time-profile crashes xctrace')
    ap.add_argument('--interval-ms', type=float, default=1.0, help='sampling interval (time-sample only)')
    a = ap.parse_args()
    needles = () if a.all else tuple(a.process or DEFAULT_PROCESSES)
    xc = subprocess.Popen(['xcrun', 'xctrace', 'export', '--input', a.trace, '--xpath',
                           f'/trace-toc/run[@number="1"]/data/table[@schema="{a.table}"]'],
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    vm = a.vmmap if a.vmmap and os.path.exists(a.vmmap) else (a.vmmap + '.gz' if a.vmmap else None)
    mapper = Mapper(read_vmmap(vm), pe_modules(vm))
    if a.table == 'time-sample':
        p = profile_samples(xc.stdout, needles, mapper, int(a.interval_ms * 1e6))
    else:
        p = profile(xc.stdout, needles, mapper)
    if xc.wait() != 0:
        print(f'warning: xctrace export exited {xc.returncode}; the profile is incomplete', file=sys.stderr)
        return 3
    report(p, a.top)
    if a.json:
        s = p['span_s'] or 1
        out = dict(span_s=p['span_s'],
                   processes={k: v / 1e6 / s for k, v in p['per_process'].items()},
                   threads=[dict(process=k[0], thread=k[1], cpu_ms_per_s=v / 1e6 / s,
                                 binaries={b: bw / 1e6 / s for b, bw in p['binaries'][k].most_common(8)},
                                 functions={f: fw / 1e6 / s for f, fw in p['functions'][k].most_common(12)})
                            for k, v in p['per_thread'].most_common()])
        with open(a.json, 'w') as f:
            json.dump(out, f, indent=1)


if __name__ == '__main__':
    sys.exit(main())

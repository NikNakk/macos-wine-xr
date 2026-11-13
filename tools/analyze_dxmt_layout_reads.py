#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tally which DXBC read instructions touch scene-family textures.

Joins DXMT_LAYOUT_AUDIT `shader`/`bind` rows with disassembled shader dumps
(tests/windows/dxbc_disassemble.cpp). Diagnostic only.
"""
import argparse, collections, json, re, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from analyze_dxmt_layout_audit import analyse, fields, STAGES

TRANSLATED = {'ld', 'ld2dms', 'sample', 'sample_l'}          # patch 0015
NEUTRAL = {'resinfo', 'sampleinfo', 'samplepos', 'bufinfo'}  # no coordinates


def shader_reads(path):
    """slot -> Counter of instruction kinds (with '+offset' when a texel offset is used)."""
    reads = collections.defaultdict(collections.Counter)
    facts = collections.Counter()
    for raw in path.read_text(errors='replace').replace('\0', '').splitlines():
        line = re.sub(r'^\s*\d+:\s*', '', raw).strip().lower()
        if not line or line.startswith('//'):
            continue
        op = line.split()[0]
        base = op.split('(')[0]
        if base.startswith('deriv_'):
            facts['derivatives'] += 1
        if base.startswith('dcl_input_ps_siv') and 'position' in line:
            facts['position'] += 1
        if not base.startswith(('sample', 'ld', 'gather4', 'resinfo', 'bufinfo', 'lod')):
            continue
        offset = '_aoffimmi' in base
        kind = base.replace('_aoffimmi', '').replace('_indexable', '')
        if kind.startswith('resinfo'): kind = 'resinfo'
        for slot in re.findall(r'\bt(\d+)\b', line):
            reads[int(slot)][kind + ('+offset' if offset else '')] += 1
    return reads, facts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log'); parser.add_argument('shaders', type=Path)
    parser.add_argument('--frame', type=int, default=1200); parser.add_argument('--json', action='store_true')
    args = parser.parse_args()
    passes, resources, uses, scene = analyse(args.log, args.frame)
    size = scene['size']
    family = {r for r, u in uses.items() if any(x['how'] == 'attachment-write' and x['pass_size'] == size for x in u)}
    for encoder in {x['encoder'] for u in uses.values() for x in u if x['encoder_type'] == 'resolve'}:
        if any(r in family for r, u in uses.items() if any(x['encoder'] == encoder and x['how'] == 'operand-read' for x in u)):
            family |= {r for r, u in uses.items() if any(x['encoder'] == encoder and x['how'] == 'operand-write' for x in u)}
    current, bindings = None, []
    for line in open(args.log, errors='replace'):
        i = line.find('LAYOUT ')
        if i < 0: continue
        kind = line[i + 7:].split(' ', 1)[0]
        f = fields(line[i:])
        if int(f.get('frame', -1)) != args.frame: continue
        if kind == 'shader':
            current = (f['sha1'], int(f['stage']), int(f['encoder']))
        elif kind == 'bind' and f['kind'] == 'srv' and f['resource'] in family and current:
            assert current[1] == int(f['stage']) and current[2] == int(f['encoder']), line
            bindings.append((current[0], int(f['stage']), int(f['encoder']), int(f['slot']), f['resource']))
    cache = {}
    by_kind, by_shader, missing, unused = collections.Counter(), {}, set(), 0
    examples = collections.defaultdict(set)
    for sha, stage, encoder, slot, resource in bindings:
        path = args.shaders / (sha + '.dxbc.asm')
        if not path.exists():
            missing.add(sha); continue
        if sha not in cache: cache[sha] = shader_reads(path)
        reads, facts = cache[sha]
        kinds = reads.get(slot)
        if not kinds:
            unused += 1; continue
        d = resources[resource]
        where = 'scene-sized' if passes.get(encoder, {}).get('size') == size else ('compute' if STAGES[stage] == 'compute' else 'other-sized')
        for kind in kinds:
            by_kind[(kind, where)] += 1
            examples[kind].add('%s %s s%d' % (sha[:10], d['format'], d['samples']))
        entry = by_shader.setdefault(sha, dict(stage=STAGES[stage], kinds=set(), derivatives=facts['derivatives'] > 0, position=facts['position'] > 0, targets=set()))
        entry['kinds'] |= set(kinds); entry['targets'].add('%s s%d' % (d['format'], d['samples']))
    def status(kind):
        base = kind.replace('+offset', '')
        if base in NEUTRAL: return 'no coordinates'
        if base in TRANSLATED: return 'translated' + (' (offset applied in physical texels)' if kind.endswith('+offset') and base.startswith('sample') else '')
        return 'NOT translated'
    shaders_ok = sum(1 for e in by_shader.values() if all(status(k).startswith(('translated', 'no coord')) for k in e['kinds']))
    report = dict(frame=args.frame, bindings=len(bindings), bindings_slot_unused=unused, shaders_missing=sorted(missing),
                  shaders=len(by_shader), shaders_fully_translated=shaders_ok,
                  shaders_with_derivatives=sum(e['derivatives'] for e in by_shader.values()),
                  shaders_reading_sv_position=sum(e['position'] for e in by_shader.values()),
                  kinds=[dict(kind=k, where=w, bindings=n, status=status(k)) for (k, w), n in sorted(by_kind.items(), key=lambda kv: -kv[1])],
                  untranslated_shaders=[dict(sha1=s, stage=e['stage'], kinds=sorted(k for k in e['kinds'] if status(k) == 'NOT translated'), targets=sorted(e['targets']))
                                        for s, e in sorted(by_shader.items()) if any(status(k) == 'NOT translated' for k in e['kinds'])],
                  offset_sample_shaders=sorted(s for s, e in by_shader.items() if any(k.startswith('sample') and k.endswith('+offset') for k in e['kinds'])))
    if args.json:
        print(json.dumps(report, indent=1)); return
    print('frame %d: %d scene-family shader bindings (%d never read by their shader), %d distinct shaders' % (args.frame, len(bindings), unused, len(by_shader)))
    print('shaders using only translated or coordinate-free reads: %d / %d; explicit derivatives: %d; reading SV_Position: %d' % (shaders_ok, len(by_shader), report['shaders_with_derivatives'], report['shaders_reading_sv_position']))
    for row in report['kinds']:
        print('  %-18s %-12s %4d  %s' % (row['kind'], row['where'], row['bindings'], row['status']))
    print('shaders with untranslated reads:')
    for s in report['untranslated_shaders']:
        print('  %s %s %s on %s' % (s['sha1'][:12], s['stage'], ','.join(s['kinds']), '; '.join(s['targets'])))
    print('shaders using Sample with texel offsets:', len(report['offset_sample_shaders']))
    if missing: print('missing dumps:', len(missing))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Summarise DXMT_LAYOUT_AUDIT rows: who reads and writes the scene-sized targets.

Diagnostic only. It classifies each use of a texture by encoder kind and shader
stage so that uses a rate-map layout could not follow (compute, copies, vertex
stage, writes from passes that would not be mapped) are listed explicitly.
"""
import argparse, collections, json, re

TYPES = {0: 'null', 1: 'render', 2: 'compute', 3: 'blit', 4: 'clear', 5: 'resolve', 6: 'present',
         7: 'spatial-upscale', 8: 'signal', 9: 'temporal-upscale', 10: 'wait', 11: 'timestamp'}
STAGES = {0: 'vertex', 1: 'pixel', 2: 'hull', 3: 'domain', 4: 'geometry', 5: 'compute'}
FORMATS = {10: 'R8Unorm', 25: 'R16Float', 55: 'R32Float', 65: 'RG16Float', 70: 'RGBA8Unorm', 71: 'RGBA8Unorm_sRGB',
           80: 'BGRA8Unorm', 81: 'BGRA8Unorm_sRGB', 90: 'RGB10A2Unorm', 92: 'RG11B10Float', 105: 'RG32Float',
           110: 'RGBA16Unorm', 115: 'RGBA16Float', 125: 'RGBA32Float', 250: 'Depth16Unorm', 252: 'Depth32Float',
           255: 'Depth24Unorm_Stencil8', 260: 'Depth32Float_Stencil8', 261: 'X32_Stencil8'}


def fields(line):
    return dict(re.findall(r'(\w+)=(\S+)', line))


def analyse(path, frame):
    passes, access, binds = {}, [], collections.defaultdict(list)
    for line in open(path, errors='replace'):
        i = line.find('LAYOUT ')
        if i < 0:
            continue
        kind = line[i + 7:].split(' ', 1)[0]
        f = fields(line[i:])
        if int(f['frame']) != frame:
            continue
        if kind == 'pass':
            passes[int(f['encoder'])] = dict(size=(int(f['width']), int(f['height'])), colors=int(f['colors']),
                                             samples=int(f['samples']), depth=int(f['depth']))
        elif kind == 'access':
            access.append(f)
        elif kind == 'bind':
            binds[(int(f['encoder']), f['resource'])].append((int(f['stage']), f['kind']))
    resources, uses = {}, collections.defaultdict(list)
    grouped = collections.defaultdict(list)
    for f in access:
        r = f['resource']
        resources[r] = dict(size=(int(f['width']), int(f['height'])), format=FORMATS.get(int(f['format']), f['format']),
                            samples=int(f['samples']), layers=int(f['layers']), mips=int(f['mips']))
        grouped[(int(f['encoder']), int(f['type']), r)].append((int(f['stage']), int(f['flags'])))
    for (encoder, kind, r), rows in grouped.items():
        bound = binds.get((encoder, r), [])
        name = TYPES.get(kind, str(kind))
        for stage, what in bound:
            uses[r].append(dict(encoder=encoder, encoder_type=name, how=what, stage=STAGES.get(stage, str(stage)),
                                pass_size=passes.get(encoder, {}).get('size')))
        # Accesses beyond the shader bindings are attachments (render) or the encoder's own operands.
        extra = sorted(rows, key=lambda x: -x[1])[:max(0, len(rows) - len(bound))]
        if extra:
            flags = 0
            for _, value in extra:
                flags |= value
            how = ('attachment' if name == 'render' else 'operand') + ('-write' if flags & 2 else '-read')
            uses[r].append(dict(encoder=encoder, encoder_type=name, how=how, stage=None,
                                pass_size=passes.get(encoder, {}).get('size')))
    scene = max((p for p in passes.values() if p['depth'] and p['samples'] > 1 and p['colors']),
                key=lambda p: p['size'][0] * p['size'][1], default=None)
    return passes, resources, uses, scene


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log')
    parser.add_argument('--frame', type=int, default=1200)
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()
    passes, resources, uses, scene = analyse(args.log, args.frame)
    if not scene:
        raise SystemExit('no MSAA colour+depth render pass in frame %d' % args.frame)
    size = scene['size']
    # Family: every texture written as an attachment by a scene-sized render pass, plus resolve destinations of those.
    family = {r for r, u in uses.items() if any(x['how'] == 'attachment-write' and x['pass_size'] == size for x in u)}
    changed = True
    while changed:
        changed = False
        for encoder in {x['encoder'] for u in uses.values() for x in u if x['encoder_type'] == 'resolve'}:
            src = [r for r, u in uses.items() if any(x['encoder'] == encoder and x['how'] == 'operand-read' for x in u)]
            dst = [r for r, u in uses.items() if any(x['encoder'] == encoder and x['how'] == 'operand-write' for x in u)]
            if any(r in family for r in src):
                for r in dst:
                    if r not in family:
                        family.add(r); changed = True
    report = dict(frame=args.frame, scene_size=size, render_passes=len(passes),
                  pass_sizes=collections.Counter('%dx%d s%d c%d%s' % (*p['size'], p['samples'], p['colors'], ' d' if p['depth'] else '')
                                                 for p in passes.values()), family=[])
    totals = collections.Counter()
    for r in sorted(family, key=lambda r: min(x['encoder'] for x in uses[r])):
        d = resources[r]
        classes = collections.Counter()
        for x in uses[r]:
            same = x['pass_size'] == size
            if x['encoder_type'] == 'render':
                if x['how'].startswith('attachment'):
                    key = ('attachment in scene-sized pass' if same else 'ATTACHMENT in other-sized pass %sx%s' % x['pass_size'])
                    if same and (passes[x['encoder']]['colors'] > 1): key = 'ATTACHMENT in scene-sized MRT pass'
                elif x['how'] == 'srv' and x['stage'] == 'pixel':
                    key = 'pixel read in scene-sized pass' if same else 'pixel read in other-sized pass'
                else:
                    key = 'RENDER %s %s' % (x['stage'], x['how'])
            elif x['encoder_type'] in ('resolve', 'clear'):
                key = x['encoder_type']
            else:
                key = '%s %s%s' % (x['encoder_type'].upper(), x['how'], ' ' + x['stage'] if x['stage'] else '')
            classes[key] += 1
        totals.update(classes)
        report['family'].append(dict(resource=r, **d, uses=dict(classes)))
    report['totals'] = dict(totals)
    report['pass_sizes'] = dict(report['pass_sizes'])
    # Where family textures go when something other than a render pass or resolve touches them.
    outside = []
    for r in family:
        for x in uses[r]:
            if x['encoder_type'] in ('render', 'resolve', 'clear'):
                continue
            others = [(o, resources[o]) for o, u in uses.items() if o != r and any(y['encoder'] == x['encoder'] for y in u)]
            outside.append(dict(resource=r, **resources[r], encoder=x['encoder'], encoder_type=x['encoder_type'], how=x['how'],
                                with_resources=['%dx%d %s s%d' % (*d['size'], d['format'], d['samples']) for _, d in others][:6]))
    report['outside_render'] = sorted(outside, key=lambda x: x['encoder'])
    if args.json:
        print(json.dumps(report, indent=1)); return
    print('frame %d: %d render passes, scene size %dx%d' % (args.frame, len(passes), *size))
    print('render pass sizes:', ', '.join('%s ×%d' % kv for kv in sorted(report['pass_sizes'].items(), key=lambda kv: -kv[1])))
    print('\nscene-family textures (%d):' % len(family))
    for f in report['family']:
        print(' %s %dx%d %s s%d mips%d' % (f['resource'][-6:], *f['size'], f['format'], f['samples'], f['mips']))
        for key, n in sorted(f['uses'].items()):
            print('    %-44s %d' % (key, n))
    print('\ntotals:')
    for key, n in sorted(totals.items(), key=lambda kv: -kv[1]):
        print('  %-46s %d' % (key, n))
    print('\nuses outside render passes and resolves:')
    for x in report['outside_render']:
        print('  enc %d %s %s: %dx%d %s s%d  with %s' % (x['encoder'], x['encoder_type'], x['how'], *x['size'], x['format'], x['samples'], '; '.join(x['with_resources'])))


if __name__ == '__main__':
    main()

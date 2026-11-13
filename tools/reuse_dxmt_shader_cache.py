#!/usr/bin/env python3
"""Use an existing DXMT build's unchanged Metal helpers in a private Ninja build.

For local experiments when Xcode's downloadable Metal compiler is absent.
This does not rebuild or validate the cached shaders. All changed host/PE code
still compiles normally. Rerun after Meson regenerates build.ninja.
"""
import argparse
from pathlib import Path
import shlex

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--cache-source', type=Path, required=True)
parser.add_argument('--cache-build', type=Path, required=True)
args = parser.parse_args()
source, build = args.source.resolve(), args.build.resolve()
cache_source, cache_build = args.cache_source.resolve(), args.cache_build.resolve()

# Include the local header used by dxmt_command.metal, not just the .metal files.
inputs = ['src/dxmt/dxmt_command.metal', 'src/dxmt/dxmt_command_constants.hpp',
          'src/airconv/shaders/air_msad.metal', 'src/airconv/shaders/air_samplepos.metal',
          'src/airconv/shaders/air_tessellation.metal']
for name in inputs:
    if (source / name).read_bytes() != (cache_source / name).read_bytes():
        raise SystemExit('Refusing shader cache: source differs: ' + name)

ninja = build / 'build.ninja'
lines = ninja.read_text().splitlines()
reused = []
for i, line in enumerate(lines):
    if not line.startswith(' COMMAND = /usr/bin/xcrun -sdk macosx metal'):
        continue
    command = shlex.split(line.split(' = ', 1)[1])
    if command[3] not in ('metal', 'metallib'):
        raise SystemExit('Unexpected compiler command: ' + line)
    target = Path(command[command.index('-o') + 1])
    if target.is_absolute() or '..' in target.parts:
        raise SystemExit('Unexpected output path: ' + str(target))
    if '-c' in command:
        path = (build / command[command.index('-c') + 1]).resolve().relative_to(source)
        if path.as_posix() not in inputs:
            raise SystemExit('Unverified shader input: ' + str(path))
    artifact = cache_build / target
    if not artifact.is_file():
        raise SystemExit('Missing cached shader: ' + str(artifact))
    lines[i] = ' COMMAND = /bin/cp ' + shlex.quote(str(artifact)) + ' ' + shlex.quote(str(target))
    reused.append(str(target))
if not reused:
    raise SystemExit('No Metal compiler commands found; build.ninja left unchanged')
ninja.write_text('\n'.join(lines) + '\n')
print('Reusing {} shader artifacts after source identity checks:'.format(len(reused)))
print('\n'.join(reused))

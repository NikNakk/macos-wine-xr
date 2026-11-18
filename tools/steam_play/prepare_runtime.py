#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Compose a LOCAL compatibility runtime. This output is not a public release artifact."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from managed_stack import load_stack, steam_environment
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'threshold_release'))
from package import relocate, validate_tree


def sha256(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def require(path):
    path = Path(path)
    if not path.is_file():
        raise ValueError(f'Required component is missing: {path}')
    return path


def clone(source, destination):
    if sys.platform == 'darwin':
        subprocess.run(['/bin/cp', '-cR', str(source), str(destination)], check=True)
    else:
        shutil.copytree(source, destination, symlinks=True)


def prepare(stack_path, wine, bridge, windows_loader, stand_in, output, prefixes, relocate_binaries=True, integration=None):
    mode = integration or ('native-steam' if stand_in is not None else 'windows-steam')
    if mode == 'windows-steam':
        if stand_in is not None:
            raise ValueError('The Windows Steam profile does not use a Steam stand-in')
        from public_bridge import module
        return module('prepare_runtime').prepare(stack_path, wine, bridge, windows_loader, output,
                                                Path(prefixes) / 'windows-steam')
    if mode != 'native-steam' or stand_in is None:
        raise ValueError('Private native Steam requires --integration native-steam and --stand-in')
    stack = load_stack(stack_path)
    stack.pop('windows_xr', None)
    if output.exists():
        raise ValueError(f'Refusing to overwrite an existing runtime: {output}')
    wine, bridge = Path(wine).resolve(), Path(bridge).resolve()
    for relative in ('bin/wine', 'bin/wineserver', 'lib/wine/x86_64-unix/ntdll.so',
                     'lib/wine/x86_64-windows/ntdll.dll', 'lib/wine/i386-windows/ntdll.dll',
                     'lib/wine/x86_64-unix/lsteamclient.so', 'lib/wine/x86_64-windows/lsteamclient.dll',
                     'lib/wine/i386-windows/lsteamclient.dll'):
        require(wine / relative)
    # Both redirect sides must exist in the supplied runtime, before copying anything.
    for arch, marker in [('x86_64', 'MWXR_LSTEAMCLIENT'), ('i386', 'steamclient')]:
        if marker.encode('utf-16-le') not in (wine / f'lib/wine/{arch}-windows/ntdll.dll').read_bytes():
            raise ValueError(f'Supplied Wine {arch} ntdll lacks the required Steam redirect')
    for relative in ('x86_64-unix/wineopenxr.so', 'x86_64-windows/wineopenxr.dll'):
        require(bridge / relative)
    require(windows_loader)
    require(stand_in)
    artifacts = stack.get('artifacts', {})
    required = ['monado/macos-x86_64', 'xrizer/windows-x86_64', 'dxmt/macos-x86_64', 'dxmt/windows-x86_64']
    if any(key not in artifacts for key in required):
        raise ValueError('Stack needs Monado x86_64, Windows xrizer and both DXMT artifacts')
    monado = require(Path(artifacts['monado/macos-x86_64']) / 'share/openxr/1/openxr_monado.json')
    xrizer = Path(artifacts['xrizer/windows-x86_64'])
    for relative in ['openvr_api.dll', 'bin/vrclient_x64.dll']:
        require(xrizer / relative)
    overlays = [(require(Path(artifacts['dxmt/macos-x86_64']) / 'x86_64-unix/winemetal.so'), 'lib/wine/x86_64-unix/winemetal.so')]
    for name in ['d3d10core.dll', 'd3d11.dll', 'dxgi.dll', 'winemetal.dll']:
        overlays.append((require(Path(artifacts['dxmt/windows-x86_64']) / 'x86_64-windows' / name), 'lib/wine/x86_64-windows/' + name))
    validate_tree(wine)
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix='.prepare-', dir=output.parent))
    try:
        runtime = staging / 'wine'
        clone(wine, runtime)
        for source, relative in overlays:
            target = runtime / relative
            if target.is_symlink():
                target.unlink()
            shutil.copy2(source, target)
        for relative in ('x86_64-unix/wineopenxr.so', 'x86_64-windows/wineopenxr.dll', 'offscreen_unity_window.exe'):
            source = bridge / relative
            if source.is_file():
                target = runtime / 'bridge' / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
        if (bridge / 'lib').is_dir():
            clone(bridge / 'lib', runtime / 'bridge/lib')
        shutil.copy2(stand_in, runtime / 'bin/mwxr-steam.exe')
        shutil.copy2(windows_loader, runtime / 'lib/wine/x86_64-windows/openxr_loader.dll')
        clone(xrizer, runtime / 'openvr')
        shutil.copy2(windows_loader, runtime / 'openvr/openxr_loader.dll')
        validate_tree(runtime)
        if relocate_binaries:
            # The combined root lets the relocator reuse the selected Wine's ntdll,
            # rather than bundling a second copy or referring to a developer build.
            relocate(runtime)
        validate_tree(runtime)
        final_wine = output.resolve() / 'wine'
        env = {
            'MWXR_IN_PROCESS_WINE': str(final_wine), 'MWXR_IN_PROCESS_BUILD': str(final_wine / 'bridge'),
            'MWXR_XRIZER_ROOT': str(final_wine / 'openvr'), 'MWXR_NATIVE_RUNTIME_JSON': str(monado),
            'MWXR_WINDOWS_OPENXR_LOADER': str(final_wine / 'lib/wine/x86_64-windows/openxr_loader.dll'),
            'ALYX_STEAM_STAND_IN': str(final_wine / 'bin/mwxr-steam.exe'),
            'MWXR_IN_PROCESS_PREFIX': str(Path(prefixes).resolve() / '546560'),
            'MWXR_GAME_PREFIXES': str(Path(prefixes).resolve()),
        }
        stack.pop('_path', None)
        stack['steam_play'] = {'integration': 'native-steam', 'environment': env, 'distribution': 'local-external-components',
                              'external_inputs': {str(p): sha256(p) for p in [wine / 'lib/wine/x86_64-unix/ntdll.so',
                                  wine / 'lib/wine/x86_64-windows/ntdll.dll', wine / 'lib/wine/i386-windows/ntdll.dll',
                                  bridge / 'x86_64-unix/wineopenxr.so', bridge / 'x86_64-windows/wineopenxr.dll',
                                  Path(windows_loader), Path(stand_in)]},
                              'build_attestation_verified': False}
        stack['steam_play']['file_hashes'] = {str(final_wine / path.relative_to(runtime)): sha256(path)
                                              for path in runtime.rglob('*') if path.is_file() and not path.is_symlink()}
        stack['steam_play']['file_hashes'][str(monado)] = sha256(monado)
        (staging / 'launch-stack.json').write_text(json.dumps(stack, indent=2) + '\n')
        # No prefix or active Steam installation has been touched. Rename only after all checks pass.
        staging.rename(output)
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    return output / 'launch-stack.json'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('stack', 'wine-runtime', 'bridge-build', 'windows-loader', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--stand-in', type=Path, help='Private native Steam only; preserves legacy explicit stand-in invocations')
    parser.add_argument('--integration', choices=('windows-steam', 'native-steam'), help='Defaults to Windows Steam, or native Steam for legacy --stand-in invocations')
    parser.add_argument('--prefixes', type=Path, default=Path.home() / 'Library/Application Support/Threshold XR/Prefixes')
    args = parser.parse_args()
    print(prepare(args.stack, args.wine_runtime, args.bridge_build, args.windows_loader, args.stand_in, args.output, args.prefixes, integration=args.integration))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        sys.exit(f'Cannot prepare Steam Play: {error}')

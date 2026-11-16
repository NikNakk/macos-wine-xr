#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Install a prebuilt Steam Play package beside existing installations; never restart Steam."""
import argparse
import json
import os
from pathlib import Path
import plistlib
import shlex
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/steam_play'))
from managed_stack import load_stack, steam_environment


def assemble(package, stack_path, destination, games=None, sign=True):
    stack = load_stack(stack_path)
    steam_environment(stack)
    package, destination = Path(package).resolve(), Path(destination).resolve()
    if destination.exists() or destination.is_relative_to(package):
        raise ValueError('Choose a new destination outside the package; existing installations are preserved')
    for name in ['bin/steam-play', 'steam-play/libmwxr_steam_compat_gate.dylib', 'steam-play/steam-compat-gate-scan',
                 'tools/steam_play/managed_runner.py', 'steam-play/compatibilitytools.d/macos-wine-xr-proton/mwxr-steam-play']:
        if not (package / name).is_file():
            raise ValueError(f'Incomplete prebuilt Steam Play package: {name}')
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix='.steam-play-install-', dir=destination.parent))
    try:
        shutil.copytree(package, stage / 'bridge', symlinks=True)
        shutil.copytree(package / 'steam-play', stage / 'payload', symlinks=True)
        # Use a snapshot: changing the desktop selection cannot silently retarget running Steam.
        stack.pop('_path', None)
        (stage / 'launch-stack.json').write_text(json.dumps(stack, indent=2) + '\n')
        runner = stage / 'runner'
        runner.write_text('#!/bin/sh\nexec /usr/bin/python3 ' + shlex.quote(str(destination / 'bridge/tools/steam_play/managed_runner.py')) +
                          ' --stack ' + shlex.quote(str(destination / 'launch-stack.json')) + ' "$@"\n')
        runner.chmod(0o755)
        (stage / 'payload/compatibilitytools.d/macos-wine-xr-proton/runner').write_text(str(destination / 'runner') + '\n')
        if games:
            shutil.copy2(games, stage / 'payload/games')
        else:
            (stage / 'payload/games').write_text('')
        app = stage / 'Threshold Steam Play.app'
        (app / 'Contents/MacOS').mkdir(parents=True)
        (app / 'Contents/Info.plist').write_bytes(plistlib.dumps({
            'CFBundleIdentifier': 'com.nickkennedy.threshold-steam-play', 'CFBundleName': 'Threshold Steam Play',
            'CFBundleExecutable': 'steam-play', 'CFBundlePackageType': 'APPL', 'CFBundleVersion': '1',
        }))
        launcher = app / 'Contents/MacOS/steam-play'
        launcher.write_text('#!/bin/sh\nexport MWXR_STEAM_PLAY_PAYLOAD=' + shlex.quote(str(destination / 'payload')) +
                            '\nexec ' + shlex.quote(str(destination / 'bridge/bin/steam-play')) + ' "$@"\n')
        launcher.chmod(0o755)
        if sign:
            subprocess.run(['codesign', '--force', '--sign', '-', str(app)], check=True)
        stage.rename(destination)
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    return destination / 'Threshold Steam Play.app'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['package', 'stack', 'destination']:
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--games', type=Path, help='optional existing game preferences to copy (original is kept)')
    args = parser.parse_args()
    print(assemble(args.package, args.stack, args.destination, args.games))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        sys.exit(f'Cannot install Steam Play: {error}')

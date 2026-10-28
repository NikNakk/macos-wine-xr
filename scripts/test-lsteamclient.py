#!/usr/bin/env python3
"""Probe both Steam bridge architectures in an explicitly selected test prefix.

Requires native Steam running and an installed, owned app ID. Reads identity,
requests stats and player count; does not change achievements or user stats.
Exit 3 means connected but callback/online verification was incomplete.
"""
import argparse
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--prefix', type=Path, required=True)
    parser.add_argument('--wine-source', type=Path, default=Path.home() / 'Code/wine-crossover/sources/wine')
    parser.add_argument('--steam-dir', type=Path, required=True, help='Windows Steam directory holding both steamclient DLLs')
    parser.add_argument('--app-id', required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    runtime, prefix, out = args.runtime.resolve(), args.prefix.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update(WINEPREFIX=str(prefix), WINEARCH='win64',
               WINEDEBUG='-all,+steamclient,+module', WINEDLLOVERRIDES='mscoree,mshtml=',
               WINESERVER=str(runtime / 'bin/wineserver'),
               DYLD_LIBRARY_PATH=str(runtime / 'lib'),
               MWXR_LSTEAMCLIENT='1', SteamAppId=args.app_id, SteamGameId=args.app_id,
               STEAM_COMPAT_CLIENT_INSTALL_PATH=str(Path.home() / 'Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS'))
    failed = False
    for arch, filename in [('i686', 'steamclient.dll'), ('x86_64', 'steamclient64.dll')]:
        exe = out / f'steam-{arch}.exe'
        subprocess.run([f'{arch}-w64-mingw32-g++', '-O2', '-static',
                        '-I' + str(args.wine_source / 'dlls/lsteamclient/steamworks_sdk_147'),
                        str(repo / 'tests/windows/steam_bridge_probe.cpp'), '-o', str(exe)], check=True)
        dll = args.steam_dir.resolve() / filename
        if not dll.is_file():
            parser.error(f'missing {dll}')
        with (out / f'{arch}.log').open('w') as log:
            try:
                result = subprocess.run([str(runtime / 'bin/wine'), str(exe),
                                         'Z:' + str(dll).replace('/', '\\')],
                                        env=env, stdout=log, stderr=subprocess.STDOUT, timeout=45)
                print(f'{arch}: exit {result.returncode}; {out / (arch + ".log")}', flush=True)
                failed |= result.returncode != 0
            except subprocess.TimeoutExpired:
                print(f'{arch}: timed out; see {out / (arch + ".log")}', flush=True)
                failed = True
            finally:
                subprocess.run([str(runtime / 'bin/wineserver'), '-k'], env=env, check=True)
                subprocess.run([str(runtime / 'bin/wineserver'), '-w'], env=env, check=True, timeout=15)
    return int(failed)


if __name__ == '__main__':
    raise SystemExit(main())

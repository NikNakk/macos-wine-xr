#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Whether a Steam game needs the WoW64 runtime: prints 32, 64 or unknown.

A game needs WoW64 when any of its Windows executables is 32-bit: the exe Steam is
starting (--exe), or any Windows launch entry in Valve's app info found under the
install folder (--install-path). Assetto Corsa's launcher is 32-bit and its race engine
64-bit; checking every entry gives the setup call (which has no exe) and the launch
the same answer, so both use the same prefix. Reads macOS Steam's offline app cache.

Usage: game_arch.py --appid N [--install-path DIR] [--exe FILE]
"""
import argparse
from pathlib import Path
import sys

here = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(here / 'steam_games'))
import check_launch  # noqa: E402

I386, AMD64 = 0x14c, 0x8664


def machine(path):
    try:
        return check_launch.pe_machine(path)
    except (OSError, ValueError):
        return None


def windows_executables(appid, install_path):
    import steam_games
    info = steam_games.read_appinfo().get(appid) or {}
    launch = (info.get('config', {}) or {}).get('launch', {}) or {}
    for entry in launch.values() if isinstance(launch, dict) else []:
        oslist = str((entry.get('config', {}) or {}).get('oslist', ''))
        if oslist and 'windows' not in oslist.split(','):
            continue
        exe = str(entry.get('executable', '')).replace('\\', '/')
        if exe:
            yield Path(install_path) / exe


def arch(appid, install_path=None, exe=None):
    machines = []
    if exe:
        machines.append(machine(exe))
    if install_path:
        machines += [machine(p) for p in windows_executables(appid, install_path) if p.is_file()]
    if I386 in machines:
        return '32'
    if AMD64 in machines:
        return '64'
    return 'unknown'


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--appid', type=int, required=True)
    p.add_argument('--install-path')
    p.add_argument('--exe')
    a = p.parse_args()
    print(arch(a.appid, a.install_path, a.exe))


if __name__ == '__main__':
    main()

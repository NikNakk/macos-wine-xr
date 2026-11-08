#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Which games macOS Steam maps to the Steam Play tool (docs/steam-play.md, Choosing games).

Mapped: every game (or app or demo) in your library that has a Windows build and no macOS build, every VR
game with a Windows build (macOS builds have no VR), every game whose macOS build is
32-bit only (no "macos64" in its app info; macOS has not run 32-bit apps since 10.15) and
that has a Windows build, plus the app IDs listed in the
payload's "games" file, minus those listed there as -APPID. Mapping a game that also has
a macOS build makes Steam install its Windows build instead. Reads macOS Steam's
offline caches (appinfo.vdf and the library cache).

  steam_play_games.py sync [--payload DIR] [--output FILE]
                                             write DIR/mapped (or FILE), which Steam Play.app reads
  steam_play_games.py list [--payload DIR]   what is mapped and why
"""
import argparse
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))                       # installed copy
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'steam_games'))  # repository
import steam_games  # noqa: E402

PAYLOAD = Path.home() / 'Library/Application Support/macos-wine-xr/steam-play'


def read_games(path):
    """(listed, excluded) app IDs from a games file: one per line, -APPID to exclude, # comments."""
    listed, excluded = set(), set()
    if path.exists():
        for line in path.read_text().splitlines():
            word = line.split('#', 1)[0].strip()
            if word.startswith('-') and word[1:].isdigit():
                excluded.add(int(word[1:]))
            elif word.isdigit():
                listed.add(int(word))
    return listed, excluded


def oslist(info):
    # Some app info has spaces in the list (" macos").
    return {o.strip() for o in str((info or {}).get('common', {}).get('oslist', '')).split(',')}


# What Steam can install and start: games, and apps and demos (Get Lost With Luke In Nature,
# a VR app, is an "Application"). Not DLC, soundtracks or Steam's "tool" entries.
PLAYABLE_TYPES = {'game', 'application', 'demo'}


def playable(info, appid):
    common = (info or {}).get('common', {})
    return bool(info) and str(common.get('type', '')).lower() in PLAYABLE_TYPES and appid not in steam_games.TOOLS


def windows_only(apps, owned):
    out = set()
    for appid in owned:
        info = apps.get(appid)
        if not playable(info, appid):
            continue
        if 'windows' in oslist(info) and 'macos' not in oslist(info):
            out.add(appid)
    return out


def vr_games(apps, owned):
    """VR games with a Windows build, macOS build or not (steam_games.is_vr: OpenVR, OpenXR
    or VR-only support, or a VR launch entry)."""
    out = set()
    for appid in owned:
        info = apps.get(appid)
        if not playable(info, appid):
            continue
        if 'windows' in oslist(info) and steam_games.is_vr(info):
            out.add(appid)
    return out


def mac_32_bit(apps, owned):
    """Games with a Windows build whose macOS build is 32-bit only: app info's
    common.osextended lacks "macos64", the flag Steam's client checks for 64-bit macOS."""
    out = set()
    for appid in owned:
        info = apps.get(appid)
        common = (info or {}).get('common', {})
        if not playable(info, appid):
            continue
        if {'windows', 'macos'} <= oslist(info) and 'macos64' not in str(common.get('osextended', '')).split(','):
            out.add(appid)
    return out


def owned_apps():
    cache = steam_games.MAC_STEAM / 'appcache/librarycache'
    return {int(d) for d in os.listdir(cache) if d.isdigit()} if cache.is_dir() else set()


def mapping(apps, owned, listed, excluded):
    """{appid: reason} for every mapped game: 'listed', 'vr', 'mac-32-bit' or 'windows-only'."""
    out = {a: 'windows-only' for a in windows_only(apps, owned)}
    out.update({a: 'vr' for a in vr_games(apps, owned) if a not in out})
    out.update({a: 'mac-32-bit' for a in mac_32_bit(apps, owned) if a not in out})
    # A listed game that is mapped anyway keeps its automatic reason, so the Compatibility
    # page can show that its default is already Windows.
    out.update({a: 'listed' for a in listed if a not in out})
    for a in excluded:
        out.pop(a, None)
    return out


def name(apps, appid):
    return str((apps.get(appid) or {}).get('common', {}).get('name', appid))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('command', choices=('sync', 'list'))
    p.add_argument('--payload', type=Path, default=PAYLOAD)
    p.add_argument('--output', type=Path, help='sync: write here instead of DIR/mapped (live mapping)')
    a = p.parse_args()
    apps = steam_games.read_appinfo()
    listed, excluded = read_games(a.payload / 'games')
    mapped = mapping(apps, owned_apps(), listed, excluded)
    if a.command == 'sync':
        lines = [f'{appid}   # {name(apps, appid)} ({why})' for appid, why in sorted(mapped.items())]
        # Automatic games left out by -APPID, as comments: the Compatibility page shows
        # their default (Steam Play.app reads only lines that start with an app ID).
        automatic = mapping(apps, owned_apps(), set(), set())
        lines += [f'#excluded {appid}   # {name(apps, appid)} ({automatic[appid]})'
                  for appid in sorted(excluded) if appid in automatic]
        out = a.output or a.payload / 'mapped'
        tmp = out.with_name(out.name + '.new')
        tmp.write_text('# Written by steam_play_games.py sync; edit "games" instead.\n' + '\n'.join(lines) + '\n')
        tmp.replace(out)
        count = lambda why: sum(w == why for w in mapped.values())  # noqa: E731
        print(f'{len(mapped)} games mapped: {count("listed")} listed, {count("vr")} VR with a macOS build, '
              f'{count("mac-32-bit")} with a 32-bit macOS build, {count("windows-only")} Windows-only, '
              f'{len(excluded)} excluded')
    else:
        for appid, why in sorted(mapped.items(), key=lambda i: name(apps, i[0]).lower()):
            mac = ' (has a macOS build: installs as Windows)' if 'macos' in oslist(apps.get(appid)) else ''
            print(f'{appid:>8}  {why:12}  {name(apps, appid)}{mac}')
        for appid in sorted(excluded):
            print(f'{appid:>8}  {"excluded":12}  {name(apps, appid)}')


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Windows Steam games for the rig's Wine prefix, without Windows Steam (docs/steam-games.md).

usage: steam_games.py info APPID [--json]
       steam_games.py install APPID [--user NAME] [--validate]
       steam_games.py list [--json]
       steam_games.py launch-spec APPID [--mode vr|default] [--json]

info and launch-spec read the game's public app info through SteamCMD signed in
anonymously; install signs in with SteamCMD's cached login (sign in once yourself:
steamcmd.sh +login NAME) and downloads the Windows 64-bit build into the prefix's Steam
library, as Windows Steam would, and keeps the app's launch entries in
~/Library/Application Support/macos-wine-xr/games. list shows the games installed there, by either route.

Environment: STEAMCMD (default ~/steamcmd/steamcmd.sh), MWXR_IN_PROCESS_PREFIX (default
~/Windows/prefix), STEAM_USER (default: the most recent account in macOS Steam).
"""
import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

HOME = Path.home()
STEAMCMD = Path(os.environ.get('STEAMCMD', HOME / 'steamcmd/steamcmd.sh'))
PREFIX = Path(os.environ.get('MWXR_IN_PROCESS_PREFIX', HOME / 'Windows/prefix'))
STEAM_WINDOWS = 'C:\\Program Files (x86)\\Steam'
LIBRARY = PREFIX / 'drive_c/Program Files (x86)/Steam/steamapps'
GAMES = Path(os.environ.get('MWXR_GAMES_DIR', HOME / 'Library/Application Support/macos-wine-xr/games'))


def game_file(appid):
    return GAMES / f'{appid}.json'


def parse_vdf(text):
    """Text KeyValues (VDF) to nested dicts; later duplicate keys win."""
    tokens = re.findall(r'"((?:[^"\\]|\\.)*)"|([{}])', text)
    pos = 0

    def block():
        nonlocal pos
        out = {}
        while pos < len(tokens):
            quoted, brace = tokens[pos]
            pos += 1
            if brace == '}':
                return out
            if brace == '{':
                continue
            key = quoted
            if pos < len(tokens) and tokens[pos][1] == '{':
                pos += 1
                out[key] = block()
            elif pos < len(tokens):
                out[key] = tokens[pos][0].replace('\\\\', '\\')
                pos += 1
        return out

    return block()


def steamcmd(args, interactive=False):
    if not STEAMCMD.exists():
        sys.exit(f'SteamCMD not found at {STEAMCMD} (set STEAMCMD)')
    cmd = [str(STEAMCMD)] + args + ['+quit']
    if interactive:  # Steam Guard or a password prompt reaches the terminal
        return subprocess.run(cmd).returncode
    return subprocess.run(cmd, capture_output=True, text=True, stdin=subprocess.DEVNULL).stdout


def app_info(appid):
    out = steamcmd(['+login', 'anonymous', '+app_info_update', '1', '+app_info_print', str(appid)])
    start = out.find(f'"{appid}"')
    if start < 0:
        sys.exit(f'no app info for {appid}')
    return parse_vdf(out[start:]).get(str(appid), {})


def windows_launches(info):
    """Windows launch entries: dicts with executable, arguments, workingdir, type."""
    entries = []
    for entry in (info.get('config', {}).get('launch', {}) or {}).values():
        oslist = entry.get('config', {}).get('oslist', '')
        if oslist and 'windows' not in oslist.split(','):
            continue
        if entry.get('config', {}).get('osarch', '64') not in ('', '64'):
            continue
        entries.append({k: entry.get(k, '') for k in ('executable', 'arguments', 'workingdir', 'type',
                                                         'description')})
    return entries


def summary(appid, info):
    common = info.get('common', {})
    return dict(appid=int(appid), name=common.get('name', str(appid)),
                installdir=info.get('config', {}).get('installdir', ''), launch=windows_launches(info),
                vr=common.get('openvrsupport') == '1' or common.get('openxrsupport') == '1' or
                any(e['type'] == 'vr' for e in windows_launches(info)))


def steam_user():
    if os.environ.get('STEAM_USER'):
        return os.environ['STEAM_USER']
    users = HOME / 'Library/Application Support/Steam/config/loginusers.vdf'
    if users.exists():
        accounts = parse_vdf(users.read_text(errors='replace')).get('users', {}).values()
        recent = [a for a in accounts if a.get('MostRecent') == '1'] or list(accounts)
        if recent:
            return recent[0].get('AccountName')
    sys.exit('set STEAM_USER to the account SteamCMD is signed in as')


def installed():
    """Games in the prefix's library: installed by Windows Steam or by install below."""
    games = {}
    for manifest in list(LIBRARY.glob('appmanifest_*.acf')) + list(LIBRARY.glob('common/*/steamapps/appmanifest_*.acf')):
        state = parse_vdf(manifest.read_text(errors='replace')).get('AppState', {})
        appid = state.get('appid')
        folder = LIBRARY / 'common' / state.get('installdir', '')
        if not appid or not folder.is_dir() or appid == '228980':  # Steamworks Common Redistributables
            continue
        games[int(appid)] = dict(appid=int(appid), name=state.get('name', appid), installdir=state.get('installdir'),
                                 folder=str(folder), launch_info=game_file(appid).exists())
    return [games[k] for k in sorted(games)]


def game_record(appid):
    for game in installed():
        if game['appid'] == appid:
            folder = Path(game['folder'])
            if game_file(appid).exists():
                return json.loads(game_file(appid).read_text()), folder
            record = summary(appid, app_info(appid))  # installed by Windows Steam: fetch once, keep
            save_record(record)
            return record, folder
    sys.exit(f'app {appid} is not installed in {LIBRARY} (steam_games.py install {appid})')


def save_record(record):
    GAMES.mkdir(parents=True, exist_ok=True)
    game_file(record['appid']).write_text(json.dumps(record, indent=1) + '\n')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('info'); s.add_argument('appid', type=int); s.add_argument('--json', action='store_true')
    s = sub.add_parser('install'); s.add_argument('appid', type=int); s.add_argument('--user')
    s.add_argument('--validate', action='store_true')
    s = sub.add_parser('list'); s.add_argument('--json', action='store_true')
    s = sub.add_parser('launch-spec'); s.add_argument('appid', type=int)
    s.add_argument('--mode', choices=('vr', 'default'), default='vr'); s.add_argument('--json', action='store_true')
    a = p.parse_args()

    if a.cmd == 'info':
        record = summary(a.appid, app_info(a.appid))
        if a.json:
            print(json.dumps(record, indent=1))
        else:
            print(f"{record['name']} ({a.appid}), folder {record['installdir']!r}, VR: {record['vr']}")
            for e in record['launch']:
                print(f"  {e['type'] or 'default':8} {e['executable']} {e['arguments']}".rstrip())
    elif a.cmd == 'list':
        games = installed()
        if a.json:
            print(json.dumps(games, indent=1))
        for g in [] if a.json else games:
            print(f"{g['appid']:>8}  {g['name']}")
    elif a.cmd == 'install':
        record = summary(a.appid, app_info(a.appid))
        if not record['installdir'] or not record['launch']:
            sys.exit(f"{record['name']}: no Windows build in its app info")
        folder = LIBRARY / 'common' / record['installdir']
        folder.mkdir(parents=True, exist_ok=True)
        print(f"Installing {record['name']} ({a.appid}) into {folder}")
        rc = steamcmd(['+@sSteamCmdForcePlatformType', 'windows', '+@sSteamCmdForcePlatformBitness', '64',
                       '+force_install_dir', str(folder), '+login', a.user or steam_user(),
                       '+app_update', str(a.appid)] + (['validate'] if a.validate else []), interactive=True)
        if rc != 0:
            sys.exit(f'SteamCMD failed ({rc})')
        save_record(record)
    elif a.cmd == 'launch-spec':
        record, folder = game_record(a.appid)
        entries = record['launch']
        chosen = (next((e for e in entries if e['type'] == a.mode), None) or
                  next((e for e in entries if e['type'] in ('default', '')), None) or (entries or [None])[0])
        if not chosen:
            sys.exit(f"{record['name']}: no Windows launch entry")
        exe = folder / chosen['executable'].replace('\\', '/')
        workdir = folder / chosen['workingdir'].replace('\\', '/') if chosen['workingdir'] else exe.parent
        spec = dict(appid=a.appid, name=record['name'], exe=str(exe), arguments=chosen['arguments'],
                    workdir=str(workdir), type=chosen['type'] or 'default', vr=record['vr'])
        if a.json:
            print(json.dumps(spec))
        else:
            print(f"{spec['exe']}\n{spec['arguments']}\n{spec['workdir']}")


if __name__ == '__main__':
    main()

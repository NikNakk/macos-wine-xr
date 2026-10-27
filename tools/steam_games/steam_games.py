#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Windows Steam games for Wine, without Windows Steam (docs/steam-games.md).

usage: steam_games.py library [--all] [--json]
       steam_games.py list [--json]
       steam_games.py info APPID [--json]
       steam_games.py install APPID [--user NAME] [--validate] [--progress]
       steam_games.py launch-spec APPID [--mode vr|default] [--json]

library   the games in your Steam library with a Windows build (VR ones unless --all),
          read offline from macOS Steam's own caches (appcache/appinfo.vdf and the
          library artwork cache): no sign-in
list      the games installed in the shared library (~/Windows/library) or the rig
          prefix's Steam library
info      a game's Windows launch entries, from Valve's public app info (anonymous
          SteamCMD)
install   the Windows 64-bit build into the shared library with SteamCMD's cached login
          (sign in once yourself: steamcmd.sh +login NAME); --progress prints one JSON
          object per line (state, done and total bytes, then the result)
launch-spec  the executable, arguments and working folder for a launch

Environment: STEAMCMD (default ~/steamcmd/steamcmd.sh), MWXR_GAME_LIBRARY (default
~/Windows/library), MWXR_IN_PROCESS_PREFIX (the rig prefix, default ~/Windows/prefix),
STEAM_USER (default: the most recent account in macOS Steam).
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

HOME = Path.home()
STEAMCMD = Path(os.environ.get('STEAMCMD', HOME / 'steamcmd/steamcmd.sh'))
MAC_STEAM = HOME / 'Library/Application Support/Steam'
SHARED = Path(os.environ.get('MWXR_GAME_LIBRARY', HOME / 'Windows/library')) / 'steamapps'
RIG_PREFIX = Path(os.environ.get('MWXR_IN_PROCESS_PREFIX', HOME / 'Windows/prefix'))
RIG = RIG_PREFIX / 'drive_c/Program Files (x86)/Steam/steamapps'
LIBRARIES = (('shared', SHARED), ('rig-prefix', RIG))
GAMES = Path(os.environ.get('MWXR_GAMES_DIR', HOME / 'Library/Application Support/macos-wine-xr/games'))
TOOLS = {228980, 250820}  # Steamworks Common Redistributables, SteamVR


# -- text KeyValues (SteamCMD output, .acf, .vdf) --------------------------------------------

def parse_vdf(text):
    """Text KeyValues to nested dicts; later duplicate keys win."""
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
            if pos < len(tokens) and tokens[pos][1] == '{':
                pos += 1
                out[quoted] = block()
            elif pos < len(tokens):
                out[quoted] = tokens[pos][0].replace('\\\\', '\\')
                pos += 1
        return out

    return block()


# -- macOS Steam's binary app info cache (appcache/appinfo.vdf) -------------------------------

def read_appinfo(path=MAC_STEAM / 'appcache/appinfo.vdf'):
    """{appid: appinfo} from appinfo.vdf, format 29 (binary KeyValues with a string table)."""
    data = path.read_bytes()
    magic, _universe = struct.unpack_from('<II', data, 0)
    if magic != 0x07564429:
        raise ValueError(f'unsupported appinfo.vdf format {magic:#x}')
    (table,) = struct.unpack_from('<q', data, 8)
    (count,) = struct.unpack_from('<I', data, table)
    strings, pos = [], table + 4
    for _ in range(count):
        end = data.index(b'\0', pos)
        strings.append(data[pos:end].decode('utf-8', 'replace'))
        pos = end + 1

    def kv(pos):
        out = {}
        while True:
            kind = data[pos]
            pos += 1
            if kind == 0x08:
                return out, pos
            (index,) = struct.unpack_from('<i', data, pos)
            key, pos = strings[index], pos + 4
            if kind == 0x00:
                out[key], pos = kv(pos)
            elif kind == 0x01:
                end = data.index(b'\0', pos)
                out[key], pos = data[pos:end].decode('utf-8', 'replace'), end + 1
            elif kind in (0x02, 0x03):
                (out[key],) = struct.unpack_from('<i' if kind == 0x02 else '<f', data, pos)
                pos += 4
            elif kind == 0x07:
                (out[key],) = struct.unpack_from('<Q', data, pos)
                pos += 8
            else:
                raise ValueError(f'unknown KeyValues type {kind:#x}')

    apps, pos = {}, 16
    while pos < table:
        appid, size = struct.unpack_from('<II', data, pos)
        if appid == 0:
            break
        # infostate, last updated, PICS token, SHA-1, change number, binary SHA-1
        body, _ = kv(pos + 8 + 4 + 4 + 8 + 20 + 4 + 20)
        apps[appid] = body.get('appinfo', body)
        pos += 8 + size
    return apps


def is_vr(info):
    common = info.get('common', {})
    return any(str(common.get(k, '')) == '1' for k in ('openvrsupport', 'openxrsupport', 'onlyvrsupport')) or \
        any(e.get('type') == 'vr' for e in windows_launches(info))


# -- SteamCMD ----------------------------------------------------------------------------------

def steamcmd(args, mode='capture'):
    if not STEAMCMD.exists():
        sys.exit(f'SteamCMD not found at {STEAMCMD} (set STEAMCMD)')
    cmd = [str(STEAMCMD)] + args + ['+quit']
    if mode == 'interactive':  # a password or Steam Guard prompt reaches the terminal
        return subprocess.run(cmd).returncode
    if mode == 'stream':
        return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                text=True, bufsize=1)
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
    launch = info.get('config', {}).get('launch', {}) or {}
    for entry in launch.values() if isinstance(launch, dict) else []:
        config = entry.get('config', {}) or {}
        oslist = config.get('oslist', '')
        if oslist and 'windows' not in oslist.split(','):
            continue
        if str(config.get('osarch', '64')) not in ('', '64'):
            continue
        entries.append({k: str(entry.get(k, '')) for k in ('executable', 'arguments', 'workingdir', 'type',
                                                              'description')})
    return entries


def summary(appid, info):
    common = info.get('common', {})
    return dict(appid=int(appid), name=common.get('name', str(appid)),
                installdir=info.get('config', {}).get('installdir', ''), launch=windows_launches(info),
                vr=is_vr(info))


def steam_user():
    if os.environ.get('STEAM_USER'):
        return os.environ['STEAM_USER']
    users = MAC_STEAM / 'config/loginusers.vdf'
    if users.exists():
        accounts = list(parse_vdf(users.read_text(errors='replace')).get('users', {}).values())
        recent = [a for a in accounts if a.get('MostRecent') == '1'] or accounts
        if recent:
            return recent[0].get('AccountName')
    sys.exit('set STEAM_USER to the account SteamCMD is signed in as')


# -- installed games and saved launch entries -----------------------------------------------------

def game_file(appid):
    return GAMES / f'{appid}.json'


def save_record(record):
    GAMES.mkdir(parents=True, exist_ok=True)
    game_file(record['appid']).write_text(json.dumps(record, indent=1) + '\n')


def installed():
    """Installed games: the shared library first, then the rig prefix's Steam library."""
    games = {}
    for library, root in LIBRARIES:
        for manifest in list(root.glob('appmanifest_*.acf')) + list(root.glob('common/*/steamapps/appmanifest_*.acf')):
            state = parse_vdf(manifest.read_text(errors='replace')).get('AppState', {})
            appid = int(state.get('appid', 0) or 0)
            folder = root / 'common' / state.get('installdir', '')
            if not appid or appid in TOOLS or appid in games or not folder.is_dir():
                continue
            games[appid] = dict(appid=appid, name=state.get('name', str(appid)), installdir=state.get('installdir'),
                                folder=str(folder), library=library, launch_info=game_file(appid).exists())
    return [games[k] for k in sorted(games)]


def game_record(appid):
    for game in installed():
        if game['appid'] == appid:
            if game_file(appid).exists():
                record = json.loads(game_file(appid).read_text())
            else:  # installed some other way: fetch once and keep
                record = summary(appid, app_info(appid))
                save_record(record)
            return record, game
    sys.exit(f'app {appid} is not installed (steam_games.py install {appid})')


def library(all_games=False):
    apps = read_appinfo()
    owned = {int(d) for d in os.listdir(MAC_STEAM / 'appcache/librarycache') if d.isdigit()}
    local = {g['appid']: g for g in installed()}
    out = []
    for appid in sorted(owned):
        info = apps.get(appid)
        common = (info or {}).get('common', {})
        if not info or str(common.get('type', '')).lower() != 'game' or appid in TOOLS:
            continue
        if 'windows' not in str(common.get('oslist', '')).split(','):
            continue
        vr = is_vr(info)
        if not (vr or all_games):
            continue
        art = MAC_STEAM / 'appcache/librarycache' / str(appid)
        game = local.get(appid)
        out.append(dict(appid=appid, name=common.get('name', str(appid)), vr=vr,
                        installed=bool(game), library=game['library'] if game else None,
                        folder=game['folder'] if game else None,
                        cover=str(art / 'library_600x900.jpg') if (art / 'library_600x900.jpg').exists() else None,
                        hero=str(art / 'library_hero.jpg') if (art / 'library_hero.jpg').exists() else None,
                        native_macos='macos' in str(common.get('oslist', '')).split(',')))
    return sorted(out, key=lambda g: g['name'].lower())


PROGRESS = re.compile(r'Update state \((0x[0-9a-f]+)\) ([a-z ]+), progress: ([0-9.]+) \((\d+) / (\d+)\)')


def install(appid, user, validate, progress):
    record = summary(appid, app_info(appid))
    if not record['installdir'] or not record['launch']:
        sys.exit(f"{record['name']}: no Windows build in its app info")
    folder = SHARED / 'common' / record['installdir']
    folder.mkdir(parents=True, exist_ok=True)
    args = ['+@sSteamCmdForcePlatformType', 'windows', '+@sSteamCmdForcePlatformBitness', '64',
            '+force_install_dir', str(folder), '+login', user or steam_user(),
            '+app_update', str(appid)] + (['validate'] if validate else [])
    if not progress:
        print(f"Installing {record['name']} ({appid}) into {folder}")
        if steamcmd(args, 'interactive') != 0:
            sys.exit('SteamCMD failed')
        save_record(record)
        return
    emit = lambda **kv: print(json.dumps(dict(appid=appid, **kv)), flush=True)
    emit(event='start', name=record['name'], folder=str(folder))
    proc = steamcmd(args, 'stream')
    result, message = 'error', 'SteamCMD exited without reporting success'
    for line in proc.stdout:
        line = line.strip()
        if m := PROGRESS.search(line):
            emit(event='progress', state=m.group(2).strip(), percent=float(m.group(3)),
                 done=int(m.group(4)), total=int(m.group(5)))
        elif 'fully installed' in line:
            result, message = 'ok', line
        elif line.startswith('ERROR') or 'FAILED' in line or 'Invalid Password' in line or 'Steam Guard' in line:
            message = line
    proc.wait()
    if result == 'ok':
        save_record(record)
    emit(event='result', result=result, message=message)
    sys.exit(0 if result == 'ok' else 1)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('library'); s.add_argument('--all', action='store_true'); s.add_argument('--json', action='store_true')
    s = sub.add_parser('list'); s.add_argument('--json', action='store_true')
    s = sub.add_parser('info'); s.add_argument('appid', type=int); s.add_argument('--json', action='store_true')
    s = sub.add_parser('install'); s.add_argument('appid', type=int); s.add_argument('--user')
    s.add_argument('--validate', action='store_true'); s.add_argument('--progress', action='store_true')
    s = sub.add_parser('launch-spec'); s.add_argument('appid', type=int)
    s.add_argument('--mode', choices=('vr', 'default'), default='vr'); s.add_argument('--json', action='store_true')
    a = p.parse_args()

    if a.cmd == 'library':
        games = library(a.all)
        if a.json:
            print(json.dumps(games, indent=1))
        for g in [] if a.json else games:
            print(f"{g['appid']:>8}  {'installed' if g['installed'] else '         '}  {g['name']}")
    elif a.cmd == 'list':
        games = installed()
        if a.json:
            print(json.dumps(games, indent=1))
        for g in [] if a.json else games:
            print(f"{g['appid']:>8}  {g['library']:10}  {g['name']}")
    elif a.cmd == 'info':
        record = summary(a.appid, app_info(a.appid))
        if a.json:
            print(json.dumps(record, indent=1))
        else:
            print(f"{record['name']} ({a.appid}), folder {record['installdir']!r}, VR: {record['vr']}")
            for e in record['launch']:
                print(f"  {e['type'] or 'default':8} {e['executable']} {e['arguments']}".rstrip())
    elif a.cmd == 'install':
        install(a.appid, a.user, a.validate, a.progress)
    elif a.cmd == 'launch-spec':
        record, game = game_record(a.appid)
        entries = record['launch']
        chosen = (next((e for e in entries if e['type'] == a.mode), None) or
                  next((e for e in entries if e['type'] in ('default', '')), None) or (entries or [None])[0])
        if not chosen:
            sys.exit(f"{record['name']}: no Windows launch entry")
        folder = Path(game['folder'])
        exe = folder / chosen['executable'].replace('\\', '/')
        workdir = folder / chosen['workingdir'].replace('\\', '/') if chosen['workingdir'] else exe.parent
        spec = dict(appid=a.appid, name=record['name'], exe=str(exe), arguments=chosen['arguments'],
                    workdir=str(workdir), type=chosen['type'] or 'default', vr=record['vr'],
                    library=game['library'])
        if a.json:
            print(json.dumps(spec))
        else:
            print(f"{spec['exe']}\n{spec['arguments']}\n{spec['workdir']}")


if __name__ == '__main__':
    main()

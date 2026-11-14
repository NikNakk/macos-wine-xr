#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
"""UEVR settings shared by every game prefix (docs/uevr.md).

UEVR keeps a game's settings in %APPDATA%\\UnrealVRMod\\<exe name>, inside the game's
prefix. Here every prefix's UnrealVRMod is a link to one folder on the Mac
(MWXR_UEVR_PROFILES, default ~/Windows/uevr/profiles), so settings survive a change of
prefix (WoW64 or not) and community profiles have one place to go.

  uevr_profile.py link PREFIX           link each Wine user's UnrealVRMod in PREFIX to the shared
                                        folder, moving settings already there into it first
  uevr_profile.py folder APPID          the game's settings folder (from macOS Steam's install)
  uevr_profile.py import APPID ZIP      unpack a UEVR profile zip there; the old folder is kept
                                        as a backup
  uevr_profile.py list                  the games with settings

--exe NAME (folder, import) names the process UEVR runs in when it is not the game's
Windows launch entry, as MWXR_UEVR_EXE does for the launch.
"""
import argparse
import os
from pathlib import Path
import shutil
import sys
import tempfile
import time
import zipfile

here = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(here / 'steam_games'))

HOME = Path.home()
PROFILES = Path(os.environ.get('MWXR_UEVR_PROFILES', HOME / 'Windows/uevr/profiles'))
BACKUPS = PROFILES.parent / 'profile-backups'
MAC_STEAM = HOME / 'Library/Application Support/Steam'
# Files a UEVR profile folder holds; a zip's folder with one of these is the profile.
PROFILE_MARKERS = ('config.txt', 'cvars_standard.txt', 'cvars_data.txt', 'scripts', 'plugins', 'uobjecthook')


def stamp():
    return time.strftime('%Y%m%d-%H%M%S')


def backup(path, label):
    BACKUPS.mkdir(parents=True, exist_ok=True)
    target = BACKUPS / f'{path.name}-{label}-{stamp()}'
    shutil.move(str(path), str(target))
    return target


# --- link ------------------------------------------------------------------------------

def link(prefix):
    users = Path(prefix) / 'drive_c/users'
    PROFILES.mkdir(parents=True, exist_ok=True)
    for user in sorted(users.iterdir()) if users.is_dir() else []:
        # steamuser is a link to the Wine user (docs/steam-play.md, Steam Cloud).
        if user.name in ('Public', 'steamuser') or user.is_symlink() or not user.is_dir():
            continue
        roaming = user / 'AppData/Roaming'
        mod = roaming / 'UnrealVRMod'
        if mod.is_symlink():
            if mod.resolve() == PROFILES.resolve():
                continue
            mod.unlink()
        elif mod.is_dir():
            for game in sorted(mod.iterdir()):
                shared = PROFILES / game.name
                if shared.exists():
                    # The shared folder wins; the prefix's copy is kept, not merged.
                    print(f'{game.name}: shared settings kept; prefix copy saved as {backup(game, "prefix")}')
                else:
                    shutil.move(str(game), str(shared))
                    print(f'{game.name}: moved from {prefix} to {shared}')
            mod.rmdir()
        roaming.mkdir(parents=True, exist_ok=True)
        mod.symlink_to(PROFILES)
        print(f'{mod} -> {PROFILES}')


# --- the game's exe ----------------------------------------------------------------------

def library_roots():
    import steam_games
    roots = [MAC_STEAM / 'steamapps']
    vdf = MAC_STEAM / 'steamapps/libraryfolders.vdf'
    if vdf.is_file():
        folders = steam_games.parse_vdf(vdf.read_text(errors='replace')).get('libraryfolders', {})
        for entry in folders.values() if isinstance(folders, dict) else []:
            path = Path(str(entry.get('path', ''))) / 'steamapps' if isinstance(entry, dict) else None
            if path and path not in roots:
                roots.append(path)
    return roots


def install_folder(appid):
    import steam_games
    for root in library_roots():
        manifest = root / f'appmanifest_{appid}.acf'
        if manifest.is_file():
            state = steam_games.parse_vdf(manifest.read_text(errors='replace')).get('AppState', {})
            folder = root / 'common' / state.get('installdir', '')
            if state.get('installdir') and folder.is_dir():
                return folder
    sys.exit(f'app {appid} is not installed in macOS Steam')


def exe_stem(appid, exe=None):
    """UEVR's folder name: the game process's file name without .exe."""
    if exe:
        return Path(exe.replace('\\', '/')).stem
    import game_arch
    folder = install_folder(appid)
    entries = [p for p in game_arch.windows_executables(appid, folder) if p.is_file()]
    if not entries:
        sys.exit(f'app {appid}: no Windows launch entry found in {folder}; give --exe')
    exe = entries[0]
    # An Unreal bootstrap exe at the top of the folder starts <Project>/Binaries/Win64's,
    # which is the process UEVR runs in.
    if exe.parent == folder:
        shipping = sorted(folder.glob('*/Binaries/Win64/*.exe'))
        shipping = [p for p in shipping if 'CrashReport' not in p.name]
        if len(shipping) == 1:
            exe = shipping[0]
    return exe.stem


# --- import -------------------------------------------------------------------------------

def profile_root(tree):
    """The folder in an unpacked zip that holds the profile: the top, or its only folder."""
    candidates = [tree] + sorted(p for p in tree.rglob('*') if p.is_dir())
    for folder in candidates:
        if any((folder / m).exists() for m in PROFILE_MARKERS):
            return folder
    sys.exit('the zip has no UEVR profile (no config.txt, cvars, scripts or plugins)')


def safe_extract(archive, target):
    with zipfile.ZipFile(archive) as z:
        for member in z.infolist():
            path = (target / member.filename).resolve()
            if not str(path).startswith(str(target.resolve()) + os.sep) and path != target.resolve():
                sys.exit(f'refusing {member.filename!r}: outside the profile folder')
        z.extractall(target)


def import_profile(appid, archive, exe=None):
    name = exe_stem(appid, exe)
    dest = PROFILES / name
    with tempfile.TemporaryDirectory() as tmp:
        tree = Path(tmp) / 'zip'
        tree.mkdir()
        safe_extract(Path(archive), tree)
        source = profile_root(tree)
        PROFILES.mkdir(parents=True, exist_ok=True)
        if dest.exists():
            print(f'previous settings saved as {backup(dest, "before-import")}')
        shutil.copytree(source, dest)
    # The runner writes the runtime choice at each injection (MWXR_UEVR_RUNTIME).
    print(f'{name}: profile from {archive} in {dest}')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='command', required=True)
    sub.add_parser('link').add_argument('prefix')
    f = sub.add_parser('folder')
    f.add_argument('appid', type=int)
    f.add_argument('--exe')
    i = sub.add_parser('import')
    i.add_argument('appid', type=int)
    i.add_argument('zip')
    i.add_argument('--exe')
    sub.add_parser('list')
    a = p.parse_args()
    if a.command == 'link':
        link(a.prefix)
    elif a.command == 'folder':
        print(PROFILES / exe_stem(a.appid, a.exe))
    elif a.command == 'import':
        import_profile(a.appid, a.zip, a.exe)
    else:
        for game in sorted(PROFILES.iterdir()) if PROFILES.is_dir() else []:
            if game.is_dir():
                print(game.name)


if __name__ == '__main__':
    main()

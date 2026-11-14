#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Keep a Wine prefix's built-in DLLs as APFS clones of the runtime's (docs/steam-games.md).

A prefix holds a copy of every Wine built-in it may load (system32 for 64-bit,
syswow64 for 32-bit, and their drivers folders): about 1.7 GB with WoW64. Wine loads
built-ins from the runtime; the copies are there so programs find the files. A prefix
cloned from the template shares those blocks, but Wine's own prefix update (after any
runtime change) writes them as new files, so every prefix ends up with a private copy.

This replaces each file that carries Wine's built-in marker ("Wine builtin DLL" in its
DOS stub) and has the same name in the runtime with a clone of the runtime's file, so
the blocks are shared again. Files without the marker are left alone: native DLLs a game
or installer put there (Microsoft's .NET, Visual C++ runtimes). A clone keeps the
runtime file's size and time, so a later run skips it without reading it.

Usage: share_prefix_dlls.py PREFIX RUNTIME [--dry-run]
"""
import argparse
import ctypes
import os
from pathlib import Path
import sys

MARKER = b'Wine builtin DLL'
libc = ctypes.CDLL(None, use_errno=True)
libc.clonefile.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32]
CLONE_NOFOLLOW = 0x0001


def is_builtin(path):
    try:
        with open(path, 'rb') as f:
            head = f.read(128)
    except OSError:
        return False
    return head[:2] == b'MZ' and MARKER in head


def clone(src, dst):
    tmp = dst.with_name(dst.name + '.mwxr-clone')
    try:
        tmp.unlink()
    except FileNotFoundError:
        pass
    if libc.clonefile(os.fsencode(src), os.fsencode(tmp), CLONE_NOFOLLOW) != 0:
        err = ctypes.get_errno()
        raise OSError(err, os.strerror(err), str(dst))
    os.replace(tmp, dst)


def folders(prefix, runtime):
    windows = Path(prefix) / 'drive_c/windows'
    lib = Path(runtime) / 'lib/wine'
    for sub, arch in (('system32', 'x86_64-windows'), ('syswow64', 'i386-windows')):
        if (lib / arch).is_dir():
            yield windows / sub, lib / arch
            yield windows / sub / 'drivers', lib / arch


def share(prefix, runtime, dry_run=False):
    cloned = matched = native = 0
    size = 0
    for folder, source in folders(prefix, runtime):
        if not folder.is_dir():
            continue
        for entry in os.scandir(folder):
            if not entry.is_file(follow_symlinks=False):
                continue
            src = source / entry.name
            try:
                s = src.stat()
            except FileNotFoundError:
                continue
            d = entry.stat(follow_symlinks=False)
            if d.st_size == s.st_size and int(d.st_mtime) == int(s.st_mtime):
                matched += 1  # cloned before (or an identical copy made the same way)
                continue
            if not is_builtin(entry.path):
                native += 1
                continue
            if not dry_run:
                clone(src, Path(entry.path))
            cloned += 1
            size += d.st_size
    return cloned, matched, native, size


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('prefix')
    p.add_argument('runtime')
    p.add_argument('--dry-run', action='store_true')
    a = p.parse_args()
    if not (Path(a.prefix) / 'system.reg').is_file():
        sys.exit(f'{a.prefix} is not a Wine prefix')
    cloned, matched, native, size = share(a.prefix, a.runtime, a.dry_run)
    if cloned or a.dry_run:
        verb = 'would share' if a.dry_run else 'shared'
        print(f'{a.prefix}: {verb} {cloned} built-ins with {a.runtime} ({size / 2**20:.0f} MB); '
              f'{matched} already shared, {native} native kept', file=sys.stderr)


if __name__ == '__main__':
    main()

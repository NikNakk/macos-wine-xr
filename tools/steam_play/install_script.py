#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Turn a Steam install script into a .reg file for the game's Wine prefix.

macOS Steam hands the Steam Play tool a resolved install script (evaluatorscript_<appid>.vdf,
docs/steam-play.md) where Windows Steam would run iscriptevaluator.exe. This applies the
part the 64-bit runtime can: its Registry steps, with %INSTALLDIR% as the install
folder's Windows path (Z: is the Mac's root). Redistributable installers ("run process")
are all 32-bit exes, so they are reported and skipped, and their "has run" keys are left
unset. Other sections are reported and skipped.

Usage: install_script.py SCRIPT.vdf --reg OUT.reg
Writes OUT.reg (UTF-16, Windows Registry Editor 5.00) when there is anything to import,
removes it otherwise, and prints one line per step.
"""
import argparse
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'steam_games'))
from steam_games import parse_vdf  # noqa: E402

ROOTS = {
    'HKEY_LOCAL_MACHINE': 'HKEY_LOCAL_MACHINE', 'HKLM': 'HKEY_LOCAL_MACHINE',
    'HKEY_CURRENT_USER': 'HKEY_CURRENT_USER', 'HKCU': 'HKEY_CURRENT_USER',
    'HKEY_CLASSES_ROOT': 'HKEY_CLASSES_ROOT', 'HKCR': 'HKEY_CLASSES_ROOT',
    'HKEY_USERS': 'HKEY_USERS', 'HKU': 'HKEY_USERS',
}
TYPES = ('string', 'dword')


def windows_path(path):
    return 'Z:' + path.replace('/', '\\')


def registry_key(name):
    """A key as the script spells it (keys keep the VDF's doubled backslashes) to reg form."""
    parts = [p for p in name.replace('\\\\', '\\').split('\\') if p]
    root = ROOTS.get(parts[0].upper()) if parts else None
    if not root:
        raise ValueError(f'unknown registry root in {name!r}')
    return '\\'.join([root] + parts[1:])


def expand(value, install_dir):
    return re.sub('%INSTALLDIR%', lambda _: windows_path(install_dir), value, flags=re.IGNORECASE)


def reg_string(text):
    return '"' + text.replace('\\', '\\\\').replace('"', '\\"') + '"'


def registry_values(block, language, install_dir, log, where):
    """[(type, name, value)] from one key's block. A block named after a language holds
    values for that language only: the script's language is used, others are skipped."""
    out = []
    for child, values in block.items():
        kind = child.lower()
        if not isinstance(values, dict):
            log(f'{where}: skipped {child!r}')
        elif kind in TYPES:
            out += [(kind, name, expand(str(value), install_dir)) for name, value in values.items()]
        elif language and kind == language.lower():
            out += registry_values(values, None, install_dir, log, where)
        else:
            log(f'{where}: skipped {child!r} values')
    return out


def is_uninstall(script):
    """True when every step has mode 3, which Steam sends when the game is uninstalled
    (mode 2: install finished, mode 1: before a start)."""
    root = script.get('evaluatorscript', script)
    modes = {str(step.get('mode', '')) for step in root.values() if isinstance(step, dict)}
    return modes == {'3'}


def convert(script):
    """(reg text or None, log lines) for a parsed evaluatorscript."""
    lines, log = [], []
    root = script.get('evaluatorscript', script)
    for step_id in sorted(root, key=lambda k: int(k) if k.isdigit() else 0):
        step = root[step_id]
        if not isinstance(step, dict):
            continue
        appid = step.get('appid', '?')
        install_dir = step.get('install_path', '')
        language = step.get('language', '')
        sections = step.get('compat_installscript', {}) or {}
        for section, body in sections.items():
            where = f'step {step_id} (app {appid})'
            if section.lower() == 'registry' and isinstance(body, dict):
                for key, block in body.items():
                    try:
                        name = registry_key(key)
                    except ValueError as e:
                        log.append(f'{where}: skipped: {e}')
                        continue
                    values = registry_values(block, language, install_dir, log.append, where)
                    if not values:
                        continue
                    lines.append(f'[{name}]')
                    for kind, value_name, value in values:
                        if kind == 'dword':
                            try:
                                number = int(value, 0)
                            except ValueError:
                                log.append(f'{where}: skipped dword {value_name!r}={value!r}')
                                continue
                            lines.append(f'{reg_string(value_name)}=dword:{number & 0xFFFFFFFF:08x}')
                        else:
                            lines.append(f'{reg_string(value_name)}={reg_string(value)}')
                        log.append(f'{where}: {name} {value_name} = {value}')
                    lines.append('')
            elif section.lower() == 'run process' and isinstance(body, dict):
                for name, entry in body.items():
                    process = (entry or {}).get('process 1', '?') if isinstance(entry, dict) else '?'
                    log.append(f'{where}: skipped installer {name!r} ({process}): installers are not run')
            else:
                log.append(f'{where}: skipped section {section!r}')
    if not lines:
        return None, log
    return 'Windows Registry Editor Version 5.00\r\n\r\n' + '\r\n'.join(lines) + '\r\n', log


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('script', type=Path)
    p.add_argument('--reg', type=Path)
    p.add_argument('--uninstall', action='store_true',
                   help='exit 0 if every step is an uninstall step (mode 3), 1 otherwise')
    a = p.parse_args()
    script = parse_vdf(a.script.read_text(encoding='utf-8', errors='replace'))
    if a.uninstall:
        sys.exit(0 if is_uninstall(script) else 1)
    if not a.reg:
        p.error('--reg is required')
    reg, log = convert(script)
    for line in log:
        print(line)
    if reg:
        a.reg.write_bytes(b'\xff\xfe' + reg.encode('utf-16-le'))
    elif a.reg.exists():
        a.reg.unlink()


if __name__ == '__main__':
    main()

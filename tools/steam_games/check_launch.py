#!/usr/bin/env python3
"""Validate a Steam entry against the selected Wine runtime before starting it."""
import argparse
from pathlib import Path
import struct
import sys


def pe_machine(executable):
    with Path(executable).open('rb') as stream:
        dos = stream.read(64)
        if len(dos) < 64 or dos[:2] != b'MZ':
            raise ValueError('not a Windows PE executable')
        stream.seek(struct.unpack_from('<I', dos, 60)[0])
        coff = stream.read(6)
        if len(coff) != 6 or coff[:4] != b'PE\0\0':
            raise ValueError('invalid Windows PE header')
        return struct.unpack_from('<H', coff, 4)[0]


def compatibility_error(executable, wine_runtime, steam_bridge=False):
    machine = pe_machine(executable)
    runtime = Path(wine_runtime)
    if machine == 0x14c and not (runtime / 'lib/wine/i386-windows/ntdll.dll').is_file():
        return (f'The selected launcher {Path(executable).name} is 32-bit, but the selected Wine runtime '
                'has no 32-bit ntdll.dll (WoW64 support). The normal launcher was kept; '
                'use a 32-bit-capable Wine runtime with the required game dependencies, '
                'or explicitly configure a supported 64-bit launch entry.')
    if machine == 0x14c and steam_bridge and not (runtime / 'lib/wine/i386-windows/lsteamclient.dll').is_file():
        return ('The selected launcher is 32-bit and Wine supports WoW64, but the macOS Steam bridge '
                'has no 32-bit lsteamclient.dll. Build and install the bridge against the WoW64 Wine tree.')
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', required=True)
    parser.add_argument('--wine-runtime', required=True)
    parser.add_argument('--name', default='Game')
    parser.add_argument('--steam-bridge', action='store_true')
    args = parser.parse_args()
    try:
        error = compatibility_error(args.exe, args.wine_runtime, args.steam_bridge)
    except (OSError, ValueError) as exc:
        sys.exit(f'{args.name}: cannot inspect launch executable: {exc}')
    if error:
        sys.exit(f'{args.name}: {error}')


if __name__ == '__main__':
    main()

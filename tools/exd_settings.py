#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Show or set EXD's integer graphics settings in a copy of GameSettings.sav.

EXD (UE4.27) keeps its graphics menu choices in a GVAS save. Integer properties
are fixed-size, so they can be changed in place without re-serialising the file.

  exd_settings.py SAVE                      show settings
  exd_settings.py SAVE OUT KEY=VALUE ...    write a copy with integers changed
"""
import struct
import sys

SHOWN = ['MSAASampleCount', 'UpscalerPreferredSelection', 'FogQuality']


def read_fstring(data, offset):
    length = struct.unpack_from('<i', data, offset)[0]
    offset += 4
    if length == 0:
        return '', offset
    if length < 0:
        end = offset - 2 * length
        return data[offset:end].decode('utf-16-le')[:-1], end
    return data[offset:offset + length - 1].decode('latin1'), offset + length


def find_property(data, name, unique=True):
    key = struct.pack('<i', len(name) + 1) + name.encode() + b'\0'
    index = data.find(key)
    if index < 0 or (unique and data.find(key, index + 1) >= 0):
        raise SystemExit(f'{name}: not found exactly once')
    kind, offset = read_fstring(data, index + len(key))
    offset += 8  # value size
    return kind, offset


def int_value(data, name):
    kind, offset = find_property(data, name)
    if kind != 'IntProperty':
        raise SystemExit(f'{name}: {kind}, not IntProperty')
    return offset + 1  # skip the property GUID flag


def command_value(data, name):
    key = struct.pack('<i', len(name) + 1) + name.encode() + b'\0'
    index = data.find(key)
    if index < 0 or data.find(key, index + 1) >= 0:
        raise SystemExit(f'{name}: not found exactly once')
    return index + len(key)


def show(data):
    for name in SHOWN:
        print(f'{name}={struct.unpack_from("<i", data, int_value(data, name))[0]}')
    for name in ['r.VolumetricFog', 'r.DefaultFeature.AntiAliasing']:
        print(f'{name}={struct.unpack_from("<i", data, command_value(data, name))[0]}')
    # The enum's type name repeats the property name; the property comes first.
    kind, offset = find_property(data, 'UDLSSMode', unique=False)
    if kind == 'EnumProperty':
        _, offset = read_fstring(data, offset)
        value, _ = read_fstring(data, offset + 1)
        print(f'UDLSSMode={value}')


def main():
    if len(sys.argv) < 2 or len(sys.argv) == 3:
        raise SystemExit(__doc__)
    data = bytearray(open(sys.argv[1], 'rb').read())
    if len(sys.argv) > 3:
        for item in sys.argv[3:]:
            name, value = item.split('=', 1)
            # Console variables live in the CustomCommands string->int map.
            offset = command_value(data, name) if name.startswith('r.') else int_value(data, name)
            data[offset:offset + 4] = struct.pack('<i', int(value))
        open(sys.argv[2], 'wb').write(data)
    show(data)


if __name__ == '__main__':
    main()

# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
import struct
import tempfile
import unittest
from pathlib import Path

import steam_games


def binary_kv(strings, value):
    """Encode nested dicts as appinfo v29 binary KeyValues, interning keys in strings."""
    out = b''
    for key, v in value.items():
        if key not in strings:
            strings.append(key)
        index = struct.pack('<i', strings.index(key))
        if isinstance(v, dict):
            out += b'\x00' + index + binary_kv(strings, v)
        elif isinstance(v, int):
            out += b'\x02' + index + struct.pack('<i', v)
        else:
            out += b'\x01' + index + v.encode() + b'\0'
    return out + b'\x08'


def appinfo_file(apps):
    strings, entries = [], b''
    for appid, info in apps.items():
        body = struct.pack('<IIQ', 2, 0, 0) + b'\0' * 20 + struct.pack('<I', 1) + b'\0' * 20
        body += binary_kv(strings, {'appinfo': info})
        entries += struct.pack('<II', appid, len(body)) + body
    entries += struct.pack('<I', 0)
    table = 16 + len(entries)
    string_table = struct.pack('<I', len(strings)) + b''.join(s.encode() + b'\0' for s in strings)
    return struct.pack('<IIq', 0x07564429, 1, table) + entries + string_table


class SteamGamesTest(unittest.TestCase):
    def test_text_keyvalues(self):
        text = '"AppState" { "appid" "1256230" "installdir" "Hyperbolica" "x" { "a" "C:\\\\b" } }'
        state = steam_games.parse_vdf(text)['AppState']
        self.assertEqual(state['appid'], '1256230')
        self.assertEqual(state['x']['a'], 'C:\\b')

    def test_binary_appinfo_and_vr(self):
        apps = {
            1256230: {'appid': 1256230, 'common': {'name': 'Hyperbolica', 'type': 'Game',
                                                   'oslist': 'windows,macos', 'openxrsupport': '1'},
                      'config': {'installdir': 'Hyperbolica', 'launch': {
                          '0': {'executable': 'Hyperbolica.exe', 'type': 'default', 'config': {'oslist': 'windows'}},
                          '1': {'executable': 'Hyperbolica.exe', 'arguments': 'startXR', 'type': 'vr',
                                'config': {'oslist': 'windows'}},
                          '2': {'executable': 'Hyperbolica.app', 'config': {'oslist': 'macos'}}}}},
            440: {'appid': 440, 'common': {'name': 'Flat', 'type': 'Game', 'oslist': 'windows'}},
        }
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'appinfo.vdf'
            path.write_bytes(appinfo_file(apps))
            read = steam_games.read_appinfo(path)
        self.assertEqual(set(read), {1256230, 440})
        self.assertEqual(read[1256230]['common']['name'], 'Hyperbolica')
        self.assertTrue(steam_games.is_vr(read[1256230]))
        self.assertFalse(steam_games.is_vr(read[440]))
        launches = steam_games.windows_launches(read[1256230])
        self.assertEqual([(e['executable'], e['arguments'], e['type']) for e in launches],
                         [('Hyperbolica.exe', '', 'default'), ('Hyperbolica.exe', 'startXR', 'vr')])

    def test_progress_line(self):
        m = steam_games.PROGRESS.search(' Update state (0x61) downloading, progress: 91.66 (67483084727 / 73624284853)')
        self.assertEqual((m.group(2), m.group(4), m.group(5)), ('downloading', '67483084727', '73624284853'))


if __name__ == '__main__':
    unittest.main()

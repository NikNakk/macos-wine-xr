import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / 'scripts/steam-play-game-settings.zsh'


class GameSettingsTests(unittest.TestCase):
    def settings(self, app, **overrides):
        env = {k: v for k, v in os.environ.items() if k not in
               ('MWXR_IN_PROCESS_WINE', 'MWXR_GAME_PREFIXES', 'GAME_PREFIX', 'MWXR_WOW64',
                'GAME_EXE', 'STEAM_COMPAT_INSTALL_PATH')}
        env.update(overrides)
        result = subprocess.run(['zsh', '-c',
            'source "$1" "$2"; print -r -- "${MWXR_IN_PROCESS_WINE:-unset}" "${MWXR_GAME_PREFIXES:-unset}" "${GAME_PREFIX:-unset}"',
            'test', str(SCRIPT), app], env=env, check=True, capture_output=True, text=True)
        return result.stdout.strip().split()

    def test_assetto_uses_separate_wow64_runtime_and_prefix_root(self):
        values = self.settings('244210')
        self.assertTrue(values[0].endswith('/Code/wine-crossover/runtime-wow64'))
        self.assertTrue(values[1].endswith('/Windows/prefixes-wow64'))
        self.assertEqual(values[2], 'pergame')

    def test_other_games_keep_existing_settings(self):
        self.assertEqual(self.settings('1256230'), ['unset', 'unset', 'unset'])

    def fake_exe(self, directory, machine):
        data = bytearray(134)
        data[:2] = b'MZ'
        struct.pack_into('<I', data, 60, 128)
        data[128:132] = b'PE\0\0'
        struct.pack_into('<H', data, 132, machine)
        exe = Path(directory) / 'game.exe'
        exe.write_bytes(data)
        return str(exe)

    def test_a_32_bit_exe_from_steam_play_selects_wow64(self):
        with tempfile.TemporaryDirectory() as directory:
            values = self.settings('1', GAME_EXE=self.fake_exe(directory, 0x14c))
        self.assertTrue(values[0].endswith('/Code/wine-crossover/runtime-wow64'))
        self.assertEqual(values[2], 'pergame')

    def test_a_64_bit_exe_keeps_the_default_runtime(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(self.settings('1', GAME_EXE=self.fake_exe(directory, 0x8664)),
                             ['unset', 'unset', 'unset'])

    def test_mwxr_wow64_overrides_detection(self):
        with tempfile.TemporaryDirectory() as directory:
            exe = self.fake_exe(directory, 0x14c)
            self.assertEqual(self.settings('1', GAME_EXE=exe, MWXR_WOW64='0'), ['unset', 'unset', 'unset'])
        self.assertEqual(self.settings('244210', MWXR_WOW64='0'), ['unset', 'unset', 'unset'])
        self.assertTrue(self.settings('1256230', MWXR_WOW64='1')[0].endswith('/runtime-wow64'))

    def test_explicit_overrides_are_preserved(self):
        self.assertEqual(self.settings('244210', MWXR_IN_PROCESS_WINE='/custom/wine',
                         MWXR_GAME_PREFIXES='/custom/prefixes', GAME_PREFIX='rig'),
                         ['/custom/wine', '/custom/prefixes', 'rig'])


if __name__ == '__main__':
    unittest.main()

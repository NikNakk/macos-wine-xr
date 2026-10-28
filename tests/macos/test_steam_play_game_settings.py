import os
from pathlib import Path
import subprocess
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / 'scripts/steam-play-game-settings.zsh'


class GameSettingsTests(unittest.TestCase):
    def settings(self, app, **overrides):
        env = {k: v for k, v in os.environ.items() if k not in
               ('MWXR_IN_PROCESS_WINE', 'MWXR_GAME_PREFIXES', 'GAME_PREFIX')}
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

    def test_explicit_overrides_are_preserved(self):
        self.assertEqual(self.settings('244210', MWXR_IN_PROCESS_WINE='/custom/wine',
                         MWXR_GAME_PREFIXES='/custom/prefixes', GAME_PREFIX='rig'),
                         ['/custom/wine', '/custom/prefixes', 'rig'])


if __name__ == '__main__':
    unittest.main()

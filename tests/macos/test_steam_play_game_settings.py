import os
from pathlib import Path
import subprocess
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / 'scripts/steam-play-game-settings.zsh'


class GameSettingsTests(unittest.TestCase):
    """Every game uses the rig's one WoW64 runtime and prefix root (docs/steam-play.md)."""

    def settings(self, app, **overrides):
        env = {k: v for k, v in os.environ.items() if k not in
               ('MWXR_IN_PROCESS_WINE', 'MWXR_GAME_PREFIXES', 'GAME_PREFIX', 'MWXR_FOVEATION',
                'MWXR_VULKAN', 'GAME_EXE', 'STEAM_COMPAT_INSTALL_PATH')}
        env.update(overrides)
        result = subprocess.run(['zsh', '-c',
            'source "$1" "$2"; print -r -- "${MWXR_IN_PROCESS_WINE:-unset}" "${MWXR_GAME_PREFIXES:-unset}" "${GAME_PREFIX:-unset}"',
            'test', str(SCRIPT), app], env=env, check=True, capture_output=True, text=True)
        return result.stdout.strip().split()

    def test_games_keep_the_rig_runtime_and_prefix_root(self):
        # Assetto Corsa (32-bit launcher) chose WoW64 here before the runtimes were unified.
        for app in ('244210', '1256230', '499520'):
            with self.subTest(app=app):
                self.assertEqual(self.settings(app), ['unset', 'unset', 'unset'])

    def test_old_wow64_setting_is_ignored(self):
        self.assertEqual(self.settings('1256230', MWXR_WOW64='1'), ['unset', 'unset', 'unset'])

    def test_foveation_selects_the_unified_foveation_runtime(self):
        self.assertTrue(self.settings('1256230', MWXR_FOVEATION='gaze')[0].endswith(
            '/build-in-process/wine-foveation-unified'))

    def test_explicit_overrides_are_preserved(self):
        self.assertEqual(self.settings('244210', MWXR_IN_PROCESS_WINE='/custom/wine',
                         MWXR_GAME_PREFIXES='/custom/prefixes', GAME_PREFIX='rig'),
                         ['/custom/wine', '/custom/prefixes', 'rig'])


if __name__ == '__main__':
    unittest.main()

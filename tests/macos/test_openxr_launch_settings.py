import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / 'scripts/openxr-launch-settings.zsh'
KEYS = ('MWXR_CLIENT_COMPOSITOR', 'MWXR_VIEWPORT_SCALE_PERCENTAGE',
        'XRT_MACOS_CLIENT_COMPOSITOR', 'OXR_VIEWPORT_SCALE_PERCENTAGE',
        'MWXR_FOVEATION', 'MWXR_GAZE_FOVEATION', 'MWXR_GAZE_FOVEATION_FIXED', 'DXMT_STAY_WARPED',
        'MWXR_IN_PROCESS_WINE', 'MWXR_IN_PROCESS_BUILD', 'MWXR_FOVEATION_BUILD',
        'MWXR_VULKAN', 'MWXR_KK_ROOT', 'KK_IMPLICIT_FDM', 'MWXR_KK_FOVEATION', 'MESA_KK_EXPERIMENTAL',
        'MWXR_XRIZER_ROOT', 'MWXR_WOW64', 'KK_IMPLICIT_SINGLE', 'KK_IMPLICIT_SCALED',
        'KK_IMPLICIT_STORAGE', 'KK_IMPLICIT_COMPACT', 'KK_IMPLICIT_HALF', 'KK_PASS_TIMING',
        'MWXR_KK_PASS_TIMING', 'MESA_VK_WSI_PRESENT_MODE', 'MWXR_SPECTATOR_VSYNC', 'GAME_EXE', 'STEAM_COMPAT_INSTALL_PATH', 'MWXR_OPENGL')
GAME_SETTINGS = SCRIPT.parent / 'steam-play-game-settings.zsh'


class OpenXRLaunchSettingsTests(unittest.TestCase):
    def settings(self, **values):
        env = {k: v for k, v in os.environ.items() if k not in KEYS}
        env.update(values)
        return subprocess.run(['zsh', '-ec',
            'source "$1"; print -r -- "${XRT_MACOS_CLIENT_COMPOSITOR-unset}" '
            '"${OXR_VIEWPORT_SCALE_PERCENTAGE-unset}"', 'test', str(SCRIPT)],
            env=env, capture_output=True, text=True)

    def test_defaults_preserve_inherited_settings(self):
        self.assertEqual(self.settings().stdout.strip(), 'unset unset')
        result = self.settings(XRT_MACOS_CLIENT_COMPOSITOR='1',
                               OXR_VIEWPORT_SCALE_PERCENTAGE='85')
        self.assertEqual(result.stdout.strip(), '1 85')

    def test_launch_overrides_inherited_settings(self):
        result = self.settings(XRT_MACOS_CLIENT_COMPOSITOR='1',
            OXR_VIEWPORT_SCALE_PERCENTAGE='100', MWXR_CLIENT_COMPOSITOR='0',
            MWXR_VIEWPORT_SCALE_PERCENTAGE='70')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), '0 70')
        self.assertIn('client_compositor=0 viewport_scale=70%', result.stderr)

    def test_invalid_values_stop_launch(self):
        for value in ('', '-1', '0', '201', '70.5', 'abc', '1+1'):
            with self.subTest(scale=value):
                self.assertEqual(self.settings(MWXR_VIEWPORT_SCALE_PERCENTAGE=value).returncode, 2)
        for value in ('', 'yes', '2'):
            with self.subTest(compositor=value):
                self.assertEqual(self.settings(MWXR_CLIENT_COMPOSITOR=value).returncode, 2)

    def foveation(self, value, marker=True, **extra):
        with tempfile.TemporaryDirectory() as wine:
            if marker:
                (Path(wine) / 'share').mkdir()
                (Path(wine) / 'share/mwxr-foveation-dxmt').write_text('test')
            env = {k: v for k, v in os.environ.items() if k not in KEYS}
            env.update(MWXR_IN_PROCESS_WINE=wine, MWXR_IN_PROCESS_BUILD='/ordinary/gate', **extra)
            if value is not None:
                env['MWXR_FOVEATION'] = value
            return subprocess.run(['zsh', '-ec',
                'source "$1"; print -r -- "${MWXR_GAZE_FOVEATION-unset}" '
                '"${MWXR_GAZE_FOVEATION_FIXED-unset}" "${DXMT_STAY_WARPED-unset}" '
                '"${MWXR_IN_PROCESS_BUILD:t}" "${KK_IMPLICIT_FDM-unset}" '
                '"${MWXR_KK_FOVEATION-unset}" "${MESA_KK_EXPERIMENTAL-unset}"', 'test', str(SCRIPT)],
                env=env, capture_output=True, text=True)

    def test_foveation_is_off_unless_asked_for(self):
        for value in (None, 'off'):
            with self.subTest(value=value):
                self.assertEqual(self.foveation(value).stdout.strip(), 'unset unset unset gate unset unset unset')

    def test_foveation_modes_and_numbers(self):
        self.assertEqual(self.foveation('gaze').stdout.strip(), '0.3 unset external gaze-gate unset unset unset')
        self.assertEqual(self.foveation('fixed').stdout.strip(), '0.3 1 external gaze-gate unset unset unset')
        self.assertEqual(self.foveation('gaze:0.25,10,22').stdout.strip(),
                         '0.25,10,22 unset external gaze-gate unset unset unset')
        self.assertEqual(self.foveation('gaze:0.25,10,22,1.5').stdout.strip(),
                         '0.25,10,22,1.5 unset external gaze-gate unset unset unset')

    def test_invalid_foveation_stops_launch(self):
        for value in ('', 'on', 'gaze:', 'gaze:2', 'gaze:0.05', 'gaze:0.3,10', 'gaze:0.3,30,20',
                      'gaze:0.3,10,95', 'gaze:0.3;10', 'fixed:abc'):
            with self.subTest(value=value):
                self.assertEqual(self.foveation(value).returncode, 2)
        self.assertEqual(self.foveation('gaze', marker=False).returncode, 2)

    def test_vulkan_foveation_uses_kosmickrisp_not_dxmt(self):
        # No foveation DXMT needed; KosmicKrisp takes the bridge's profile.
        result = self.foveation('gaze', marker=False, MWXR_VULKAN='kosmickrisp',
                                MESA_KK_EXPERIMENTAL='custom_border')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(),
                         '0.3 unset unset vk-fov-gate auto 1 custom_border,fdm_rate')
        result = self.foveation('fixed:0.4', marker=False, MWXR_VULKAN='kosmickrisp',
                                MESA_KK_EXPERIMENTAL='fdm_rate')
        self.assertEqual(result.stdout.strip(), '0.4 1 unset vk-fov-gate auto 1 fdm_rate')

    def game(self, appid, **values):
        env = {k: v for k, v in os.environ.items() if k not in KEYS}
        env.update(values)
        return subprocess.run(['zsh', '-ec',
            'source "$1" "$2"; print -r -- "${MWXR_VULKAN-unset}" "${MWXR_IN_PROCESS_WINE:t}" '
            '"${MWXR_XRIZER_ROOT:t}" "${MWXR_KK_ROOT:t}"', 'test', str(GAME_SETTINGS), appid],
            env=env, capture_output=True, text=True)

    def test_vulkan_games_select_kosmickrisp(self):
        for appid in ('552440', '275850'):
            with self.subTest(appid=appid):
                self.assertEqual(self.game(appid).stdout.strip(),
                                 'kosmickrisp vulkan-wine vulkan-xrizer zink-fov')
        # Foveation keeps the Vulkan runtime rather than the DXMT foveation one.
        self.assertEqual(self.game('552440', MWXR_FOVEATION='gaze').stdout.strip(),
                         'kosmickrisp vulkan-wine vulkan-xrizer zink-fov')
        self.assertEqual(self.game('552440', MWXR_VULKAN='off').stdout.split(), ['unset'])
        self.assertEqual(self.game('546560').stdout.split(), ['unset'])

    def test_nms_foveates_storage_targets_without_compaction(self):
        def switches(appid, **values):
            env = {k: v for k, v in os.environ.items() if k not in KEYS}
            env.update(values)
            return subprocess.run(['zsh', '-ec',
                'source "$1" "$2"; print -r -- "${KK_IMPLICIT_STORAGE-unset}" '
                '"${KK_IMPLICIT_COMPACT-unset}"', 'test', str(GAME_SETTINGS), appid],
                env=env, capture_output=True, text=True).stdout.split()
        self.assertEqual(switches('275850'), ['1', '0'])
        self.assertEqual(switches('275850', KK_IMPLICIT_COMPACT='1'), ['1', '1'])
        self.assertEqual(switches('552440'), ['unset', 'unset'])

    def test_pass_timing_from_launch_options(self):
        def timing(appid, **values):
            env = {k: v for k, v in os.environ.items() if k not in KEYS}
            env.update(values)
            return subprocess.run(['zsh', '-ec',
                'source "$1" "$2"; print -r -- "${KK_PASS_TIMING-unset}"',
                'test', str(GAME_SETTINGS), appid], env=env, capture_output=True,
                text=True).stdout.strip()
        self.assertEqual(timing('275850', MWXR_KK_PASS_TIMING='1'), '1')
        self.assertEqual(timing('275850'), 'unset')

    def test_spectator_window_presents_immediately(self):
        def mode(appid, **values):
            env = {k: v for k, v in os.environ.items() if k not in KEYS}
            env.update(values)
            return subprocess.run(['zsh', '-ec',
                'source "$1" "$2"; print -r -- "${MESA_VK_WSI_PRESENT_MODE-unset}"',
                'test', str(GAME_SETTINGS), appid], env=env, capture_output=True,
                text=True).stdout.strip()
        self.assertEqual(mode('275850'), 'immediate')
        self.assertEqual(mode('552440'), 'immediate')
        self.assertEqual(mode('275850', MWXR_SPECTATOR_VSYNC='1'), 'unset')
        self.assertEqual(mode('546560'), 'unset')


if __name__ == '__main__':
    unittest.main()

import os
from pathlib import Path
import subprocess
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / 'scripts/openxr-launch-settings.zsh'
KEYS = ('MWXR_CLIENT_COMPOSITOR', 'MWXR_VIEWPORT_SCALE_PERCENTAGE',
        'XRT_MACOS_CLIENT_COMPOSITOR', 'OXR_VIEWPORT_SCALE_PERCENTAGE')


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


if __name__ == '__main__':
    unittest.main()

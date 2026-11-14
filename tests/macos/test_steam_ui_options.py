import json
from pathlib import Path
import shutil
import subprocess
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / 'src/macos/steam_ui_options.js'

# Loads the page script in Node and runs the Launch Options editing on each case.
HARNESS = r'''
globalThis.SteamClient = {Apps: {RegisterForAppDetails() {}, SetAppLaunchOptions() {}}};
require(process.argv[1]);
const o = globalThis.mwxrOptions, byId = Object.fromEntries(o.options.map(x => [x.id, x]));
const cases = JSON.parse(process.argv[2]);
console.log(JSON.stringify(cases.map(([op, s, id, v]) =>
  op === 'read' ? byId[id].read(s) : o.withValues(s, byId[id].write(v)))));
'''


@unittest.skipUnless(shutil.which('node'), 'needs node')
class SteamUiOptionsTests(unittest.TestCase):
    def run_cases(self, cases):
        out = subprocess.run(['node', '-e', HARNESS, str(SCRIPT), json.dumps(cases)],
                             check=True, capture_output=True, text=True).stdout
        return json.loads(out)

    def test_uevr_words(self):
        self.assertEqual(self.run_cases([
            ['write', '', 'uevr', 'openxr'],
            ['write', '-novid %command%', 'uevr', 'openvr'],
            ['write', 'MWXR_UEVR=1 MWXR_UEVR_RUNTIME=openvr -novid', 'uevr', ''],
            ['write', 'MWXR_UEVR=1 MWXR_UEVR_RUNTIME=openvr', 'uevr', 'openxr'],
            # MWXR_UEVR_DELAY is not MWXR_UEVR: kept.
            ['write', 'MWXR_UEVR_DELAY=8 -x', 'uevr', 'openxr'],
        ]), [
            'MWXR_UEVR=1',
            'MWXR_UEVR=1 MWXR_UEVR_RUNTIME=openvr -novid %command%',
            '-novid',
            'MWXR_UEVR=1',
            'MWXR_UEVR=1 MWXR_UEVR_DELAY=8 -x',
        ])

    def test_reading(self):
        self.assertEqual(self.run_cases([
            ['read', '', 'uevr', None],
            ['read', '-x MWXR_UEVR=1', 'uevr', None],
            ['read', 'MWXR_UEVR=1 MWXR_UEVR_RUNTIME=openvr', 'uevr', None],
            ['read', 'MWXR_UEVR=0', 'uevr', None],
            ['read', 'MWXR_FOVEATION=off', 'foveation', None],
            ['read', 'MWXR_KK_PASS_TIMING=1 MWXR_FOVEATION=gaze:0.6 +foo', 'foveation', None],
        ]), ['', 'openxr', 'openvr', '', '', 'gaze:0.6'])

    def test_other_words_and_arguments_are_kept(self):
        self.assertEqual(self.run_cases([
            ['write', 'MWXR_KK_PASS_TIMING=1 MWXR_FOVEATION=gaze:0.6 +foo "a b"', 'foveation', 'fixed'],
            ['write', 'MWXR_FOVEATION=fixed %command% -vr', 'foveation', ''],
            ['write', '  -dx11  ', 'foveation', 'gaze'],
        ]), [
            'MWXR_FOVEATION=fixed MWXR_KK_PASS_TIMING=1 +foo "a b"',
            '%command% -vr',
            'MWXR_FOVEATION=gaze -dx11  ',
        ])


if __name__ == '__main__':
    unittest.main()

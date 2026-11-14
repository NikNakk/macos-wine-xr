import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile

TOOL = Path(__file__).resolve().parents[2] / 'tools/steam_play/uevr_profile.py'


class UevrProfileTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.profiles = self.root / 'uevr/profiles'
        self.prefix = self.root / 'prefix'
        users = self.prefix / 'drive_c/users'
        (users / 'crossover/AppData/Roaming').mkdir(parents=True)
        (users / 'Public').mkdir()
        (users / 'steamuser').symlink_to('crossover')
        self.mod = users / 'crossover/AppData/Roaming/UnrealVRMod'

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, *args, check=True):
        env = dict(os.environ, MWXR_UEVR_PROFILES=str(self.profiles))
        return subprocess.run(['python3', str(TOOL), *args], env=env, check=check, capture_output=True, text=True)

    def test_link_moves_prefix_settings_into_shared_folder(self):
        (self.mod / 'Game').mkdir(parents=True)
        (self.mod / 'Game/config.txt').write_text('VR_ResolutionScale=0.8\n')
        self.run_tool('link', str(self.prefix))
        self.assertTrue(self.mod.is_symlink())
        self.assertEqual(self.mod.resolve(), self.profiles.resolve())
        self.assertEqual((self.profiles / 'Game/config.txt').read_text(), 'VR_ResolutionScale=0.8\n')
        self.assertFalse((self.prefix / 'drive_c/users/Public/AppData').exists())
        self.run_tool('link', str(self.prefix))  # again: nothing to do
        self.assertTrue(self.mod.is_symlink())

    def test_link_keeps_shared_settings_and_backs_up_prefix_copy(self):
        (self.profiles / 'Game').mkdir(parents=True)
        (self.profiles / 'Game/config.txt').write_text('shared\n')
        (self.mod / 'Game').mkdir(parents=True)
        (self.mod / 'Game/config.txt').write_text('prefix\n')
        self.run_tool('link', str(self.prefix))
        self.assertEqual((self.profiles / 'Game/config.txt').read_text(), 'shared\n')
        saved = list((self.profiles.parent / 'profile-backups').glob('Game-prefix-*'))
        self.assertEqual(len(saved), 1)
        self.assertEqual((saved[0] / 'config.txt').read_text(), 'prefix\n')

    def make_zip(self, entries):
        path = self.root / 'profile.zip'
        with zipfile.ZipFile(path, 'w') as z:
            for name, data in entries.items():
                z.writestr(name, data)
        return path

    def test_import_finds_nested_profile_and_backs_up_old_one(self):
        (self.profiles / 'Game').mkdir(parents=True)
        (self.profiles / 'Game/config.txt').write_text('old\n')
        archive = self.make_zip({'Game-v2/Game/config.txt': 'new\n', 'Game-v2/Game/scripts/a.lua': '-- lua\n',
                                 'Game-v2/README.txt': 'read me\n'})
        self.run_tool('import', '1', str(archive), '--exe', 'Game.exe')
        self.assertEqual((self.profiles / 'Game/config.txt').read_text(), 'new\n')
        self.assertTrue((self.profiles / 'Game/scripts/a.lua').is_file())
        self.assertEqual(len(list((self.profiles.parent / 'profile-backups').glob('Game-before-import-*'))), 1)

    def test_import_refuses_paths_outside_the_folder(self):
        archive = self.make_zip({'config.txt': 'x\n', '../escape.txt': 'x\n'})
        result = self.run_tool('import', '1', str(archive), '--exe', 'Game', check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / 'uevr/escape.txt').exists())
        self.assertFalse((self.profiles / 'Game').exists())

    def test_import_refuses_zip_without_a_profile(self):
        archive = self.make_zip({'readme.txt': 'x\n'})
        self.assertNotEqual(self.run_tool('import', '1', str(archive), '--exe', 'Game', check=False).returncode, 0)


if __name__ == '__main__':
    unittest.main()

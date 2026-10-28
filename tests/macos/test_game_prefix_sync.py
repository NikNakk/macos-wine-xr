"""Check that per-game provisioning keeps Windows architectures separate."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / 'scripts/game-prefix.zsh'


class PrefixSyncTests(unittest.TestCase):
    def test_wow64_sync_and_old_prefix_rejection(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runtime, prefixes = root / 'runtime', root / 'prefixes'
            prefix = prefixes / '244210'
            system32 = prefix / 'drive_c/windows/system32'
            system32.mkdir(parents=True)
            (prefix / 'system.reg').touch()
            (system32 / 'ntdll.dll').write_bytes(b'old64')
            for arch in ['x86_64', 'i386']:
                folder = runtime / f'lib/wine/{arch}-windows'
                folder.mkdir(parents=True)
                for name in ['ntdll.dll', 'lsteamclient.dll']:
                    (folder / name).write_bytes(arch.encode())
            env = dict(os.environ, MWXR_IN_PROCESS_WINE=str(runtime), MWXR_GAME_PREFIXES=str(prefixes))
            result = subprocess.run(['zsh', str(SCRIPT), 'ensure', '244210'], env=env, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b'WoW64 system DLLs', result.stderr)
            self.assertEqual((system32 / 'ntdll.dll').read_bytes(), b'old64')
            syswow64 = prefix / 'drive_c/windows/syswow64'
            syswow64.mkdir()
            (syswow64 / 'ntdll.dll').write_bytes(b'old32')
            subprocess.run(['zsh', str(SCRIPT), 'ensure', '244210'], env=env, check=True, capture_output=True)
            for folder, content in [(system32, b'x86_64'), (syswow64, b'i386')]:
                for name in ['ntdll.dll', 'lsteamclient.dll']:
                    self.assertEqual((folder / name).read_bytes(), content)


if __name__ == '__main__':
    unittest.main()

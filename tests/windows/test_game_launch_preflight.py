import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

module_path = Path(__file__).resolve().parents[2] / 'tools/steam_games/check_launch.py'
spec = importlib.util.spec_from_file_location('check_launch', module_path)
launch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launch)


class LaunchPreflightTests(unittest.TestCase):
    def test_architecture_and_runtime_support(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            exe = root / 'launcher.exe'
            data = bytearray(134)
            data[:2] = b'MZ'
            struct.pack_into('<I', data, 60, 128)
            data[128:132] = b'PE\0\0'
            struct.pack_into('<H', data, 132, 0x14c)
            exe.write_bytes(data)
            self.assertIn('32-bit', launch.compatibility_error(exe, root))
            wow = root / 'lib/wine/i386-windows/ntdll.dll'
            wow.parent.mkdir(parents=True)
            wow.write_bytes(b'installed')
            self.assertIsNone(launch.compatibility_error(exe, root))
            self.assertIn('lsteamclient', launch.compatibility_error(exe, root, steam_bridge=True))
            bridge = wow.with_name('lsteamclient.dll')
            bridge.write_bytes(b'installed')
            self.assertIsNone(launch.compatibility_error(exe, root, steam_bridge=True))
            wow.unlink()
            struct.pack_into('<H', data, 132, 0x8664)
            exe.write_bytes(data)
            self.assertIsNone(launch.compatibility_error(exe, root))

    def test_invalid_executable_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'bad.exe'
            exe.write_bytes(b'not an executable')
            with self.assertRaises(ValueError):
                launch.pe_machine(exe)


if __name__ == '__main__':
    unittest.main()

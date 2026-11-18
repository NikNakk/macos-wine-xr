#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/steam_play'))
import managed_stack as contract
import managed_runner
import prepare_runtime
spec = importlib.util.spec_from_file_location('install_prebuilt', ROOT / 'scripts/install-prebuilt-steam-play.py')
install = importlib.util.module_from_spec(spec)
spec.loader.exec_module(install)


class ManagedSteamPlay(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='managed-steam-test-')
        self.root = Path(self.temporary.name).resolve()
        self.wine = self.root / 'external Wine'
        self.bridge = self.root / 'external bridge'
        def write(root, relative, data=b'fixture'):
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            return path
        self.write = write
        for name in ['bin/wine', 'bin/wineserver', 'lib/wine/x86_64-unix/ntdll.so',
                     'lib/wine/x86_64-unix/lsteamclient.so', 'lib/wine/x86_64-windows/lsteamclient.dll',
                     'lib/wine/i386-windows/lsteamclient.dll', 'lib/wine/x86_64-unix/winemetal.so']:
            write(self.wine, name)
        write(self.wine, 'lib/wine/x86_64-windows/ntdll.dll', 'MWXR_LSTEAMCLIENT'.encode('utf-16-le'))
        write(self.wine, 'lib/wine/i386-windows/ntdll.dll', 'steamclient'.encode('utf-16-le'))
        for name in ['x86_64-unix/wineopenxr.so', 'x86_64-windows/wineopenxr.dll']:
            write(self.bridge, name)
        self.loader = write(self.root, 'loader.dll')
        self.stand_in = write(self.root, 'mwxr-steam.exe')
        self.artifacts = {}
        for key in ['monado/macos-x86_64', 'xrizer/windows-x86_64', 'dxmt/macos-x86_64', 'dxmt/windows-x86_64']:
            self.artifacts[key] = str(self.root / 'components' / key)
        monado = Path(self.artifacts['monado/macos-x86_64'])
        runtime = write(monado, 'share/openxr/1/openxr_monado.json', b'{"runtime":{"library_path":"../../../lib/libopenxr_monado.dylib"}}')
        write(monado, 'lib/libopenxr_monado.dylib', b'xx\0v25.1.0-1-gabcdef1\0')
        for name in ['openvr_api.dll', 'bin/vrclient_x64.dll']:
            write(Path(self.artifacts['xrizer/windows-x86_64']), name)
        write(Path(self.artifacts['dxmt/macos-x86_64']), 'x86_64-unix/winemetal.so', b'selected DXMT')
        for name in ['d3d10core.dll', 'd3d11.dll', 'dxgi.dll', 'winemetal.dll']:
            write(Path(self.artifacts['dxmt/windows-x86_64']), 'x86_64-windows/' + name)
        self.stack = self.root / 'selected stack.json'
        self.document = {'schema': contract.SCHEMA, 'id': 'test', 'monado_build_tag': 'v25.1.0-1-gabcdef1',
                         'native': {'components': {}, 'runtime_json': str(runtime)}, 'artifacts': self.artifacts}
        self.stack.write_text(json.dumps(self.document))
        self.prefixes = self.root / 'persistent prefixes'
        self.output = self.root / 'prepared stack'

    def tearDown(self):
        self.temporary.cleanup()

    def prepare(self):
        return prepare_runtime.prepare(self.stack, self.wine, self.bridge, self.loader, self.stand_in,
                                       self.output, self.prefixes, relocate_binaries=False)

    def test_composition_keeps_external_runtime_and_prefixes_untouched(self):
        result = self.prepare()
        value = contract.load_stack(result)
        environment = contract.steam_environment(value)
        self.assertEqual((self.wine / 'lib/wine/x86_64-unix/winemetal.so').read_bytes(), b'fixture')
        self.assertEqual((self.output / 'wine/lib/wine/x86_64-unix/winemetal.so').read_bytes(), b'selected DXMT')
        self.assertFalse(self.prefixes.exists())
        self.assertEqual(environment['MWXR_IN_PROCESS_PREFIX'], str(self.prefixes / '546560'))
        self.assertFalse(value['steam_play']['build_attestation_verified'])
        with self.assertRaisesRegex(ValueError, 'overwrite'):
            self.prepare()

    def test_changed_bridge_is_rejected_before_launch(self):
        result = self.prepare()
        (self.output / 'wine/bridge/x86_64-unix/wineopenxr.so').write_bytes(b'tampered')
        with self.assertRaisesRegex(ValueError, 'changed'):
            contract.steam_environment(contract.load_stack(result))

    def test_private_profile_can_adopt_the_packaged_public_loader(self):
        self.write(self.bridge, 'lib/libopenxr_loader.dylib', b'packaged loader')
        value = contract.load_stack(self.prepare())
        contract.steam_environment(value)
        target = self.output / 'wine/bridge/lib/libopenxr_loader.dylib'
        self.assertEqual(target.read_bytes(), b'packaged loader')
        target.write_bytes(b'tampered')
        with self.assertRaisesRegex(ValueError, 'changed'):
            contract.steam_environment(value)

    def test_missing_bridge_leaves_no_partial_runtime(self):
        (self.bridge / 'x86_64-unix/wineopenxr.so').unlink()
        with self.assertRaises(ValueError):
            self.prepare()
        self.assertFalse(self.output.exists())
        self.assertFalse(self.prefixes.exists())

    def test_native_alyx_uses_threshold_run_and_filters_only_standard_prefix(self):
        runner = self.write(self.root, 'threshold-run')
        runner.chmod(0o755)
        self.document['framework_runner'] = str(runner)
        self.stack.write_text(json.dumps(self.document))
        stack = contract.load_stack(self.stack)
        with mock.patch.object(managed_runner, 'check_monado'):
            command = managed_runner.native_command(stack, managed_runner.STANDARD_ALYX + ['+load', 'quick'], {})
        self.assertEqual(command[:4], [str(runner), 'alyx', '--stack', str(self.stack)])
        self.assertEqual(command[-3:], ['--', '+load', 'quick'])
        self.assertNotIn('-steam', command)

    def test_monado_check_uses_launchagent_and_refuses_a_different_client(self):
        service = self.write(self.root, 'service', b'v25.1.0-1-gabcdef1\0')
        agent = self.root / 'Library/LaunchAgents/org.freedesktop.monado.service.plist'
        agent.parent.mkdir(parents=True)
        agent.write_bytes(plistlib.dumps({'ProgramArguments': [str(service)]}))
        with mock.patch('subprocess.run', return_value=mock.Mock(stdout='')):
            contract.check_monado(self.document, self.document['native']['runtime_json'], self.root)
        service.write_bytes(b'v25.1.0-2-g1234567\0')
        with self.assertRaisesRegex(ValueError, 'Activate'):
            contract.check_monado(self.document, self.document['native']['runtime_json'], self.root)

    def test_steam_stop_reaches_threshold_run_and_waits_for_it(self):
        marker = self.root / 'host-started'
        stopped = self.root / 'host-stopped'
        runner = self.write(self.root, 'threshold-run', ("#!/usr/bin/python3\n"
            "import os,signal,time\nfrom pathlib import Path\n"
            "def stop(*args):\n Path(os.environ['STOPPED']).write_text('stopped')\n raise SystemExit(0)\n"
            "signal.signal(signal.SIGTERM,stop)\n"
            "Path(os.environ['MARKER']).write_text(str(os.getpid()))\n"
            "while True: time.sleep(.05)\n").encode())
        runner.chmod(0o755)
        self.document['framework_runner'] = str(runner)
        self.stack.write_text(json.dumps(self.document))
        driver = "import sys; sys.path.insert(0,sys.argv.pop(1)); import managed_runner as m; m.check_monado=lambda *a:None; sys.exit(m.main())"
        process = subprocess.Popen([sys.executable, '-c', driver, str(ROOT / 'tools/steam_play'),
                                    '--stack', str(self.stack), '546560'],
                                   env=dict(os.environ, MWXR_ALYX='native', MARKER=str(marker), STOPPED=str(stopped)),
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            for _ in range(100):
                if marker.exists() or process.poll() is not None:
                    break
                time.sleep(.02)
            self.assertTrue(marker.exists())
            process.terminate()
            output, errors = process.communicate(timeout=5)
            self.assertEqual(process.returncode, 0, errors.decode())
            self.assertTrue(stopped.exists())
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
            if marker.exists() and not stopped.exists():
                try:
                    os.kill(int(marker.read_text()), signal.SIGTERM)
                except ProcessLookupError:
                    pass

    def test_prebuilt_install_is_side_by_side_and_handles_spaces(self):
        config = self.prepare()
        package = self.root / 'package'
        for name in ['bin/steam-play', 'steam-play/libmwxr_steam_compat_gate.dylib', 'steam-play/steam-compat-gate-scan',
                     'tools/steam_play/managed_runner.py', 'steam-play/compatibilitytools.d/macos-wine-xr-proton/mwxr-steam-play']:
            self.write(package, name)
        destination = self.root / 'installed payload'
        app = install.assemble(package, config, destination, sign=False)
        self.assertTrue(app.is_dir())
        runner = (destination / 'runner').read_text()
        self.assertIn("'" + str(destination / 'launch-stack.json') + "'", runner)
        self.assertEqual((destination / 'payload/compatibilitytools.d/macos-wine-xr-proton/runner').read_text(), str(destination / 'runner') + '\n')
        self.assertFalse(self.prefixes.exists())

    def prepare_public(self):
        for name in ['lib/wine/x86_64-unix/lsteamclient.so', 'lib/wine/x86_64-windows/lsteamclient.dll',
                     'lib/wine/i386-windows/lsteamclient.dll']:
            (self.wine / name).unlink()
        for name in ['run-in-process-openxr', 'run-openvr-game', 'run-windows-steam', 'install-windows-steam',
                     'windows-steam-env', 'openxr-launch-settings']:
            self.write(self.bridge, 'scripts/' + name + '.zsh')
        return prepare_runtime.prepare(self.stack, self.wine, self.bridge, self.loader, None,
                                       self.output, self.prefixes, relocate_binaries=False)

    def test_legacy_private_mode_and_new_public_default(self):
        self.assertEqual(contract.integration_mode(self.document), 'windows-steam')
        result = self.prepare()
        value = contract.load_stack(result)
        self.assertEqual(contract.integration_mode(value), 'native-steam')
        del value['steam_play']['integration']
        self.assertEqual(contract.integration_mode(value), 'native-steam')
        contract.steam_environment(value)

    def test_public_profile_uses_separate_prefix_and_has_no_stand_in(self):
        result = self.prepare_public()
        value = contract.load_stack(result)
        env = contract.steam_environment(value)
        self.assertEqual(contract.integration_mode(value), 'windows-steam')
        self.assertEqual(env['MWXR_IN_PROCESS_PREFIX'], str(self.prefixes / 'windows-steam'))
        self.assertNotIn('ALYX_STEAM_STAND_IN', env)
        self.assertFalse(self.prefixes.exists())
        with self.assertRaisesRegex(ValueError, 'private native-steam'):
            install.assemble(self.root / 'package', result, self.root / 'installation', sign=False)

    def test_public_runner_clears_private_redirect_and_uses_public_helper(self):
        result = self.prepare_public()
        with mock.patch.dict(os.environ, {'MWXR_LSTEAMCLIENT': '1', 'ALYX_STEAM_STAND_IN': '/private/stand-in', 'MWXR_ALYX': 'native'}), \
             mock.patch.object(managed_runner, 'check_monado'), \
             mock.patch.object(managed_runner, 'wait_for_command', return_value=0) as run:
            self.assertEqual(managed_runner.main(['--stack', str(result), '546560', '--executable', '/games/test.exe', '--openvr', 'argument with spaces']), 0)
        command, env = run.call_args.args
        self.assertTrue(command[1].endswith('/bridge/scripts/run-openvr-game.zsh'))
        self.assertEqual(command[-2:], ['/games/test.exe', 'argument with spaces'])
        self.assertNotIn('MWXR_LSTEAMCLIENT', env)
        self.assertNotIn('ALYX_STEAM_STAND_IN', env)
        self.assertNotIn('MWXR_ALYX', env)
        self.assertEqual(env['SteamAppId'], '546560')

    def test_mixed_public_private_profiles_are_rejected(self):
        value = contract.load_stack(self.prepare())
        value['windows_xr'] = {'integration': 'windows-steam'}
        with self.assertRaisesRegex(ValueError, 'separate'):
            contract.steam_environment(value)


if __name__ == '__main__':
    unittest.main()

#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Launch Steam games using a prepared stack, without a machine-specific rig wrapper."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys

from managed_stack import load_stack, native_selection, steam_environment, check_monado, integration_mode

STANDARD_ALYX = ['-vr', '-steam', '-noasserts', '-nopassiveasserts', '+map', 'startup']


def native_command(stack, arguments, environment):
    components, native = native_selection(stack)
    check_monado(stack, native['runtime_json'])
    runner = stack.get('framework_runner')
    if not runner and environment.get('THRESHOLD_RUNTIME'):
        runner = str(Path(environment['THRESHOLD_RUNTIME']) / 'bin/threshold-run')
    if not runner or not os.access(runner, os.X_OK):
        raise ValueError('Select an installed threshold-runtime provider (framework_runner or THRESHOLD_RUNTIME)')
    if arguments[:len(STANDARD_ALYX)] == STANDARD_ALYX:
        arguments = arguments[len(STANDARD_ALYX):]
    command = [runner, 'alyx', '--stack', stack['_path'], '--timeout', environment.get('ALYX_NATIVE_TIMEOUT', '43200')]
    if environment.get('MWXR_ALYX_COMPONENT_SET'):
        command += ['--components', environment['MWXR_ALYX_COMPONENT_SET']]
    elif environment.get('MWXR_ALYX_OPTIMIZED') == '1':
        raise ValueError('Use MWXR_ALYX_COMPONENT_SET to choose a named threshold-runtime component set')
    if environment.get('ALYX_NATIVE_RUNS'):
        from datetime import datetime, timezone
        command += ['--output', str(Path(environment['ALYX_NATIVE_RUNS']) / datetime.now(timezone.utc).strftime('steam-%Y%m%dT%H%M%S'))]
    return command + (['--'] + arguments if arguments else [])


def wait_for_command(command, environment):
    process = subprocess.Popen(command, env=environment)
    def stop(signum, frame):
        if process.poll() is None:
            # threshold-run handles SIGTERM by stopping and waiting for the guest group.
            process.send_signal(signal.SIGTERM)
    previous = {sig: signal.signal(sig, stop) for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT)}
    try:
        code = process.wait()
        return code if code >= 0 else 128 - code
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)


def main(argv=None):
    args = list(sys.argv[1:] if argv is None else argv)
    path = None
    if args[:1] == ['--stack']:
        if len(args) < 3:
            raise ValueError('usage: managed_runner.py --stack FILE APPID [game arguments]')
        path, args = args[1], args[2:]
    native_only = args[:1] == ['--native']
    if native_only:
        args = ['546560'] + args[1:]
    if not args or not args[0].isdigit():
        raise ValueError('Give a numeric Steam application ID')
    stack = load_stack(path)
    if not stack:
        raise ValueError('No Threshold XR launch stack is selected')
    mode = integration_mode(stack)  # reject mixed/unknown records before either launch path
    environment = dict(os.environ, THRESHOLD_LAUNCH_STACK=stack['_path'])
    if native_only or (args[0] == '546560' and stack.get('windows_xr') is None and environment.get('MWXR_ALYX') == 'native'):
        command = native_command(stack, args[1:], environment)
    else:
        environment.update(steam_environment(stack))
        check_monado(stack, environment['MWXR_NATIVE_RUNTIME_JSON'])
        if mode == 'windows-steam':
            from public_bridge import module
            public = module('managed_profiles')
            for key in public.NATIVE_ENV:
                environment.pop(key, None)
            if len(args) < 3 or args[1] != '--executable':
                raise ValueError('Windows Steam: give APPID --executable GAME.EXE [--openvr] [game arguments]')
            executable, extra = args[2], args[3:]
            action = 'openvr' if extra[:1] == ['--openvr'] else 'openxr'
            if action == 'openvr':
                extra = extra[1:]
            environment['SteamAppId'] = environment['SteamGameId'] = args[0]
            command = ['zsh', str(Path(environment['MWXR_PUBLIC_BRIDGE_ROOT']) / 'scripts' /
                ('run-openvr-game.zsh' if action == 'openvr' else 'run-in-process-openxr.zsh')), executable, *extra]
        else:
            environment.pop('MWXR_PUBLIC_BRIDGE_ROOT', None)
            bridge = Path(__file__).resolve().parents[2]
            command = [str(bridge / 'scripts/run-windows-game.zsh')] + args
    return wait_for_command(command, environment)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (ValueError, OSError, KeyError) as error:
        print(f'Steam Play: {error}', file=sys.stderr)
        sys.exit(2)

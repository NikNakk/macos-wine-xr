# SPDX-License-Identifier: MIT
"""Read the path/provenance contract exported by Threshold XR; never evaluate shell code."""
import json
import os
from pathlib import Path

SCHEMA = 'threshold-xr/launch-stack-1'


def load_stack(path=None, environment=None):
    env = os.environ if environment is None else environment
    explicit = path or env.get('THRESHOLD_LAUNCH_STACK')
    source = Path(explicit).expanduser() if explicit else Path.home() / 'Library/Application Support/Threshold XR/selected-launch-stack.json'
    if not source.exists() and not explicit:
        return None
    try:
        value = json.loads(source.read_text())
    except (OSError, ValueError) as error:
        raise ValueError(f'Cannot read launch stack {source}: {error}') from error
    if not isinstance(value, dict) or value.get('schema') != SCHEMA or not isinstance(value.get('id'), str) or not value['id']:
        raise ValueError(f'Unsupported or malformed launch stack: {source}')
    value['_path'] = str(source.resolve())
    return value


def native_selection(stack):
    if stack is None:
        return {}, {}
    native = stack.get('native')
    if not isinstance(native, dict) or not isinstance(native.get('components'), dict):
        raise ValueError('Selected stack has no native application configuration')
    components = native['components']
    if any(key not in ('xrizer', 'kk_icd', 'openxr_loader', 'vulkan_loader') for key in components):
        raise ValueError('Unknown native component in launch stack')
    for path in list(components.values()) + [native.get('runtime_json')]:
        if not isinstance(path, str) or not Path(path).is_absolute() or not Path(path).is_file():
            raise ValueError(f'Missing or invalid managed native component: {path}')
    if not isinstance(stack.get('monado_build_tag'), str) or not stack['monado_build_tag']:
        raise ValueError('Selected stack has no Monado build tag')
    return components, native


def steam_environment(stack):
    selection = stack.get('steam_play') if stack else None
    if not isinstance(selection, dict) or not isinstance(selection.get('environment'), dict):
        raise ValueError('Prepare a Steam Play compatibility runtime for this stack first')
    env = selection['environment']
    required = {'MWXR_IN_PROCESS_WINE', 'MWXR_IN_PROCESS_BUILD', 'MWXR_XRIZER_ROOT',
                'MWXR_NATIVE_RUNTIME_JSON', 'MWXR_WINDOWS_OPENXR_LOADER', 'ALYX_STEAM_STAND_IN',
                'MWXR_IN_PROCESS_PREFIX', 'MWXR_GAME_PREFIXES'}
    if set(env) != required or any(not isinstance(value, str) or not Path(value).is_absolute() for value in env.values()):
        raise ValueError('Invalid Steam Play environment in launch stack')
    files = [Path(env['MWXR_IN_PROCESS_WINE']) / relative for relative in [
        'lib/wine/x86_64-unix/ntdll.so', 'lib/wine/x86_64-windows/ntdll.dll',
        'lib/wine/i386-windows/ntdll.dll', 'lib/wine/x86_64-unix/winemetal.so',
        'lib/wine/x86_64-windows/d3d11.dll', 'lib/wine/x86_64-windows/dxgi.dll']]
    files += [Path(env['MWXR_IN_PROCESS_WINE']) / 'bin/wine',
             Path(env['MWXR_IN_PROCESS_WINE']) / 'bin/wineserver',
             Path(env['MWXR_IN_PROCESS_WINE']) / 'lib/wine/x86_64-windows/lsteamclient.dll',
             Path(env['MWXR_IN_PROCESS_WINE']) / 'lib/wine/i386-windows/lsteamclient.dll',
             Path(env['MWXR_IN_PROCESS_WINE']) / 'lib/wine/x86_64-unix/lsteamclient.so',
             Path(env['MWXR_IN_PROCESS_BUILD']) / 'x86_64-windows/wineopenxr.dll',
             Path(env['MWXR_IN_PROCESS_BUILD']) / 'x86_64-unix/wineopenxr.so',
             Path(env['MWXR_XRIZER_ROOT']) / 'bin/vrclient_x64.dll',
             Path(env['MWXR_XRIZER_ROOT']) / 'openvr_api.dll',
             Path(env['MWXR_NATIVE_RUNTIME_JSON']), Path(env['MWXR_WINDOWS_OPENXR_LOADER']),
             Path(env['ALYX_STEAM_STAND_IN'])]
    for path in files:
        if not path.is_file():
            raise ValueError(f'Missing managed Steam Play file: {path}')
    import hashlib
    hashes = selection.get('file_hashes', {})
    for path in files:
        expected = hashes.get(str(path))
        if not expected:
            raise ValueError(f'Missing recorded hash for managed Steam Play file: {path}')
        digest = hashlib.sha256()
        with path.open('rb') as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b''):
                digest.update(block)
        if digest.hexdigest() != expected:
            raise ValueError(f'Managed Steam Play file changed: {path}')
    return dict(env)


def check_monado(stack, runtime_json, home=None):
    import plistlib
    import re
    home = Path.home() if home is None else Path(home)
    agent_path = home / 'Library/LaunchAgents/org.freedesktop.monado.service.plist'
    agent = plistlib.loads(agent_path.read_bytes())
    executable = agent.get('Program') or next(iter(agent.get('ProgramArguments', [])), None)
    if not executable:
        raise ValueError('Monado LaunchAgent has no service executable')
    tag = stack.get('monado_build_tag')
    document = json.loads(Path(runtime_json).read_text())
    library = (Path(runtime_json).parent / document['runtime']['library_path']).resolve()
    pattern = re.compile(rb'v\d+\.\d+\.\d+(?:-\d+-g[0-9a-f]+)?(?:-dirty)?(?=\0)')
    if not isinstance(tag, str) or not tag:
        raise ValueError('Selected stack has no Monado build tag')
    for binary in (Path(executable), library):
        match = pattern.search(binary.read_bytes())
        if not match or match.group().decode() != tag:
            raise ValueError(f'Selected stack needs Monado {tag}; {binary} has a different build tag. Activate the matching service before launching.')

    import subprocess
    for pid in subprocess.run(['/usr/bin/pgrep', '-x', 'monado-service'], capture_output=True, text=True, timeout=5).stdout.split():
        running = subprocess.run(['/bin/ps', '-o', 'comm=', '-p', pid], capture_output=True, text=True, timeout=5).stdout.strip()
        if running and Path(running).resolve() != Path(executable).resolve():
            raise ValueError(f'Another Monado service is running: {pid} {running}')

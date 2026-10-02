#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Check native-ID regeneration and fail closed on incompatible native schemas."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tools'))
from generate_monado_wire import LOCAL, load_calls


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--monado-source', required=True, type=Path)
    args = ap.parse_args()
    native = dict(load_calls(args.monado_source))
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        proto = root / 'src/xrt/ipc/shared/proto'
        proto.mkdir(parents=True)
        subprocess.run(['git', 'init', '-q', str(root)], check=True)
        subprocess.run(['git', '-C', str(root), '-c', 'user.name=Test', '-c', 'user.email=test@example.invalid', 'commit', '--allow-empty', '-qm', 'fixture'], check=True)
        output = root / 'wire.h'
        command = [sys.executable, str(REPO / 'tools/generate_monado_wire.py'), '--monado-source', str(root), '--output', str(output)]
        # Remove every transitional native command. Native IDs must shift while Wine IDs remain frozen.
        clean = {name: spec for name, spec in native.items() if name not in LOCAL}
        schema = proto / '10-native.json'
        schema.write_text(json.dumps(clean))
        subprocess.run(command, check=True, capture_output=True)
        generated = output.read_text()
        for i, name in enumerate(clean, 1):
            assert f'#define NATIVE_IPC_{name.upper()} {i}\n' in generated
        for i, name in enumerate(json.loads((REPO / 'protocol/monado-wine.json').read_text()), 1):
            assert f'IPC_{name.upper()} = {i},' in generated
        assert 'case IPC_INSTANCE_GET_SHM_CHUNK: return NATIVE_' not in generated
        for name in ('instance_is_system_available', 'compositor_layer_sync_with_semaphore'):
            altered = dict(clean)
            altered[name] = {'in': [{'name': 'unexpected', 'type': 'uint32_t'}]}
            schema.write_text(json.dumps(altered))
            result = subprocess.run(command, capture_output=True, text=True)
            assert result.returncode != 0 and name in result.stderr
        altered = dict(clean)
        del altered['swapchain_import_metal']
        schema.write_text(json.dumps(altered))
        result = subprocess.run(command, capture_output=True, text=True)
        assert result.returncode != 0 and 'swapchain_import_metal' in result.stderr
    print('Native command removal/ID shifts and missing/incompatible native schemas checked')


if __name__ == '__main__':
    main()

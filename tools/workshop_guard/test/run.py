#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Cross-compile and exercise real Win32 debugger entry/return interception."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--wine', required=True)
p.add_argument('--prefix', required=True)
p.add_argument('--out', required=True)
a = p.parse_args()
source = Path(__file__).resolve().parent
out = Path(a.out).resolve()
out.mkdir(parents=True, exist_ok=True)
compiler = 'x86_64-w64-mingw32-g++'
subprocess.run([compiler, '-std=c++17', '-O1', '-fno-optimize-sibling-calls',
                '-shared', '-static', str(source/'fixture_dll.cpp'),
                '-o', str(out/'guard_fixture.dll')], check=True)
subprocess.run([compiler, '-std=c++17', '-O2', '-static',
                str(source/'fixture_main.cpp'), '-o', str(out/'guard-fixture.exe')], check=True)
dll = out/'guard_fixture.dll'
nm = subprocess.check_output(['x86_64-w64-mingw32-nm', str(dll)], text=True)
address = int(re.search(r'^([0-9a-fA-F]+) T WorkItem$', nm, re.M)[1], 16)
pe = subprocess.check_output(['x86_64-w64-mingw32-objdump', '-p', str(dll)], text=True)
base = int(re.search(r'ImageBase\s+([0-9a-fA-F]+)', pe)[1], 16)
hashval = hashlib.sha256(dll.read_bytes()).hexdigest()
subprocess.run([compiler, '-std=c++17', '-O2', '-static',
                f'-DMWXR_GUARD_FIXTURE_HASH="{hashval}"',
                f'-DMWXR_GUARD_FIXTURE_RVA={address-base}', str(source.parent/'main.cpp'),
                '-lbcrypt', '-o', str(out/'fixture-guard.exe')], check=True)
env = dict(os.environ, WINEPREFIX=str(Path(a.prefix).resolve()), WINEDEBUG='-all')
wine = str(Path(a.wine).resolve())
for label, duration, fixture_args in [('graph', 30, []), ('restore', 8, ['--detach']), ('refuse', 8, ['--detach'])]:
    original = dll.read_bytes()
    if label == 'refuse':
        dll.write_bytes(original + b'\0')
    with (out/f'{label}-guard.log').open('w') as g, (out/f'{label}-fixture.log').open('w') as f:
        guard = subprocess.Popen([wine, str(out/'fixture-guard.exe'), '0', str(duration)],
                                 env=env, stdout=g, stderr=subprocess.STDOUT)
        fixture = subprocess.Popen([wine, str(out/'guard-fixture.exe'), *fixture_args],
                                   env=env, stdout=f, stderr=subprocess.STDOUT)
        try:
            fixture_code = fixture.wait(timeout=60)
            guard_code = guard.wait(timeout=60)
        except subprocess.TimeoutExpired:
            # End only our fixture first; its debugger can then exit safely.
            fixture.kill()
            fixture.wait()
            try:
                guard.wait(timeout=10)
            except subprocess.TimeoutExpired:
                guard.kill()
                guard.wait()
            raise
        finally:
            if label == 'refuse':
                dll.write_bytes(original)
    guard_log = (out/f'{label}-guard.log').read_text()
    fixture_log = (out/f'{label}-fixture.log').read_text()
    if label == 'refuse':
        assert fixture_code == 0 and guard_code == 2, (label, fixture_code, guard_code)
        assert 'REFUSED' in guard_log and 'ARMED pid=' not in guard_log, guard_log
        assert 'RESTORE_FIXTURE PASS' in fixture_log, fixture_log
        print('refuse: PASS')
        continue
    assert fixture_code == guard_code == 0, (label, fixture_code, guard_code)
    assert 'ARMED' in guard_log and 'failed=0' in guard_log, guard_log
    if label == 'graph':
        assert 'FIXTURE PASS' in fixture_log and 'COMPLETED 32' in fixture_log, fixture_log
        assert 'reason=cycle' in guard_log and 'reason=depth-limit' in guard_log, guard_log
    else:
        assert 'RESTORE_FIXTURE PASS' in fixture_log, fixture_log
        assert 'RESTORED entry and active return addresses' in guard_log, guard_log
        assert 'DETACHED success=1' in guard_log, guard_log
        assert 'STATUS calls=1' in guard_log or 'SUMMARY calls=1' in guard_log, guard_log
    print(f'{label}: PASS')

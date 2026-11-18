# Copyright 2026 Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Load shared public bridge tools from the pinned submodule, never a moving checkout."""
import importlib.util
from pathlib import Path
import sys

ROOT=Path(__file__).resolve().parents[2]/'components/threshold-wine-xr'

def module(name):
    path=ROOT/'tools'/f'{name}.py'
    if not path.is_file():
        raise ValueError('Initialize the public bridge: git submodule update --init components/threshold-wine-xr')
    key='threshold_public_'+name
    if key not in sys.modules:
        if str(ROOT/'tools') not in sys.path:sys.path.insert(0,str(ROOT/'tools'))
        spec=importlib.util.spec_from_file_location(key,path)
        result=importlib.util.module_from_spec(spec)
        sys.modules[key]=result
        spec.loader.exec_module(result)
    return sys.modules[key]

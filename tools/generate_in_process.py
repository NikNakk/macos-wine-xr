#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Configure Proton's unchanged generator for the initial OpenXR 1.0 gate."""
import argparse
import logging
import importlib.machinery
import importlib.util
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--xml', required=True)
p.add_argument('--output', required=True)
a = p.parse_args()
source = Path(__file__).resolve().parents[1] / 'src/in_process/proton/make_openxr'
loader = importlib.machinery.SourceFileLoader('proton_openxr', str(source))
spec = importlib.util.spec_from_loader(loader.name, loader)
g = importlib.util.module_from_spec(spec)
loader.exec_module(g)
g.LOGGER.setLevel(logging.WARNING)
g.WINE_XR_VERSION = (1, 0)
g.NOT_OUR_FUNCTIONS = g.NOT_OUR_FUNCTIONS + ["xrCreateApiLayerInstance"]
# Graphics is intentionally unavailable until direct-object interop is built.
g.XrRegistry._is_extension_supported = lambda self, name: False
g.MANUAL_UNIX_THUNKS = {'xrCreateInstance', 'xrEnumerateInstanceExtensionProperties', 'xrEnumerateApiLayerProperties',
                        'xrCreateSession', 'xrCreateSwapchain', 'xrDestroyInstance'}
g.FUNCTION_OVERRIDES = {name: {'dispatch': name not in
    {'xrCreateInstance', 'xrEnumerateInstanceExtensionProperties', 'xrEnumerateApiLayerProperties', 'xrGetInstanceProcAddr'}}
    for name in g.MANUAL_UNIX_THUNKS | {'xrGetInstanceProcAddr'}}
g.MANUAL_LOADER_FUNCTIONS = {'xrGetInstanceProcAddr', 'xrNegotiateLoaderRuntimeInterface',
    'xrCreateApiLayerInstance', 'xrNegotiateLoaderApiLayerInterface'}
g.MANUAL_LOADER_THUNKS = {'xrCreateInstance', 'xrDestroyInstance'}
registry = g.XrRegistry(a.xml)
generator = g.XrGenerator(registry)
out = Path(a.output)
out.mkdir(parents=True, exist_ok=True)
for name, method in [
    ('wineopenxr.h', generator.generate_openxr_h),
    ('openxr_thunks.c', generator.generate_thunks_c),
    ('loader_thunks.c', generator.generate_loader_thunks_c),
    ('loader_thunks.h', generator.generate_loader_thunks_h),
]:
    with (out / name).open('w') as f:
        method(f)
with (out / 'openxr_thunks.h').open('w') as f:
    generator.generate_thunks_h(f, 'wine_')

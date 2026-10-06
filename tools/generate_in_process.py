#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Configure Proton's unchanged generator for the OpenXR 1.0 Metal adaptation."""
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
# Generate Windows D3D11 and internal native Metal structures. Only D3D11
# and Win32 time conversion are advertised; native Metal entry points are
# hidden from Windows GIPA.
g.XrRegistry._is_extension_supported = lambda self, name: name in {
    'XR_KHR_D3D11_enable', 'XR_KHR_metal_enable', 'XR_KHR_win32_convert_performance_counter_time'}
g.MANUAL_UNIX_THUNKS = {'xrCreateInstance', 'xrEnumerateInstanceExtensionProperties', 'xrEnumerateApiLayerProperties',
                        'xrCreateSession', 'xrCreateSwapchain', 'xrDestroyInstance', 'xrDestroySession',
    'xrGetD3D11GraphicsRequirementsKHR', 'xrEnumerateSwapchainFormats', 'xrReleaseSwapchainImage',
    'xrConvertWin32PerformanceCounterToTimeKHR', 'xrConvertTimeToWin32PerformanceCounterKHR'}
g.FUNCTION_OVERRIDES = {name: {'dispatch': name not in
    {'xrCreateInstance', 'xrEnumerateInstanceExtensionProperties', 'xrEnumerateApiLayerProperties', 'xrGetInstanceProcAddr', 'xrGetD3D11GraphicsRequirementsKHR',
     'xrConvertWin32PerformanceCounterToTimeKHR', 'xrConvertTimeToWin32PerformanceCounterKHR'}}
    for name in g.MANUAL_UNIX_THUNKS | {'xrGetInstanceProcAddr'}}
g.MANUAL_LOADER_FUNCTIONS = {'xrGetInstanceProcAddr', 'xrNegotiateLoaderRuntimeInterface',
    'xrCreateApiLayerInstance', 'xrNegotiateLoaderApiLayerInterface'}
g.MANUAL_LOADER_THUNKS = {'xrCreateInstance', 'xrDestroyInstance', 'xrCreateSession', 'xrAcquireSwapchainImage',
    'xrDestroySession', 'xrCreateSwapchain', 'xrDestroySwapchain', 'xrEnumerateSwapchainImages',
    'xrReleaseSwapchainImage', 'xrPollEvent', 'xrEndFrame', 'xrGetD3D11GraphicsRequirementsKHR'}
g.MANUAL_LOADER_FUNCTIONS.discard('xrGetD3D11GraphicsRequirementsKHR')
g.ALLOWED_PROTECTS = g.ALLOWED_PROTECTS + ['XR_USE_GRAPHICS_API_METAL']
g.FUNCTION_OVERRIDES['xrCreateInstance']['extra_param'] = 'wine_instance'
g.FUNCTION_OVERRIDES['xrCreateSession']['extra_param'] = 'wine_session'

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

# One extra unix entry, after the generated OpenXR thunks, for the
# renderer-neutral graphics interop helper (graphics_interop_native.h).
def patch(name, old, new, count):
    path = out / name
    text = path.read_text()
    if text.count(old) != count:
        raise SystemExit(f'{name}: expected {count} unix-call table anchor(s)')
    path.write_text(text.replace(old, new))
patch('loader_thunks.h', '    unix_count,\n', '    unix_mw_graphics_native,\n    unix_count,\n', 1)
patch('openxr_thunks.c', '};\nC_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == unix_count);',
      '    (unixlib_entry_t)mw_graphics_native_call,\n};\nC_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == unix_count);', 1)
patch('openxr_thunks.c', 'const unixlib_entry_t __wine_unix_call_funcs[] =',
      'extern int mw_graphics_native_call(void *params);\nconst unixlib_entry_t __wine_unix_call_funcs[] =', 1)

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
# Generate Windows D3D11/Vulkan and internal native Metal structures. Win32
# time conversion and display refresh rates are advertised; native Metal entry
# points are hidden from Windows GIPA.
g.XrRegistry._is_extension_supported = lambda self, name: name in {
    'XR_KHR_D3D11_enable', 'XR_KHR_vulkan_enable', 'XR_KHR_vulkan_enable2', 'XR_KHR_metal_enable', 'XR_KHR_win32_convert_performance_counter_time',
    'XR_FB_display_refresh_rate', 'XR_KHR_visibility_mask', 'XR_FB_swapchain_update_state', 'XR_FB_foveation',
    'XR_FB_foveation_configuration', 'XR_META_foveation_eye_tracked'}
g.MANUAL_UNIX_THUNKS = {'xrCreateInstance', 'xrEnumerateInstanceExtensionProperties', 'xrEnumerateApiLayerProperties',
                        'xrCreateSession', 'xrCreateSwapchain', 'xrDestroyInstance', 'xrDestroySession',
    'xrGetD3D11GraphicsRequirementsKHR', 'xrEnumerateSwapchainFormats', 'xrReleaseSwapchainImage',
    'xrEnumerateViewConfigurations', 'xrEnumerateViewConfigurationViews',
    'xrConvertWin32PerformanceCounterToTimeKHR', 'xrConvertTimeToWin32PerformanceCounterKHR'}
g.FUNCTION_OVERRIDES = {name: {'dispatch': name not in
    {'xrCreateInstance', 'xrEnumerateInstanceExtensionProperties', 'xrEnumerateApiLayerProperties', 'xrGetInstanceProcAddr', 'xrGetD3D11GraphicsRequirementsKHR',
     'xrConvertWin32PerformanceCounterToTimeKHR', 'xrConvertTimeToWin32PerformanceCounterKHR'}}
    for name in g.MANUAL_UNIX_THUNKS | {'xrGetInstanceProcAddr'}}
g.MANUAL_LOADER_FUNCTIONS = {'xrGetInstanceProcAddr', 'xrNegotiateLoaderRuntimeInterface',
    'xrCreateApiLayerInstance', 'xrNegotiateLoaderApiLayerInterface'}
g.MANUAL_LOADER_THUNKS = {'xrCreateInstance', 'xrDestroyInstance', 'xrCreateSession', 'xrAcquireSwapchainImage', 'xrWaitSwapchainImage',
    'xrDestroySession', 'xrCreateSwapchain', 'xrDestroySwapchain', 'xrEnumerateSwapchainImages',
    'xrReleaseSwapchainImage', 'xrPollEvent', 'xrEndFrame', 'xrGetD3D11GraphicsRequirementsKHR'}
g.MANUAL_LOADER_FUNCTIONS.discard('xrGetD3D11GraphicsRequirementsKHR')
g.ALLOWED_PROTECTS = g.ALLOWED_PROTECTS + ['XR_USE_GRAPHICS_API_METAL']
g.FUNCTION_OVERRIDES['xrCreateInstance']['extra_param'] = 'wine_instance'
g.FUNCTION_OVERRIDES['xrCreateSession']['extra_param'] = 'wine_session'

# Vulkan adaptation lives entirely in PE and calls Wine's Vulkan API. Never pass
# Wine-wrapped Vulkan handles to the native OpenXR loader.
vulkan_functions = {'xrGetVulkanGraphicsRequirementsKHR', 'xrGetVulkanGraphicsRequirements2KHR',
    'xrGetVulkanGraphicsDeviceKHR', 'xrGetVulkanGraphicsDevice2KHR',
    'xrGetVulkanInstanceExtensionsKHR', 'xrGetVulkanDeviceExtensionsKHR',
    'xrCreateVulkanInstanceKHR', 'xrCreateVulkanDeviceKHR'}
g.MANUAL_LOADER_FUNCTIONS |= vulkan_functions
g.ALLOWED_PROTECTS += ['XR_USE_GRAPHICS_API_VULKAN']

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
# A second one for XR_MNDX_display_distortion, which is not in the Khronos
# registry (mndx_display_distortion.h).
patch('loader_thunks.h', '    unix_count,\n',
      '    unix_mw_vulkan_native,\n    unix_mw_graphics_native,\n    unix_mw_display_distortion,\n    unix_mw_foveation,\n    unix_mw_gaze_foveation,\n    unix_count,\n', 1)
patch('openxr_thunks.c', '};\nC_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == unix_count);',
      '    (unixlib_entry_t)mw_vulkan_native_call,\n    (unixlib_entry_t)mw_graphics_native_call,\n    (unixlib_entry_t)mw_display_distortion_call,\n'
      '    (unixlib_entry_t)mw_foveation_call,\n    (unixlib_entry_t)mw_gaze_foveation_call,\n'
      '};\nC_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == unix_count);', 1)
patch('openxr_thunks.c', 'const unixlib_entry_t __wine_unix_call_funcs[] =',
      'extern int mw_vulkan_native_call(void *params);\nextern int mw_graphics_native_call(void *params);\nextern int mw_display_distortion_call(void *params);\n'
      'extern int mw_foveation_call(void *params);\nextern int mw_gaze_foveation_call(void *params);\n'
      'const unixlib_entry_t __wine_unix_call_funcs[] =', 1)

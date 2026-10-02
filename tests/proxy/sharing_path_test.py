#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from check_sharing_path import check

zero = 'host: swapchain=131072 images=4 strategy=shared-metal-zero-copy\n'
blit = 'host: swapchain=131073 images=3 strategy=gpu-blit\n'
assert check(zero, 'shared-metal-zero-copy') == 1
assert check(blit, 'gpu-blit') == 1
for log in ('', blit, zero + blit):
    try:
        check(log, 'shared-metal-zero-copy')
    except RuntimeError:
        pass
    else:
        raise AssertionError('An absent or blitting Monado swapchain must fail the sharing check')
print('Monado sharing assertion rejects missing, GPU-blit and mixed sharing paths')

#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Verify every native-host swapchain selected the expected sharing path."""
import argparse
from pathlib import Path
import re


def check(text, expected):
    selected = re.findall(r'^host: swapchain=\d+ images=\d+ strategy=(\S+)$', text, re.MULTILINE)
    if not selected or any(path != expected for path in selected):
        raise RuntimeError(f'Expected {expected} for every swapchain; selected: {selected}')
    return len(selected)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--log', required=True, type=Path)
    ap.add_argument('--expected', required=True, choices=['shared-metal-zero-copy', 'gpu-blit'])
    args = ap.parse_args()
    count = check(args.log.read_text(errors='replace'), args.expected)
    print(f'Sharing path verified: {count} swapchains selected {args.expected}')


if __name__ == '__main__':
    main()

#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# Builds mwxr-uevr-inject.exe (src/windows/uevr_inject.c), which run-windows-game.zsh
# runs beside a game with MWXR_UEVR=1 (docs/uevr.md).
set -euo pipefail
repo=${0:A:h:h}
out=${MWXR_UEVR_INJECTOR:-${repo}/build-in-process/uevr/mwxr-uevr-inject.exe}
mkdir -p "${out:h}"
x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -municode -static \
 "${repo}/src/windows/uevr_inject.c" -lshell32 -o "${out}"
print "Built ${out}"

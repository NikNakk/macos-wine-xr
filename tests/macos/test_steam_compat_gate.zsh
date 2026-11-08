#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Checks that the Steam Play gate (src/macos/steam_compat_gate.c) finds exactly one site in
# the installed macOS Steam client, without patching or starting it. Run it after a Steam
# client update: a failure means Steam Play.app will start Steam with Steam Play off, a
# warning that games chosen or bought while Steam runs wait for a restart.
# Usage: test_steam_compat_gate.zsh [steamclient.dylib]
set -euo pipefail
repo=${0:A:h:h:h}
client=${1:-$HOME/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamclient.dylib}
work=$(mktemp -d "${TMPDIR:-/tmp}/steam-compat-gate.XXXXXX")
trap 'rm -rf "${work}"' EXIT
clang -Wall -Wextra -Werror -O2 -arch arm64 -DMWXR_GATE_SCAN_MAIN \
  "${repo}/src/macos/steam_compat_gate.c" "${repo}/src/macos/steam_live_mapping.c" -o "${work}/scan"
lipo -thin arm64 "${client}" -output "${work}/steamclient.dylib" 2>/dev/null || cp "${client}" "${work}/steamclient.dylib"
MWXR_STEAM_COMPAT_GATE_LOG=${work}/scan.log "${work}/scan" "${work}/steamclient.dylib"
cat "${work}/scan.log"
sites=$(grep -c candidate "${work}/scan.log" || true)
[[ ${sites} == 1 ]] && ! grep -q 'expected 1' "${work}/scan.log" || { print -u2 "FAIL: ${sites} sites"; exit 1; }
# Live mapping (src/macos/steam_live_mapping.c) is optional: without it, changes wait for a restart.
grep -q 'live mapping: AddMapping at' "${work}/scan.log" || print -u2 "WARN: live mapping off for this client"
print "PASS"

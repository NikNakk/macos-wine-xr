#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# Assemble a source-backed Steam Play payload. Never install or start Steam here.
set -euo pipefail
repo=${0:A:h:h}
(( $# == 1 )) || { print -u2 'usage: package-steam-play.zsh NEW_OUTPUT_DIRECTORY'; exit 2; }
output=${1:A}
[[ ! -e ${output} ]] || { print -u2 "Refusing to overwrite ${output}"; exit 1; }
mkdir -p "${output:h}"
stage=$(mktemp -d "${output:h}/.steam-play.XXXXXX")
trap 'rm -rf "${stage}"' EXIT
mkdir -p "${stage}/bin" "${stage}/steam-play/compatibilitytools.d/macos-wine-xr-proton"
clang -O2 -Wall -Wextra -arch arm64 -arch x86_64 -mmacosx-version-min=14.0 -dynamiclib \
 -install_name @rpath/libmwxr_steam_compat_gate.dylib \
 "${repo}/src/macos/steam_compat_gate.c" "${repo}/src/macos/steam_ui_patch.c" \
 "${repo}/src/macos/steam_restart.c" "${repo}/src/macos/steam_live_mapping.c" \
 -o "${stage}/steam-play/libmwxr_steam_compat_gate.dylib"
clang -O2 -Wall -Wextra -arch arm64 -arch x86_64 -mmacosx-version-min=14.0 \
 "${repo}/src/macos/steam_play_launcher.c" -o "${stage}/bin/steam-play"
clang -O2 -Wall -Wextra -arch arm64 -mmacosx-version-min=14.0 -DMWXR_GATE_SCAN_MAIN \
 "${repo}/src/macos/steam_compat_gate.c" "${repo}/src/macos/steam_live_mapping.c" \
 -o "${stage}/steam-play/steam-compat-gate-scan"
cp "${repo}/tools/steam_play/"{steam_play_games.py,uevr_profile.py} \
 "${repo}/tools/steam_games/steam_games.py" "${repo}/src/macos/steam_ui_options.js" "${stage}/steam-play/"
cp "${repo}/tools/steam_play/"{compatibilitytool.vdf,toolmanifest.vdf,mwxr-steam-play} \
 "${stage}/steam-play/compatibilitytools.d/macos-wine-xr-proton/"
cp -R "${repo}/scripts" "${repo}/tools" "${stage}/"
cp "${repo}/LICENSE" "${stage}/LICENSE"
# Supply the exact sources/build recipe for the launcher binaries in this archive.
mkdir -p "${stage}/sources/steam-play/src/macos" "${stage}/sources/steam-play/scripts"
cp "${repo}/src/macos/"steam_*.[ch] "${stage}/sources/steam-play/src/macos/"
cp "${repo}/src/macos/steam_ui_options.js" "${stage}/sources/steam-play/src/macos/"
cp "${repo}/scripts/package-steam-play.zsh" "${stage}/sources/steam-play/scripts/"
cp -R "${repo}/tools" "${stage}/sources/steam-play/"
cp "${repo}/LICENSE" "${stage}/sources/steam-play/"
find "${stage}" -name __pycache__ -type d -prune -exec rm -rf {} +
find "${stage}" -name .DS_Store -delete
codesign --force --sign - "${stage}/bin/steam-play" "${stage}/steam-play/libmwxr_steam_compat_gate.dylib" \
 "${stage}/steam-play/steam-compat-gate-scan"
python3 -c 'import sys; from pathlib import Path; sys.path.insert(0, sys.argv[1]); from package import public_distribution_check; public_distribution_check(Path(sys.argv[2]))' \
 "${repo}/tools/threshold_release" "${stage}"
mv "${stage}" "${output}"
print "Packaged ${output}; no Wine, lsteamclient or Valve client binaries included."

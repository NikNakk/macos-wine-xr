#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# Private Wine/font and xrizer/Talos fixes; do not modify the installed copies.
set -euo pipefail
repo=${0:A:h:h}
wine_source=${MWXR_VULKAN_WINE_SOURCE:-$HOME/Code/wine-crossover/runtime-lsteamclient}
xrizer_source=${MWXR_XRIZER_SOURCE:-$HOME/Code/xrizer}
wine_target=${repo}/build-in-process/vulkan-wine
source_target=${repo}/build-in-process/xrizer-vulkan-src
xrizer_target=${repo}/build-in-process/vulkan-xrizer
[[ -d ${wine_target} ]] || cp -cR "${wine_source}" "${wine_target}"
python3 - "${wine_target}/lib/wine/x86_64-unix/win32u.so" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
bad = b'\tlibfreetype.dylib (compatibility version 27.0.0, current version 27.2.0)'
good = b'libfreetype.6.dylib'
data = p.read_bytes()
if data.count(bad) == 1:
    p.write_bytes(data.replace(bad, good.ljust(len(bad), b'\0')))
elif data.count(good) != 1:
    raise SystemExit('Unrecognised FreeType soname; refusing to modify this Wine binary')
print('Private Wine uses libfreetype.6.dylib')
PY
mkdir -p "${source_target}"
rsync -a --exclude .git --exclude target "${xrizer_source}/" "${source_target}/"
patch --batch -p1 -d "${source_target}" < "${repo}/patches/xrizer/0001-recover-unset-vulkan-queue-family.patch"
mingw_root=$(x86_64-w64-mingw32-g++ -print-sysroot)
mingw_version=$(x86_64-w64-mingw32-g++ -dumpversion)
export BINDGEN_EXTRA_CLANG_ARGS=${BINDGEN_EXTRA_CLANG_ARGS:-"--sysroot=${mingw_root} -isystem ${mingw_root}/x86_64-w64-mingw32/include/c++/${mingw_version} -isystem ${mingw_root}/x86_64-w64-mingw32/include/c++/${mingw_version}/x86_64-w64-mingw32 -isystem ${mingw_root}/x86_64-w64-mingw32/include"}
export RUSTC=${RUSTC:-$(rustup which rustc)} XRIZER_VERSION=macos-vulkan-queue-fix
export CARGO_TARGET_DIR=${repo}/build-in-process/xrizer-vulkan-build
export CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=x86_64-w64-mingw32-gcc
rustup run stable cargo build --offline --manifest-path "${source_target}/Cargo.toml" \
 --release --target x86_64-pc-windows-gnu --no-default-features --features monado
[[ -d ${xrizer_target} ]] || cp -cR "$HOME/Windows/openvr-runtimes/xrizer" "${xrizer_target}"
cp "${CARGO_TARGET_DIR}/x86_64-pc-windows-gnu/release/xrizer.dll" "${xrizer_target}/bin/vrclient_x64.dll"
print "Prepared ${wine_target} and ${xrizer_target} for scripts/test-vulkan-game.zsh"

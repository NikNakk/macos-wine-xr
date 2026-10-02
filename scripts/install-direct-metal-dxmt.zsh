#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=${0:A:h:h}
: ${DXMT_BUILD_DIR:?built DXMT directory with direct-object interface}
: ${MWXR_WINE_RUNTIME:?source Wine 11.10 runtime, preserved}
root=${repo}/build-in-process/wine-direct
[[ ${MWXR_WINE_RUNTIME:A} != ${root:A} ]] || { print -u2 'Source and destination must differ'; exit 1; }
for artifact in d3d11/d3d11.dll d3d10/d3d10core.dll dxgi/dxgi.dll winemetal/winemetal.dll winemetal/unix/winemetal.so; do
 [[ -s ${DXMT_BUILD_DIR}/src/${artifact} ]] || { print -u2 "Missing ${artifact}"; exit 1; }
done
if [[ ! -x ${root}/bin/wine ]]; then
 # APFS clone where available; a normal copy is safe on other filesystems.
 cp -cR "${MWXR_WINE_RUNTIME}" "${root}" || cp -R "${MWXR_WINE_RUNTIME}" "${root}"
fi
for artifact in d3d11/d3d11.dll d3d10/d3d10core.dll dxgi/dxgi.dll winemetal/winemetal.dll; do
 cp "${DXMT_BUILD_DIR}/src/${artifact}" "${root}/lib/wine/x86_64-windows/"
done
cp "${DXMT_BUILD_DIR}/src/winemetal/unix/winemetal.so" "${root}/lib/wine/x86_64-unix/"
print "Direct-object DXMT installed into private runtime ${root}"

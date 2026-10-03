#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# Overlays Apple's Game Porting Toolkit D3DMetal onto a copy of a CrossOver
# (CodeWeavers FOSS source) Wine install, as Silo does. Apple's files are taken
# from a redist/lib tree the user extracted from their own GPTK .dmg; nothing
# Apple-licensed is stored in this repository. The source install is unchanged.
set -euo pipefail
: ${MWXR_CROSSOVER_INSTALL:?make install prefix of CrossOver-source Wine (x86_64)}
: ${MWXR_GPTK_LIB:?redist/lib from the GPTK evaluation-environment .dmg}
: ${MWXR_GPTK_RUNTIME:?destination runtime directory}
[[ ${MWXR_CROSSOVER_INSTALL:A} != ${MWXR_GPTK_RUNTIME:A} ]] || { print -u2 'Source and destination must differ'; exit 1; }
[[ -f ${MWXR_GPTK_LIB}/external/libd3dshared.dylib && -d ${MWXR_GPTK_LIB}/external/D3DMetal.framework ]] || {
 print -u2 'MWXR_GPTK_LIB must contain external/libd3dshared.dylib and external/D3DMetal.framework'; exit 1
}
rm -rf "${MWXR_GPTK_RUNTIME}"
cp -cR "${MWXR_CROSSOVER_INSTALL}" "${MWXR_GPTK_RUNTIME}" 2>/dev/null || cp -R "${MWXR_CROSSOVER_INSTALL}" "${MWXR_GPTK_RUNTIME}"
mkdir -p "${MWXR_GPTK_RUNTIME}/lib/external"
cp -R "${MWXR_GPTK_LIB}/external/." "${MWXR_GPTK_RUNTIME}/lib/external/"
# Wine pairs each PE module with the unix .so in its own tree; GPTK's are
# relative links to libd3dshared, which loads D3DMetal.framework beside them.
for dll in "${MWXR_GPTK_LIB}"/wine/x86_64-windows/*.dll; do
 module=${dll:t:r}
 cp "${dll}" "${MWXR_GPTK_RUNTIME}/lib/wine/x86_64-windows/"
 rm -f "${MWXR_GPTK_RUNTIME}/lib/wine/x86_64-unix/${module}.so"
 ln -s ../../external/libd3dshared.dylib "${MWXR_GPTK_RUNTIME}/lib/wine/x86_64-unix/${module}.so"
done
ln -sfn ../../external/D3DMetal.framework "${MWXR_GPTK_RUNTIME}/lib/wine/x86_64-unix/D3DMetal.framework"
# Apple's GPTK read-me asks for this on the extracted copy.
xattr -dr com.apple.quarantine "${MWXR_GPTK_RUNTIME}/lib/external" 2>/dev/null || true
print "GPTK D3DMetal runtime at ${MWXR_GPTK_RUNTIME}"

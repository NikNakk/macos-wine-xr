#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=${0:A:h:h}
: ${MWXR_IN_PROCESS_WINE:?Private paired DXMT runtime}
out=${1:-${repo}/build-in-process/tbdr-audit-validation}
mkdir -p "${out}"
x86_64-w64-mingw32-g++ -static "${repo}/tests/windows/dxmt_tbdr_audit_probe.cpp" \
 -ld3d11 -ld3dcompiler_47 -o "${out}/dxmt_tbdr_audit_probe.exe"
export WINEPREFIX=${repo}/build-in-process/prefix-foveation WINEARCH=win64 WINEDEBUG=-all
export WINESERVER=${MWXR_IN_PROCESS_WINE}/bin/wineserver
export DYLD_FALLBACK_LIBRARY_PATH=${MWXR_IN_PROCESS_WINE}/lib/wine/x86_64-unix:${MWXR_IN_PROCESS_WINE}/lib
export MTL_DEBUG_LAYER=1 DXMT_BASE_FOVEATION=0 DXMT_RATE_MAP_POSITION=0 DXMT_RATE_MAP_ATTACHMENT_PROBE=0
export DXMT_TBDR_AUDIT_DETAIL=${DXMT_TBDR_AUDIT_DETAIL:-0} DXMT_TBDR_AUDIT_START_FRAME=0 DXMT_TBDR_AUDIT_FRAME_COUNT=0
unset WINEDLLOVERRIDES
for enabled in 0 1; do
 DXMT_TBDR_AUDIT=${enabled} "${MWXR_IN_PROCESS_WINE}/bin/wine" "${out}/dxmt_tbdr_audit_probe.exe" > "${out}/audit-${enabled}.log" 2>&1
done
python3 - "${out}" <<'PY'
from pathlib import Path
import re,sys,json
root=Path(sys.argv[1]);off=(root/'audit-0.log').read_text();on=(root/'audit-1.log').read_text()
assert 'TBDR_PROBE pixels=8192 wrong=0' in off and 'TBDR_PROBE pixels=8192 wrong=0' in on
assert 'TBDR cb ' not in off
rows=[dict(re.findall(r'(\w+)=([^\s]+)',line)) for line in on.splitlines() if 'TBDR cb ' in line]
assert rows,'missing audit rows'
sums={key:sum(int(r[key]) for r in rows) for key in ['render_before','render_after','clear_before','clear_after','resolve_before','resolve_after','timestamps','mapped_render']}
assert sums['timestamps']==0 and sums['mapped_render']==0,sums
assert sums['clear_before']>=2 and sums['clear_after']==0,sums
assert sums['resolve_before']==1 and sums['resolve_after']==0,sums
assert sums['render_after']==2,sums
(root/'summary.json').write_text(json.dumps(dict(pixels_per_run=8192,wrong=0,audit_off_no_rows=True,metal_validation=True,counters=sums),indent=2)+'\n')
print('PASS: audit off/on full pixel readback; clear and MSAA resolve fusion preserved; no timestamp queries or mapped passes.')
PY

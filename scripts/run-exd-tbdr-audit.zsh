#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# Simulated-headset TBDR audit of EXD in the experiment prefix (scripts/exd-paths.zsh).
# Preserves and restores the prefix's settings save.
set -euo pipefail
unsetopt BG_NICE
repo=${0:A:h:h}
source "${repo}/scripts/exd-paths.zsh"
mode=${1:-summary}
case ${mode} in summary|detail|capture|trace) ;; *) print -u2 'mode: summary, detail, trace or capture'; exit 2;; esac
seconds=${MWXR_TBDR_SECONDS:-35}
[[ ${seconds} == <-> && ${seconds} -ge 5 && ${seconds} -le 120 ]] || exit 2
save=${exd_prefix}/drive_c/users/crossover/AppData/Local/EXD/Saved/SaveGames/GameSettings.sav
[[ -f ${save} && -f ${save}.before-ngx ]] || { print -u2 "no settings templates in ${save:h}"; exit 2; }
out=${MWXR_FOVEATION_RUN_DIR:-${exd_runs}/tbdr-$(date +%Y%m%d-%H%M%S)-${mode}}
mkdir -p "${out}"
cp "${save}" "${out}/private-save-before-audit.sav"
child=''
timer=''
cleanup(){
 [[ -n ${timer} ]] && kill -TERM ${timer} 2>/dev/null || true
 [[ -n ${child} ]] && kill -TERM ${child} 2>/dev/null || true
 [[ -n ${child} ]] && wait ${child} 2>/dev/null || true
 WINEPREFIX=${exd_prefix} "${exd_wine}/bin/wineserver" -k 2>/dev/null || true
 cp "${out}/private-save-before-audit.sav" "${save}"
}
trap cleanup EXIT
trap 'exit 143' TERM HUP
trap 'exit 130' INT
# Known preserved baseline: DLSS off, preferred upscaler 0, MSAA 2. No binary edits.
cp "${save}.before-ngx" "${save}"
export MWXR_FOVEATION_RUN_DIR=${out}
export DXMT_BASE_FOVEATION=0 DXMT_RATE_MAP_POSITION=0 DXMT_RATE_MAP_ATTACHMENT_PROBE=0
export MWXR_DXMT_FRAME_BOUNDARY=0 DXMT_NGX_SUBRECTS=0
export DXMT_SCENE_TRACE=0 DXMT_NGX_TRACE=0 DXMT_TBDR_AUDIT=1
export DXMT_TBDR_AUDIT_DETAIL=0 DXMT_ENCODER_LABELS=0
export MTL_DEBUG_LAYER=0
unset DXMT_CAPTURE_FRAME DXMT_CAPTURE_FRAME_COUNT DXMT_CAPTURE_EXECUTABLE MTL_CAPTURE_ENABLED
if [[ ${mode} == detail ]]; then
 export DXMT_TBDR_AUDIT_DETAIL=1 DXMT_ENCODER_LABELS=1
fi
if [[ ${mode} == trace ]]; then export DXMT_ENCODER_LABELS=1; fi
if [[ ${mode} == capture ]]; then
 export DXMT_ENCODER_LABELS=1 MTL_CAPTURE_ENABLED=1 DXMT_CAPTURE_EXECUTABLE=EXD-Win64-Shipping
 export DXMT_CAPTURE_FRAME=${MWXR_TBDR_CAPTURE_FRAME:-300} DXMT_CAPTURE_FRAME_COUNT=1
fi
python3 - "${out}" "${save}" "${mode}" "${seconds}" "${exd_build}" "${exd_game}" "${exd_wine}" <<'PY'
import hashlib,json,os,sys
from pathlib import Path
out,save,mode,seconds,build,game,wine=sys.argv[1:]
keys=['DXMT_BASE_FOVEATION','DXMT_RATE_MAP_POSITION','DXMT_RATE_MAP_ATTACHMENT_PROBE','MWXR_DXMT_FRAME_BOUNDARY','DXMT_NGX_SUBRECTS','DXMT_SCENE_TRACE','DXMT_NGX_TRACE','DXMT_TBDR_AUDIT','DXMT_TBDR_AUDIT_DETAIL','DXMT_ENCODER_LABELS','MTL_DEBUG_LAYER','MTL_CAPTURE_ENABLED','DXMT_CAPTURE_FRAME','DXMT_CAPTURE_FRAME_COUNT','DXMT_CAPTURE_EXECUTABLE','DXMT_PIXEL_FORMAT_VIEW','DXMT_TBDR_SHADER_DUMP','DXMT_TBDR_DRAW_STATE','DXMT_TBDR_AUDIT_START_FRAME','DXMT_TBDR_AUDIT_FRAME_COUNT']
metadata=dict(mode=mode,seconds=int(seconds),steam_build=build or None,game=game,wine=wine,baseline='preserved pre-NGX save; upscaler off, MSAA2',save_sha256=hashlib.sha256(Path(save).read_bytes()).hexdigest(),environment={k:os.environ.get(k) for k in keys})
(Path(out)/'audit-config.json').write_text(json.dumps(metadata,indent=2)+'\n')
PY
print "EXD TBDR ${mode}: ${out} (${seconds}s)"
"${repo}/scripts/run-exd-scene-trace.zsh" > "${out}/launcher.log" 2>&1 &
child=$!
(sleep ${seconds}; kill -TERM ${child} 2>/dev/null || true) &
timer=$!
set +e
wait ${child}
run_status=$?
set -e
child=''
print "Launch ended (${run_status}); restoring the settings save."

if [[ ${run_status} != 0 && ${run_status} != 143 && ${run_status} != 130 ]]; then exit ${run_status}; fi

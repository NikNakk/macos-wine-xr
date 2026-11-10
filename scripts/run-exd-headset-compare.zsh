#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# One timed EXD run on the installed PS VR2 service, using the private EXD
# copy (patched DXMT with the NGX per-eye fix) under build-in-process/exd-ngx.
# Usage: run-exd-headset-compare.zsh CONFIG [SECONDS]
#   msaa2       upscaler off, 2x MSAA (the TBDR audit baseline)
#   msaa0       upscaler off, MSAA off (0; a value of 1 gives 4x MSAA)
#   msaa4       upscaler off, 4x MSAA (save value 1)
#   dlss        DLSS Quality via NGX->MetalFX, per-eye fix on, 2x MSAA
#   dlss-msaa0  as dlss, MSAA off
#   dlss-nofix  as dlss, per-eye fix off (the old behaviour)
# Optional: MWXR_VIEWPORT_SCALE_PERCENTAGE, MWXR_EXD_LOW, MWXR_EXD_CVARS, MWXR_EXD_UPSCALER,
# MWXR_EXD_TAG, MWXR_EXD_DETAIL_FRAME (see below). Restores the game's settings afterwards.
set -euo pipefail
unsetopt BG_NICE
repo=${0:A:h:h}
# MWXR_EXD_EXPERIMENT_ROOT: another prepared copy (as scripts/run-exd-scene-trace.zsh).
root=${MWXR_EXD_EXPERIMENT_ROOT:-${repo}/build-in-process/exd-ngx}
config=${1:?config: msaa2, msaa0, msaa4, dlss, dlss-msaa0 or dlss-nofix}
seconds=${2:-90}
saves=${root}/prefix/drive_c/users/crossover/AppData/Local/EXD/Saved/SaveGames
save=${saves}/GameSettings.sav
[[ -f ${save}.before-ngx && -x ${root}/wine/bin/wine ]] || { print -u2 'prepared private EXD copy missing'; exit 2; }
# The save as Codex left it (DLSS Quality, upscaler 1, MSAA 2) is the DLSS template.
[[ -f ${save}.dlss ]] || cp "${save}" "${save}.dlss"
[[ -f ${save}.before-headset ]] || cp "${save}" "${save}.before-headset"

subrects=0
case ${config} in
 msaa2) template=${save}.before-ngx; edits=(MSAASampleCount=2) ;;
 msaa0) template=${save}.before-ngx; edits=(MSAASampleCount=0) ;;
 msaa4) template=${save}.before-ngx; edits=(MSAASampleCount=1) ;;
 dlss) template=${save}.dlss; edits=(MSAASampleCount=2); subrects=1 ;;
 dlss-msaa0) template=${save}.dlss; edits=(MSAASampleCount=0); subrects=1 ;;
 dlss-nofix) template=${save}.dlss; edits=(MSAASampleCount=2) ;;
 *) print -u2 "unknown config ${config}"; exit 2 ;;
esac
# MWXR_EXD_TAG names the run directory, after the config and viewport scale.
out=${root}/headset-$(date +%Y%m%d-%H%M%S)-${config}${MWXR_VIEWPORT_SCALE_PERCENTAGE:+-s${MWXR_VIEWPORT_SCALE_PERCENTAGE}}${MWXR_EXD_TAG:+-${MWXR_EXD_TAG}}${MWXR_EXD_DETAIL_FRAME:+-detail}
mkdir -p "${out}/game" "${out}/timing"
cp "${save}" "${out}/save-before.sav"
# MWXR_EXD_LOW=1: volumetric fog off and every scalability group except textures at Low.
ini=${root}/prefix/drive_c/users/crossover/AppData/Local/EXD/Saved/Config/WindowsNoEditor/GameUserSettings.ini
cp "${ini}" "${out}/GameUserSettings-before.ini"
if [[ -n ${MWXR_EXD_LOW:-} ]]; then
 edits+=(FogQuality=0 r.VolumetricFog=0)
 sed -i '' -E 's/^sg\.(Shadow|ViewDistance|PostProcess|Effects|Foliage|Shading)Quality=.*/sg.\1Quality=0/' "${ini}"
fi
# MWXR_EXD_UPSCALER=N: EXD's UpscalerPreferredSelection (0 off, 1 DLSS as saved).
[[ -n ${MWXR_EXD_UPSCALER:-} ]] && edits+=(UpscalerPreferredSelection=${MWXR_EXD_UPSCALER})
# MWXR_EXD_CVARS="r.A=1;r.B=0": console variables in the user Engine.ini [SystemSettings].
engine=${ini:h}/Engine.ini
cp "${engine}" "${out}/Engine-before.ini"
if [[ -n ${MWXR_EXD_CVARS:-} ]]; then
 # Engine.ini has CRLF line endings; insert after the [SystemSettings] header.
 python3 -I - "${engine}" "${MWXR_EXD_CVARS}" <<'PY2'
import sys
path, cvars = sys.argv[1], [c for c in sys.argv[2].split(';') if c]
text = open(path, newline='').read()
nl = '\r\n' if '\r\n' in text else '\n'
header = '[SystemSettings]' + nl
if header not in text:
    text = text.rstrip('\r\n') + nl + nl + header
i = text.index(header) + len(header)
open(path, 'w', newline='').write(text[:i] + ''.join(c + nl for c in cvars) + text[i:])
PY2
fi
python3 -I "${repo}/tools/exd_settings.py" "${template}" "${save}" "${edits[@]}" > "${out}/settings.txt"
grep '^sg\.' "${ini}" >> "${out}/settings.txt"
sed -n '/^\[SystemSettings\]/,$p' "${engine}" >> "${out}/settings.txt"

# Installed hardware service and its matching x86_64 client (as ~/Windows/run-windows-game.zsh).
unset XR_RUNTIME_JSON XRT_MACOS_METAL_IPC_SERVICE_NAME XDG_RUNTIME_DIR IPC_IGNORE_VERSION
# MWXR_EXD_SERVICE_ENV="K=V ...": instead of the installed LaunchAgent, start a private
# on-demand job running the same service binary with the LaunchAgent's environment plus
# these overrides (for example XRT_COMPOSITOR_COMPUTE=1). The LaunchAgent is not changed.
# MWXR_EXD_SERVICE_BIN replaces the job's service binary; MWXR_EXD_RUNTIME_JSON must then
# name the x86_64 client built from the same source (the IPC version check needs it).
service_label=''
if [[ -n ${MWXR_EXD_SERVICE_ENV:-} ]]; then
 if pgrep -x monado-service >/dev/null; then print -u2 'a Monado service is already running'; exit 2; fi
 service_label=org.freedesktop.monado.exd-private.${UID}.$$
 export XRT_MACOS_METAL_IPC_SERVICE_NAME=${service_label}.metal-ipc
 export XDG_RUNTIME_DIR=/private/tmp/mwxr-exd-service.${UID}.$$
 mkdir -p "${XDG_RUNTIME_DIR}"
 python3 -I - "$HOME/Library/LaunchAgents/org.freedesktop.monado.service.plist" "${out}/service.plist" \
  "${service_label}" "${out}" ${=MWXR_EXD_SERVICE_ENV} <<'PY2'
import os, plistlib, sys
source, target, label, out, *overrides = sys.argv[1:]
plist = plistlib.load(open(source, 'rb'))
env = dict(plist.get('EnvironmentVariables', {}))
env.update(kv.split('=', 1) for kv in overrides)
if os.environ.get('MWXR_EXD_SERVICE_BIN'):
    plist['ProgramArguments'][0] = os.environ['MWXR_EXD_SERVICE_BIN']
env['XRT_MACOS_METAL_IPC_SERVICE_NAME'] = os.environ['XRT_MACOS_METAL_IPC_SERVICE_NAME']
env['XDG_RUNTIME_DIR'] = os.environ['XDG_RUNTIME_DIR']
plist.update(Label=label, EnvironmentVariables=env, RunAtLoad=False,
             MachServices={env['XRT_MACOS_METAL_IPC_SERVICE_NAME']: True},
             StandardOutPath=out + '/service.out.log', StandardErrorPath=out + '/service.err.log')
plistlib.dump(plist, open(target, 'wb'))
PY2
 launchctl bootstrap "gui/${UID}" "${out}/service.plist"
 print "${MWXR_EXD_SERVICE_ENV}" >> "${out}/settings.txt"
fi
[[ -n ${MWXR_EXD_SERVICE_BIN:-} && -z ${MWXR_EXD_RUNTIME_JSON:-} ]] && { print -u2 'MWXR_EXD_SERVICE_BIN needs MWXR_EXD_RUNTIME_JSON'; exit 2; }
export MWXR_NATIVE_RUNTIME_JSON=${MWXR_EXD_RUNTIME_JSON:-$HOME/Code/alyx-native/build/monado-system-x86_64/openxr_monado-dev.json}
# MWXR_EXD_BUILD: bridge build directory under build-in-process (default foveation-gate).
export MWXR_IN_PROCESS_BUILD=${repo}/build-in-process/${MWXR_EXD_BUILD:-foveation-gate}
export MWXR_IN_PROCESS_PREFIX=${root}/prefix MWXR_IN_PROCESS_WINE=${root}/wine
export MWXR_CLIENT_COMPOSITOR=0 MTL_DEBUG_LAYER=0
export GAME_LOG_DIR=${out}/game DXMT_LOG_PATH="Z:${out//\//\\}\\game"
export DXMT_ENABLE_NVEXT=1 DXMT_NGX_CHEEKY=0 DXMT_NGX_SUBRECTS=${subrects}
export DXMT_BASE_FOVEATION=0 DXMT_RATE_MAP_POSITION=0 DXMT_RATE_MAP_ATTACHMENT_PROBE=0
export DXMT_SCENE_TRACE=0 DXMT_NGX_TRACE=0 DXMT_ENCODER_LABELS=${MWXR_EXD_LABELS:-0}
export DXMT_TBDR_AUDIT=1 DXMT_TBDR_AUDIT_DETAIL=0 MWXR_DXMT_FRAME_BOUNDARY=0
# MWXR_EXD_DETAIL_FRAME=N: also log per-pass attachments for frames N and N+1.
if [[ -n ${MWXR_EXD_DETAIL_FRAME:-} ]]; then
 export DXMT_TBDR_AUDIT_DETAIL=1 DXMT_ENCODER_LABELS=1
 export DXMT_TBDR_AUDIT_START_FRAME=${MWXR_EXD_DETAIL_FRAME} DXMT_TBDR_AUDIT_FRAME_COUNT=2
fi
export MWXR_WINEDLLOVERRIDES='nvapi64,nvngx,_nvngx,nvngx_dlss=b;gameoverlayrenderer64='
env | grep -E '^(MWXR|DXMT|XRT|OXR|MTL)_' | sort > "${out}/env.txt"

child=''
cleanup() {
 [[ -n ${child} ]] && kill -TERM ${child} 2>/dev/null || true
 WINEPREFIX=${root}/prefix "${root}/wine/bin/wineserver" -k 2>/dev/null || true
 [[ -n ${child} ]] && wait ${child} 2>/dev/null || true
 cp "${out}/save-before.sav" "${save}"
 cp "${out}/GameUserSettings-before.ini" "${ini}"
 cp "${out}/Engine-before.ini" "${engine}"
 if [[ -n ${service_label} ]]; then
  launchctl bootout "gui/${UID}/${service_label}" 2>/dev/null || true
  rm -rf "${XDG_RUNTIME_DIR}"
 fi
 # Monado's per-client timing CSVs written during this run.
 find "$HOME/Library/Logs/Monado/timing" -type f -newer "${out}/settings.txt" \
  -exec cp {} "${out}/timing/" \; 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 143' TERM HUP
trap 'exit 130' INT
print "EXD headset ${config} (${seconds}s): ${out}"
cat "${out}/settings.txt"
"${repo}/scripts/run-exd-scene-trace.zsh" --launch-game \
 -dx11 -hmd=OpenXRHMD -EXDOpenXROnly -DisablePlugins=OculusOpenXR -EXDDLSS=Classic \
 -ngxenable -windowed -ResX=1280 -ResY=720 > "${out}/launcher.log" 2>&1 &
child=$!
for (( i = 0; i < seconds; ++i )); do
 kill -0 ${child} 2>/dev/null || break
 sleep 1
done
print "Run finished after ${i}s"

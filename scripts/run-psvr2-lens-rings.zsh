#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# Show coloured PS VR2 lens rings in the headset, drawn from this headset's own lens
# calibration, to choose PSVR2_VISIBILITY_MASK_R2: look for the outermost ring you can
# see all the way round in each eye. Each ring is exactly where the mask would cut for
# that value; its label gives the share of the image the mask would hide.
#
# Usage (from the main checkout): scripts/run-psvr2-lens-rings.zsh [SECONDS]
#   Without SECONDS it runs until Ctrl-C.
# It runs a private on-demand copy of the installed Monado service job (same environment
# plus PSVR2_LOG=info) from the claude/quad-gaze-inset build; the LaunchAgent is unchanged.
# MWXR_LENS_SERVICE_EXTRA="K=V ..." adds service settings (for a dry run without the
# headset: SIMULATED_DESKTOP=1 XRT_MACOS_REFRESH_RATE_HZ=0; typical calibration is used).
set -euo pipefail
unsetopt BG_NICE
repo=${0:A:h:h}
seconds=${1:-0}
monado=${MWXR_LENS_MONADO_BUILD:-$HOME/Code/alyx-native/build/monado-quad-gaze-arm64}
service_bin=${monado}/src/xrt/targets/service/monado-service
app_bin=${monado}/src/xrt/targets/psvr2_openxr_test/psvr2-openxr-test
runtime_json=${monado}/openxr_monado-dev.json
python=${MWXR_LENS_PYTHON:-/opt/homebrew/bin/python3.15}
for f in "${service_bin}" "${app_bin}" "${runtime_json}"; do
 [[ -e ${f} ]] || { print -u2 "missing ${f}"; exit 2; }
done
"${python}" -c 'import numpy, PIL' 2>/dev/null || { print -u2 "${python} needs numpy and Pillow"; exit 2; }
if pgrep -x monado-service >/dev/null; then print -u2 'a Monado service is already running'; exit 2; fi

out=${repo}/build-in-process/lens-rings-$(date +%Y%m%d-%H%M%S)
mkdir -p "${out}"
label=org.freedesktop.monado.lens-rings.${UID}.$$
export XRT_MACOS_METAL_IPC_SERVICE_NAME=${label}.metal-ipc
export XDG_RUNTIME_DIR=/private/tmp/mwxr-lens-rings.${UID}.$$
export XR_RUNTIME_JSON=${runtime_json}
mkdir -p "${XDG_RUNTIME_DIR}"
python3 -I - "$HOME/Library/LaunchAgents/org.freedesktop.monado.service.plist" "${out}/service.plist" \
 "${label}" "${out}" "${service_bin}" PSVR2_LOG=info ${=MWXR_LENS_SERVICE_EXTRA:-} <<'PY'
import os, plistlib, sys
source, target, label, out, binary, *overrides = sys.argv[1:]
plist = plistlib.load(open(source, 'rb'))
env = dict(plist.get('EnvironmentVariables', {}))
env.update(kv.split('=', 1) for kv in overrides)
env['XRT_MACOS_METAL_IPC_SERVICE_NAME'] = os.environ['XRT_MACOS_METAL_IPC_SERVICE_NAME']
env['XDG_RUNTIME_DIR'] = os.environ['XDG_RUNTIME_DIR']
plist['ProgramArguments'][0] = binary
plist.update(Label=label, EnvironmentVariables=env, RunAtLoad=False,
             MachServices={env['XRT_MACOS_METAL_IPC_SERVICE_NAME']: True},
             StandardOutPath=out + '/service.out.log', StandardErrorPath=out + '/service.err.log')
plistlib.dump(plist, open(target, 'wb'))
PY

app=''
cleanup() {
 [[ -n ${app} ]] && kill -INT ${app} 2>/dev/null && wait ${app} 2>/dev/null || true
 launchctl bootout "gui/${UID}/${label}" 2>/dev/null || true
 for i in {1..20}; do pgrep -x monado-service >/dev/null || break; sleep 0.5; done
 rm -rf "${XDG_RUNTIME_DIR}"
}
trap cleanup EXIT
trap 'exit 143' TERM HUP
trap 'exit 130' INT
launchctl bootstrap "gui/${UID}" "${out}/service.plist"

# 1. A short run of the ordinary scene starts the service, which logs the lens calibration.
"${app_bin}" > "${out}/app-start.log" 2>&1 &
app=$!
for i in {1..40}; do
 grep -q "Lens calibration version" "${out}/service.err.log" "${out}/service.out.log" 2>/dev/null && break
 kill -0 ${app} 2>/dev/null || break
 sleep 0.5
done
kill -INT ${app} 2>/dev/null || true; wait ${app} 2>/dev/null || true; app=''

# 2. Rings from that calibration (typical values if the headset was not found).
log=$(grep -l "Lens calibration version" "${out}"/service.*.log 2>/dev/null | head -1 || true)
if [[ -n ${log} ]]; then
 "${python}" "${repo}/tools/psvr2_lens_rings.py" "${out}" --calibration-log "${log}" | tee "${out}/rings.txt"
else
 print -u2 'no PS VR2 lens calibration in the service log: drawing typical calibration'
 "${python}" "${repo}/tools/psvr2_lens_rings.py" "${out}" | tee "${out}/rings.txt"
fi

# 3. The rings in the headset.
print "Showing the lens rings${seconds:+ for ${seconds}s}; Ctrl-C to stop. Images: ${out}"
"${app_bin}" --lens-images "${out}/lens-rings-left.png" "${out}/lens-rings-right.png" > "${out}/app.log" 2>&1 &
app=$!
if (( seconds > 0 )); then
 for (( i = 0; i < seconds; ++i )); do kill -0 ${app} 2>/dev/null || break; sleep 1; done
else
 wait ${app} || true
fi

#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# A private launchd endpoint and Unix socket; the hardware job is never touched.
set -euo pipefail
repo=${0:A:h:h}
: ${MONADO_SIM_BUILD:?Configured ARM64 simulated-only Monado build}
: ${MWXR_NATIVE_RUNTIME_JSON:?x86_64 Monado client manifest}
: ${MONADO_VULKAN_ICD:?ARM64 service MoltenVK ICD JSON}
[[ $# -gt 0 ]] || { print -u2 'usage: run-in-process-simulated.zsh application.exe [args...]'; exit 2; }
cache=${MONADO_SIM_BUILD}/CMakeCache.txt
service=${MONADO_SIM_BUILD}/src/xrt/targets/service/monado-service
[[ -x ${service} && -f ${cache} && -f ${MONADO_VULKAN_ICD} ]] || { print -u2 'Missing service build or ICD'; exit 1; }
for driver in PSVR2 PSSENSE; do
 rg -q "^XRT_BUILD_DRIVER_${driver}:BOOL=OFF$" "${cache}" || {
  print -u2 "Refusing a service build with ${driver} enabled or unknown"; exit 1
 }
done
rg -q '^XRT_BUILD_DRIVER_SIMULATED:BOOL=ON$' "${cache}" || { print -u2 'Simulated driver is required'; exit 1; }
label=org.freedesktop.monado.inprocess-test.${UID}.$$
export XRT_MACOS_METAL_IPC_SERVICE_NAME=org.freedesktop.monado.metal-ipc.inprocess-test.${UID}.$$
export XDG_RUNTIME_DIR=/private/tmp/mwxr-inprocess.${UID}.$$
root=${repo}/build-in-process/isolated-${UID}-$$
mkdir -p "${root}/traces" "${XDG_RUNTIME_DIR}"
export MTL_DEBUG_LAYER=${MTL_DEBUG_LAYER:-1}
# Disable the pacing hint for the baseline, including inherited launchd settings.
unset U_PACING_APP_USE_MIN_FRAME_PERIOD
python3 - "${root}" "${label}" "${service}" "${MONADO_VULKAN_ICD}" <<'PY'
import os, plistlib, sys
from pathlib import Path
root,label,service,icd=sys.argv[1:]
env=dict(PATH='/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin',
    SIMULATED_ENABLE='1',XRT_COMPOSITOR_NULL='0',XRT_NO_STDIN='1',
    XDG_RUNTIME_DIR=os.environ['XDG_RUNTIME_DIR'],
    XRT_MACOS_METAL_IPC_SERVICE_NAME=os.environ['XRT_MACOS_METAL_IPC_SERVICE_NAME'],
    VK_ICD_FILENAMES=icd,PSVR2_TIMING_TRACE='1',PSVR2_TIMING_TRACE_DIR=root+'/traces',
    MTL_DEBUG_LAYER=os.environ['MTL_DEBUG_LAYER'],U_PACING_APP_USE_MIN_FRAME_PERIOD='0')
# RunAtLoad=false exercises Wine -> native client -> launchd cold activation.
plist=dict(Label=label,ProgramArguments=[service],RunAtLoad=False,
    MachServices={env['XRT_MACOS_METAL_IPC_SERVICE_NAME']:True},EnvironmentVariables=env,
    StandardOutPath=root+'/service.out.log',StandardErrorPath=root+'/service.err.log')
with (Path(root)/'service.plist').open('wb') as f: plistlib.dump(plist,f)
PY
launchctl bootstrap "gui/${UID}" "${root}/service.plist"
cleanup() {
 launchctl bootout "gui/${UID}/${label}" || true
 # Keep traces, plist and logs. Remove only this runner's stale socket files.
 rm -f "${XDG_RUNTIME_DIR}/monado_comp_ipc" "${XDG_RUNTIME_DIR}/monado.pid"
 rmdir "${XDG_RUNTIME_DIR}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
print "Isolated service logs and pacer traces: ${root}"
"${repo}/scripts/run-in-process-openxr.zsh" "$@"

#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=${0:A:h:h}
: ${MONADO_FOVEATION_SERVICE:?Matching ARM64 Monado service}
: ${MONADO_VULKAN_ICD:?ARM64 service Vulkan driver JSON}
: ${MWXR_NATIVE_RUNTIME_JSON:?Private x86_64 runtime with Metal foveation enabled}
: ${MWXR_IN_PROCESS_WINE:?Private experimental DXMT runtime}
cache=${MONADO_FOVEATION_SERVICE:h:h:h:h:h}/CMakeCache.txt
for driver in PSVR2 PSSENSE RIFT; do
 rg -q "^XRT_BUILD_DRIVER_${driver}:BOOL=OFF$" "${cache}" || {
  print -u2 "This proof requires a service build with ${driver} disabled"; exit 1
 }
done
rg -q '^XRT_BUILD_DRIVER_SIMULATED:BOOL=ON$' "${cache}" || exit 1
export MWXR_IN_PROCESS_BUILD=${MWXR_IN_PROCESS_BUILD:-${repo}/build-in-process/foveation-gate}
export MWXR_IN_PROCESS_PREFIX=${MWXR_IN_PROCESS_PREFIX:-${repo}/build-in-process/prefix-foveation}
root=${MWXR_FOVEATION_RUN_DIR:-${repo}/build-in-process/foveation-xr-$(date +%Y%m%d-%H%M%S)}
mkdir -p "${root}/traces"
label=org.freedesktop.monado.foveation-proof.${UID}.$$
export XRT_MACOS_METAL_IPC_SERVICE_NAME=${label}.metal
export XDG_RUNTIME_DIR=/private/tmp/mwxr-foveation.${UID}.$$
mkdir -p "${XDG_RUNTIME_DIR}"
export XRT_MACOS_CLIENT_COMPOSITOR=0 MWXR_CLIENT_COMPOSITOR=0 MTL_DEBUG_LAYER=${MTL_DEBUG_LAYER:-1}
python3 - "${root}" "${label}" "${MONADO_FOVEATION_SERVICE}" "${MONADO_VULKAN_ICD}" <<'PY'
import os, plistlib, sys
from pathlib import Path
root,label,service,icd=sys.argv[1:]
env=dict(PATH='/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin',SIMULATED_ENABLE='1',
    XRT_COMPOSITOR_FORCE_DEBUG_IMAGE='1',XRT_NO_STDIN='1',XRT_COMPOSITOR_NULL='0',
    XRT_MACOS_CLIENT_COMPOSITOR='0',XRT_COMPOSITOR_COMPUTE='0',MTL_DEBUG_LAYER='1',
    XRT_MACOS_METAL_IPC_SERVICE_NAME=os.environ['XRT_MACOS_METAL_IPC_SERVICE_NAME'],
    XDG_RUNTIME_DIR=os.environ['XDG_RUNTIME_DIR'],VK_DRIVER_FILES=icd,
    PSVR2_TIMING_TRACE='1',PSVR2_TIMING_TRACE_DIR=root+'/traces')
plist=dict(Label=label,ProgramArguments=[service],RunAtLoad=False,
    MachServices={env['XRT_MACOS_METAL_IPC_SERVICE_NAME']:True},EnvironmentVariables=env,
    StandardOutPath=root+'/service.out.log',StandardErrorPath=root+'/service.err.log')
with (Path(root)/'service.plist').open('wb') as f:plistlib.dump(plist,f)
PY
launchctl bootstrap "gui/${UID}" "${root}/service.plist"
cleanup() {
 launchctl bootout "gui/${UID}/${label}" || true
 rm -f "${XDG_RUNTIME_DIR}/monado_comp_ipc" "${XDG_RUNTIME_DIR}/monado.pid"
 rmdir "${XDG_RUNTIME_DIR}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
print "Private simulated service and traces: ${root}"
if [[ -n ${MWXR_FOVEATION_LAUNCH_SCRIPT:-} ]]; then
 "${MWXR_FOVEATION_LAUNCH_SCRIPT}" "$@"
 exit $?
fi
cp "${MWXR_IN_PROCESS_BUILD}/x86_64-windows/wine_foveation_probe.dll" \
 "${MWXR_IN_PROCESS_PREFIX}/drive_c/windows/system32/"
"${repo}/scripts/run-in-process-openxr.zsh" \
 "${MWXR_FOVEATION_PROBE_EXE:-${MWXR_IN_PROCESS_BUILD}/openxr_foveation_probe.exe}" "$@"

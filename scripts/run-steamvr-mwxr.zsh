#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Run Windows SteamVR with the mwxr OpenXR driver through the in-process
# wineopenxr runtime. See docs/steamvr-home.md.
#
#   MWXR_MONADO=simulated  an isolated simulated-only Monado service
#                          (SIMULATED_LEFT/RIGHT=wmr adds simulated controllers)
#   MWXR_MONADO=hardware   the installed Monado LaunchAgent (PS VR2)
#   APPID=<steam app id>   optionally launch a Steam app once SteamVR is up
#
# Required:
#   MWXR_STEAMVR_ROOT        built by build-current-dxmt.zsh, with a prefix
#                            holding Steam and SteamVR (prefix/, bin/, wine-11.10/)
#   MWXR_NATIVE_RUNTIME_JSON x86_64 Monado client manifest matching the service
#   MONADO_SIM_BUILD         (simulated only) ARM64 simulated-only service build
set -euo pipefail
repo=${0:A:h:h}
: ${MWXR_STEAMVR_ROOT:?Wine 11.10/current-DXMT root with Steam and SteamVR in its prefix}
: ${MWXR_NATIVE_RUNTIME_JSON:?x86_64 Monado client manifest}
root=${MWXR_STEAMVR_ROOT}
driver_root=${MWXR_STEAMVR_DRIVER:-${repo}/build-in-process/steamvr-driver/mwxr}
prefix=${root}/prefix
steam_dir="${prefix}/drive_c/Program Files (x86)/Steam"
mode=${MWXR_MONADO:-simulated}

[[ -f ${driver_root}/bin/win64/driver_mwxr.dll ]] || { print -u2 "Build the driver first: ${driver_root}"; exit 1; }
[[ -d ${steam_dir}/steamapps/common/SteamVR ]] || { print -u2 "No SteamVR in ${prefix}"; exit 1; }

# Register the driver folder, select it, and never leave it blocked by
# SteamVR safe mode after a failed development start.
python3 - "${prefix}" "Z:${driver_root//\//\\}" "${steam_dir}/config/steamvr.vrsettings" <<'PY'
import json, sys
from pathlib import Path
prefix, driver, settings = sys.argv[1:]
for profile in Path(prefix, 'drive_c/users').iterdir():
    if profile.name == 'Public' or not profile.is_dir():
        continue
    path = profile / 'AppData/Local/openvr/openvrpaths.vrpath'
    path.parent.mkdir(parents=True, exist_ok=True)
    data = json.loads(path.read_text()) if path.exists() else {"jsonid": "vrpathreg", "version": 1}
    data.setdefault("runtime", ["C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR"])
    data.setdefault("config", ["C:\\Program Files (x86)\\Steam\\config"])
    data.setdefault("log", ["C:\\Program Files (x86)\\Steam\\logs"])
    data["external_drivers"] = [driver]
    path.write_text(json.dumps(data, indent=1))
path = Path(settings)
s = json.loads(path.read_text()) if path.exists() else {}
s.setdefault("steamvr", {}).update({"forcedDriver": "mwxr", "activateMultipleDrivers": True, "enableSafeMode": False})
s.setdefault("driver_null", {})["enable"] = False
s.setdefault("driver_mwxr", {}).update({"enable": True, "blocked_by_safe_mode": False})
# The desktop view needs Windows.Graphics.Capture (useNewDesktop) or real
# desktop duplication; neither works under Wine.
s.setdefault("dashboard", {}).update({"showDesktop": False, "enableWindowView": False, "useNewDesktop": False})
s.setdefault("power", {}).update({"pauseCompositorOnStandby": False, "turnOffScreensTimeout": 86400.0})
path.write_text(json.dumps(s, indent=3))
PY

export MWXR_IN_PROCESS_BUILD=${MWXR_IN_PROCESS_BUILD:-${repo}/build-in-process/gate-steamvr}
export MWXR_IN_PROCESS_WINE=${root}/wine-11.10
export MWXR_IN_PROCESS_PREFIX=${prefix}
export MWXR_NATIVE_RUNTIME_JSON
export DXMT_LOG_LEVEL=${DXMT_LOG_LEVEL:-info}
export DXMT_LOG_PATH=${DXMT_LOG_PATH:-Z:${root}/dxmt-logs}
mkdir -p "${root}/dxmt-logs"
logs=${root}/run-mwxr-$(date +%Y%m%d-%H%M%S); mkdir -p "${logs}"; print "Logs: ${logs}"
vrstartup=${steam_dir}/steamapps/common/SteamVR/bin/win64/vrstartup.exe

# Steam first (it needs no XR environment), so SteamVR and Home can reach it.
WINEPREFIX=${prefix} WINEDEBUG=-all "${root}/bin/wine-current-dxmt" 'C:\Program Files (x86)\Steam\steam.exe' -silent \
 -cef-disable-gpu -cef-disable-gpu-compositing -cef-in-process-gpu -cef-disable-sandbox -no-cef-sandbox \
 -noverifyfiles -norepairfiles > "${logs}/steam.log" 2>&1 &
print "Started Steam; waiting ${STEAM_WAIT_S:-40} s"; sleep ${STEAM_WAIT_S:-40}

if [[ -n ${APPID:-} ]]; then
 ( sleep ${VR_WAIT_S:-60}
   WINEPREFIX=${prefix} WINEDEBUG=-all "${root}/bin/wine-current-dxmt" 'C:\Program Files (x86)\Steam\steam.exe' \
    -applaunch "${APPID}" > "${logs}/applaunch.log" 2>&1
   print "Requested launch of app ${APPID}" ) &
fi

# vrstartup.exe returns at once (exit status 3 is normal); vrserver and its
# children inherit this environment.
case ${mode} in
 simulated)
  : ${MONADO_SIM_BUILD:?Configured ARM64 simulated-only Monado build}
  # Isolated service as in run-in-process-simulated.zsh, kept up until vrserver exits.
  service=${MONADO_SIM_BUILD}/src/xrt/targets/service/monado-service
  label=org.freedesktop.monado.mwxr-test.${UID}.$$
  export XRT_MACOS_METAL_IPC_SERVICE_NAME=org.freedesktop.monado.metal-ipc.mwxr-test.${UID}.$$
  export XDG_RUNTIME_DIR=/private/tmp/mwxr-steamvr.${UID}.$$
  mkdir -p "${XDG_RUNTIME_DIR}"
  python3 - "${logs}" "${label}" "${service}" "${MONADO_VULKAN_ICD:-/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json}" <<'PY'
import os, plistlib, sys
root, label, service, icd = sys.argv[1:]
env = dict(PATH='/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin', SIMULATED_ENABLE='1', XRT_COMPOSITOR_NULL='0',
    XRT_NO_STDIN='1', XDG_RUNTIME_DIR=os.environ['XDG_RUNTIME_DIR'],
    XRT_MACOS_METAL_IPC_SERVICE_NAME=os.environ['XRT_MACOS_METAL_IPC_SERVICE_NAME'], VK_ICD_FILENAMES=icd)
# Simulated controllers: SIMULATED_LEFT/RIGHT=simple|wmr|ml2.
env.update({k: os.environ[k] for k in ('SIMULATED_LEFT', 'SIMULATED_RIGHT') if os.environ.get(k)})
# Log levels, for example XRT_COMPOSITOR_LOG=info.
env.update({k: v for k, v in os.environ.items() if k.startswith('XRT_') and k.endswith('_LOG')})
plist = dict(Label=label, ProgramArguments=[service], RunAtLoad=False,
    MachServices={env['XRT_MACOS_METAL_IPC_SERVICE_NAME']: True}, EnvironmentVariables=env,
    StandardOutPath=root + '/service.out.log', StandardErrorPath=root + '/service.err.log')
with open(root + '/service.plist', 'wb') as f: plistlib.dump(plist, f)
PY
  launchctl bootstrap "gui/${UID}" "${logs}/service.plist"
  trap 'launchctl bootout "gui/${UID}/${label}" || true; rm -rf "${XDG_RUNTIME_DIR}"' EXIT INT TERM
  "${repo}/scripts/run-in-process-openxr.zsh" "${vrstartup}" > "${logs}/vrstartup.log" 2>&1 || true ;;
 hardware)
  unset XR_RUNTIME_JSON XRT_MACOS_METAL_IPC_SERVICE_NAME XDG_RUNTIME_DIR IPC_IGNORE_VERSION
  export XRT_MACOS_CLIENT_COMPOSITOR=${XRT_MACOS_CLIENT_COMPOSITOR:-1}
  "${repo}/scripts/run-in-process-openxr.zsh" "${vrstartup}" > "${logs}/vrstartup.log" 2>&1 || true ;;
 *) print -u2 "MWXR_MONADO must be simulated or hardware"; exit 2 ;;
esac
print "SteamVR started; waiting for vrserver to exit (close the SteamVR status window to stop it)"
sleep 20
while pgrep -f 'vrserver.exe' >/dev/null; do sleep 3; done
print "vrserver exited"

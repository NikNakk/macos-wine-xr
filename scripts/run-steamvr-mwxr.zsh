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
#   MWXR_MONADO=isolated   another PS VR2 service build (MONADO_SERVICE_BUILD),
#                          under its own launchd label and socket with the
#                          installed LaunchAgent's environment; the agent itself
#                          is left alone but must not be running (USB)
#   APPID=<steam app id>   optionally launch a Steam app once SteamVR is up
#   APP_ARGS='...'         extra arguments for that app, for example -nowindow
#                          (no desktop companion window in Source 2 games)
#   XRT_MACOS_REFRESH_RATE_HZ=90  PS VR2 refresh rate, passed to the service too
#   MWXR_DISPLAY_MODE=direct|virtual  sets driver_mwxr.displayMode first
#   MWXR_OPENVR_SHIM_INSTALL=0  do not install tools/openvr_shim beside SteamVR
#                           Home (default 1; it logs Home's frame loop to the run's
#                           openvr-shim.log; Valve's openvr_api.dll is restored on exit)
#   MWXR_OPENVR_SHIM_APP_DIRS='Half-Life Alyx/game/bin/win64'  also install it in these
#                           folders (colon-separated, relative to steamapps/common)
#   MWXR_CAFFEINATE=0       let macOS sleep the displays during the run (by default
#                           they stay awake: display sleep turns the headset's display
#                           off, and a service with XRT_MACOS_EXIT_ON_DISPLAY_LOSS=1 exits)
#
# Required:
#   MWXR_STEAMVR_ROOT        built by build-current-dxmt.zsh, with a prefix
#                            holding Steam and SteamVR (prefix/, bin/, wine-11.10/)
#   MWXR_WINE_TREE           Wine install tree (default ${MWXR_STEAMVR_ROOT}/wine-11.10)
#   MWXR_WINE_WRAPPER        Wine launcher (default ${MWXR_STEAMVR_ROOT}/bin/wine-current-dxmt),
#                            for example a CrossOver rig's wine-crossover-dxmt
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
python3 - "${prefix}" "Z:${driver_root//\//\\}" "${steam_dir}/config/steamvr.vrsettings" "${MWXR_DISPLAY_MODE:-}" <<'PY'
import json, sys
from pathlib import Path
prefix, driver, settings, display_mode = sys.argv[1:]
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
if display_mode:
    s["driver_mwxr"]["displayMode"] = display_mode
# The desktop view needs Windows.Graphics.Capture (useNewDesktop) or real
# desktop duplication; neither works under Wine.
s.setdefault("dashboard", {}).update({"showDesktop": False, "enableWindowView": False, "useNewDesktop": False})
s.setdefault("power", {}).update({"pauseCompositorOnStandby": False, "turnOffScreensTimeout": 86400.0})
path.write_text(json.dumps(s, indent=3))
PY

export MWXR_IN_PROCESS_BUILD=${MWXR_IN_PROCESS_BUILD:-${repo}/build-in-process/gate-steamvr}
export MWXR_IN_PROCESS_WINE=${MWXR_WINE_TREE:-${root}/wine-11.10}
wine_wrapper=${MWXR_WINE_WRAPPER:-${root}/bin/wine-current-dxmt}
export MWXR_IN_PROCESS_PREFIX=${prefix}
export MWXR_NATIVE_RUNTIME_JSON
export DXMT_LOG_LEVEL=${DXMT_LOG_LEVEL:-info}
export DXMT_LOG_PATH=${DXMT_LOG_PATH:-Z:${root}/dxmt-logs}
mkdir -p "${root}/dxmt-logs"
logs=${root}/run-mwxr-$(date +%Y%m%d-%H%M%S); mkdir -p "${logs}"; print "Logs: ${logs}"
# Keep the displays (the headset's included) awake until this script exits.
[[ ${MWXR_CAFFEINATE:-1} == 1 ]] && caffeinate -d -w $$ &!
vrstartup=${steam_dir}/steamapps/common/SteamVR/bin/win64/vrstartup.exe

# Whether Steam runs in this prefix: Wine names each prefix's server directory
# after the prefix's device and inode, and its processes keep it open.
steam_running() {
  local server_dir=$(printf 'server-%x-%x' $(stat -f '%d %i' "${prefix}")) pid
  for pid in $(pgrep -f 'Steam.*\\steam\.exe'); do
    lsof -p ${pid} 2>/dev/null | /usr/bin/grep -q "/${server_dir}/" && return 0
  done
  return 1
}

# Steam first (it needs no XR environment), so SteamVR and Home can reach it.
# Not -silent: in the CrossOver prefix, Steam started that way stalls at start-up.
if steam_running; then
  print "Steam is already running in this prefix"
else
  WINEPREFIX=${prefix} WINEDEBUG=-all "${wine_wrapper}" 'C:\Program Files (x86)\Steam\steam.exe' \
   -cef-disable-gpu -cef-disable-gpu-compositing -cef-in-process-gpu -cef-disable-sandbox -no-cef-sandbox \
   -noverifyfiles -norepairfiles > "${logs}/steam.log" 2>&1 &
  print "Started Steam; waiting ${STEAM_WAIT_S:-40} s"; sleep ${STEAM_WAIT_S:-40}
fi

if [[ -n ${APPID:-} ]]; then
 ( sleep ${VR_WAIT_S:-60}
   WINEPREFIX=${prefix} WINEDEBUG=-all "${wine_wrapper}" 'C:\Program Files (x86)\Steam\steam.exe' \
    -applaunch "${APPID}" ${=APP_ARGS:-} > "${logs}/applaunch.log" 2>&1
   print "Requested launch of app ${APPID}" ) &
fi

# vrstartup.exe returns at once (exit status 3 is normal); vrserver and its
# children inherit this environment.
# An isolated service: own launchd label, Metal XPC name and socket directory,
# booted out again when this script exits. (The trap is set here: in zsh, an
# EXIT trap set inside a function fires when the function returns.)
isolated_label= isolated_dir=
# The OpenVR shim beside SteamVR Home, which logs Home's WaitGetPoses and
# Submit calls (see tools/openvr_shim). Valve's DLL is kept as
# openvr_api_valve.dll and put back on exit.
shim=${MWXR_OPENVR_SHIM:-${repo}/build-in-process/openvr-shim/openvr_api.dll}
shim_dirs=("${steam_dir}/steamapps/common/SteamVR/tools/steamvr_environments/game/bin/win64")
for dir in ${(s.:.)MWXR_OPENVR_SHIM_APP_DIRS:-}; do shim_dirs+=("${steam_dir}/steamapps/common/${dir}"); done
shim_installed=()
install_shim() {
  [[ ${MWXR_OPENVR_SHIM_INSTALL:-1} == 1 && -f ${shim} ]] || return 0
  local dir
  for dir in "${shim_dirs[@]}"; do
    [[ -f ${dir}/openvr_api.dll ]] || { print -u2 "No openvr_api.dll in ${dir}; shim not installed there"; continue; }
    # A file without the marker is Valve's (perhaps updated by Steam): keep it.
    if ! /usr/bin/grep -q mwxr-openvr-shim "${dir}/openvr_api.dll"; then
      mv -f "${dir}/openvr_api.dll" "${dir}/openvr_api_valve.dll"
    fi
    [[ -f ${dir}/openvr_api_valve.dll ]] || continue
    cp "${shim}" "${dir}/openvr_api.dll"
    shim_installed+=("${dir}")
    print "Installed the OpenVR shim in ${dir#${steam_dir}/steamapps/common/}"
  done
}
restore_shim() {
  local dir
  for dir in "${shim_installed[@]}"; do
    [[ -f ${dir}/openvr_api_valve.dll ]] && mv -f "${dir}/openvr_api_valve.dll" "${dir}/openvr_api.dll"
    # Apps started by a Steam launched elsewhere log beside the shim.
    [[ -f ${dir}/openvr-shim.log ]] && cat "${dir}/openvr-shim.log" >> "${logs}/openvr-shim.log" && rm -f "${dir}/openvr-shim.log"
  done
  shim_installed=()
}
cleanup() {
  restore_shim
  [[ -n ${isolated_label} ]] && { launchctl bootout "gui/${UID}/${isolated_label}" || true; rm -rf "${isolated_dir}"; }
  return 0
}
trap cleanup EXIT INT TERM
start_isolated_service() { # <service binary> <environment template plist or empty> <simulated 0|1>
  label=org.freedesktop.monado.mwxr-test.${UID}.$$
  export XRT_MACOS_METAL_IPC_SERVICE_NAME=org.freedesktop.monado.metal-ipc.mwxr-test.${UID}.$$
  export XDG_RUNTIME_DIR=/private/tmp/mwxr-steamvr.${UID}.$$
  mkdir -p "${XDG_RUNTIME_DIR}"
  python3 - "${logs}" "${label}" "$1" "${MONADO_VULKAN_ICD:-/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json}" "$2" "$3" <<'PY'
import os, plistlib, sys
root, label, service, icd, template, simulated = sys.argv[1:]
env = {}
if template:
    with open(template, 'rb') as f:
        env = dict(plistlib.load(f).get('EnvironmentVariables', {}))
    # Belongs to the installed service only.
    env.pop('IPC_WINE_TCP_PORT', None)
env.update(PATH=env.get('PATH', '/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin'), XRT_NO_STDIN='1',
    XDG_RUNTIME_DIR=os.environ['XDG_RUNTIME_DIR'], VK_ICD_FILENAMES=icd,
    XRT_MACOS_METAL_IPC_SERVICE_NAME=os.environ['XRT_MACOS_METAL_IPC_SERVICE_NAME'])
if simulated == '1':
    env.update(SIMULATED_ENABLE='1', XRT_COMPOSITOR_NULL='0')
    # Simulated controllers: SIMULATED_LEFT/RIGHT=simple|wmr|ml2.
    env.update({k: os.environ[k] for k in ('SIMULATED_LEFT', 'SIMULATED_RIGHT') if os.environ.get(k)})
# Log levels (for example XRT_COMPOSITOR_LOG=info) and the refresh rate.
env.update({k: v for k, v in os.environ.items()
            if k.startswith('XRT_') and (k.endswith('_LOG') or k == 'XRT_MACOS_REFRESH_RATE_HZ')})
plist = dict(Label=label, ProgramArguments=[service], RunAtLoad=False, ProcessType='Interactive',
    MachServices={env['XRT_MACOS_METAL_IPC_SERVICE_NAME']: True}, EnvironmentVariables=env,
    StandardOutPath=root + '/service.out.log', StandardErrorPath=root + '/service.err.log')
with open(root + '/service.plist', 'wb') as f: plistlib.dump(plist, f)
PY
  launchctl bootstrap "gui/${UID}" "${logs}/service.plist"
  isolated_label=${label} isolated_dir=${XDG_RUNTIME_DIR}
}

install_shim
export MWXR_OPENVR_SHIM_LOG="Z:${logs//\//\\}\\openvr-shim.log"

case ${mode} in
 simulated)
  : ${MONADO_SIM_BUILD:?Configured ARM64 simulated-only Monado build}
  start_isolated_service "${MONADO_SIM_BUILD}/src/xrt/targets/service/monado-service" "" 1
  "${repo}/scripts/run-in-process-openxr.zsh" "${vrstartup}" > "${logs}/vrstartup.log" 2>&1 || true ;;
 isolated)
  : ${MONADO_SERVICE_BUILD:?ARM64 Monado build with the PS VR2 driver}
  if pgrep -x monado-service >/dev/null; then
    print -u2 "A monado-service is running and may hold the PS VR2; stop it first"; exit 1
  fi
  unset XR_RUNTIME_JSON IPC_IGNORE_VERSION
  start_isolated_service "${MONADO_SERVICE_BUILD}/src/xrt/targets/service/monado-service" \
    "${MWXR_SERVICE_TEMPLATE:-${HOME}/Library/LaunchAgents/org.freedesktop.monado.service.plist}" 0
  export XRT_MACOS_CLIENT_COMPOSITOR=${XRT_MACOS_CLIENT_COMPOSITOR:-1}
  "${repo}/scripts/run-in-process-openxr.zsh" "${vrstartup}" > "${logs}/vrstartup.log" 2>&1 || true ;;
 hardware)
  unset XR_RUNTIME_JSON XRT_MACOS_METAL_IPC_SERVICE_NAME XDG_RUNTIME_DIR IPC_IGNORE_VERSION
  export XRT_MACOS_CLIENT_COMPOSITOR=${XRT_MACOS_CLIENT_COMPOSITOR:-1}
  "${repo}/scripts/run-in-process-openxr.zsh" "${vrstartup}" > "${logs}/vrstartup.log" 2>&1 || true ;;
 *) print -u2 "MWXR_MONADO must be simulated, isolated or hardware"; exit 2 ;;
esac
print "SteamVR started; waiting for vrserver to exit (close the SteamVR status window to stop it)"
sleep 20
while pgrep -f 'vrserver.exe' >/dev/null; do sleep 3; done
print "vrserver exited"

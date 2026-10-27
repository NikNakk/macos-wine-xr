#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# Half-Life: Alyx through the in-process runtime: OpenVR -> xrizer -> the
# game's Windows OpenXR loader -> wineopenxr -> native OpenXR runtime.
# Steam is started in the same prefix first, as the game needs its client.
set -euo pipefail
repo=${0:A:h:h}
: ${ALYX_GAME_ROOT:?Alyx install containing game/bin/win64/hlvr.exe}
: ${MWXR_XRIZER_ROOT:?xrizer provision containing bin/vrclient_x64.dll and openxr_loader.dll}
: ${MWXR_IN_PROCESS_WINE:?Wine runtime (GPTK/D3DMetal or direct-object DXMT)}
: ${MWXR_IN_PROCESS_PREFIX:?Prefix with Steam installed and logged in}
exe=${ALYX_GAME_ROOT}/game/bin/win64/hlvr.exe
bin=${exe:h}
[[ -f ${exe} ]] || { print -u2 "Missing ${exe}"; exit 1; }
[[ -f ${MWXR_XRIZER_ROOT}/bin/vrclient_x64.dll ]] || { print -u2 'Missing xrizer bin/vrclient_x64.dll'; exit 1; }
# xrizer is selected with VR_OVERRIDE through Valve's own openvr_api.dll, and
# the game loads the provisioned Windows OpenXR loader beside it.
cmp -s "${bin}/openvr_api.dll" "${bin}/openvr_api.dll.monado-original" || {
 print -u2 "Alyx's openvr_api.dll is not Valve's original; restore it before using xrizer"; exit 1
}
cmp -s "${bin}/openxr_loader.dll" "${MWXR_XRIZER_ROOT}/openxr_loader.dll" || {
 print -u2 "Alyx does not have the provisioned Windows OpenXR loader beside hlvr.exe"; exit 1
}
steam='C:\Program Files (x86)\Steam\steam.exe'
[[ -f ${MWXR_IN_PROCESS_PREFIX}/drive_c/Program\ Files\ \(x86\)/Steam/steam.exe ]] || {
 print -u2 "No Steam in ${MWXR_IN_PROCESS_PREFIX}"; exit 1
}

logs=${ALYX_LOG_DIR:-${repo}/build-in-process/alyx-$(date +%Y%m%d-%H%M%S)}
mkdir -p "${logs}"
print "Alyx logs: ${logs}"
wine=${MWXR_IN_PROCESS_WINE}/bin/wine
export WINEPREFIX=${MWXR_IN_PROCESS_PREFIX}
export WINEARCH=win64
export WINEDEBUG=${WINEDEBUG:--all}
export DYLD_FALLBACK_LIBRARY_PATH=${MWXR_IN_PROCESS_WINE}/lib/wine/x86_64-unix:${MWXR_IN_PROCESS_WINE}/lib

if [[ ${ALYX_START_STEAM:-1} == 1 ]]; then
 # Wine updates a prefix itself when its runtime's wine.inf changes. An explicit
 # "wineboot -u" on every launch left a temporary copy of each DLL a running process
 # had open (dll*.tmp, pending until the prefix restarts): 12.9 GB in the rig prefix.
 [[ -f ${WINEPREFIX}/system.reg ]] || WINEDLLOVERRIDES="mscoree=;mshtml=" "${wine}" wineboot --init
 # Software-rendered CEF so Steam's UI paints under Wine (Silo/Vineport flags).
 "${wine}" "${steam}" -silent -cef-disable-gpu -cef-disable-gpu-compositing -cef-in-process-gpu \
  -cef-disable-sandbox -no-cef-sandbox -noverifyfiles -norepairfiles > "${logs}/steam.log" 2>&1 &
 print "Started Steam; waiting ${ALYX_STEAM_WAIT_S:-30} s for it to sign in."
 sleep ${ALYX_STEAM_WAIT_S:-30}
fi

# Valve's openvr_api.dll needs a path registry even with VR_OVERRIDE. Wine
# builds name the Windows user differently (CrossOver: "crossover"), so write
# it for every user profile in the prefix.
xrizer_windows="Z:${MWXR_XRIZER_ROOT//\//\\}"
for profile in "${MWXR_IN_PROCESS_PREFIX}"/drive_c/users/*(/N); do
 [[ ${profile:t} == Public ]] && continue
 mkdir -p "${profile}/AppData/Local/openvr"
 cat > "${profile}/AppData/Local/openvr/openvrpaths.vrpath" <<JSON
{"jsonid": "vrpathreg", "runtime": ["${xrizer_windows//\\/\\\\}"], "version": 1}
JSON
done

trace_windows="Z:${logs//\//\\}"
export VR_OVERRIDE="Z:${MWXR_XRIZER_ROOT//\//\\}"
export XDG_STATE_HOME=${trace_windows}
export RUST_LOG=${RUST_LOG:-xrizer=info,openvr=warn,tracked_property=warn,unknown_interfaces=info}
# ALYX_BASE_ARGS replaces the default map and fidelity arguments (split on
# spaces), for example to load a save at a fixed fidelity level.
if [[ -n ${ALYX_BASE_ARGS:-} ]]; then
 base_args=(${=ALYX_BASE_ARGS})
else
 base_args=(+map startup -novid -nowindow -console -vconsole +vr_fidelity_level_auto 0 +vr_fidelity_level 3)
fi
# ALYX_STEAM_STAND_IN: a Steam stand-in that starts the game (mwxr-steam.exe, for the
# macOS Steam client through lsteamclient; see docs/lsteamclient.md).
launch=("${exe}")
if [[ -n ${ALYX_STEAM_STAND_IN:-} ]]; then
 # the stand-in starts the game with CreateProcess, which needs its Windows path
 exe_windows=$("${wine}" winepath -w "${exe}" 2>/dev/null | tr -d '\r')
 launch=("${ALYX_STEAM_STAND_IN}" -- "${exe_windows}")
fi
"${repo}/scripts/run-in-process-openxr.zsh" "${launch[@]}" -vr -steam -noasserts -nopassiveasserts \
 "${base_args[@]}" "$@" 2>&1 | tee "${logs}/alyx.log"
exit ${pipestatus[1]}

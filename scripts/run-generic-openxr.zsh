#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
# Explicitly use a private prefix: the Win64 loader may ignore XR_RUNTIME_JSON
# under Wine and therefore needs the ActiveRuntime registry entry there.
set -euo pipefail
repo=${0:A:h:h}
: ${XR_RUNTIME_JSON:?Set XR_RUNTIME_JSON to the native macOS runtime manifest}
: ${MWXR_OPENXR_LOADER:?Set MWXR_OPENXR_LOADER to the native Khronos loader dylib}
: ${MWXR_WINE:?Set MWXR_WINE to Wine with the matching current DXMT installed}
: ${MWXR_PRIVATE_WINEPREFIX:?Set MWXR_PRIVATE_WINEPREFIX to a dedicated test prefix}
if (( $# == 0 )); then
 print -u2 'usage: run-generic-openxr.zsh application.exe [arguments]'; exit 2
fi
native_build=${MWXR_NATIVE_BUILD:-${repo}/build-native-openxr}
win_build=${MWXR_WIN_BUILD:-${repo}/build-win-openxr}
export MWXR_RPC_PORT=${MWXR_RPC_PORT:-4243}
export MWXR_RPC_TOKEN=${MWXR_RPC_TOKEN:-$(python3 -c 'import secrets; print(secrets.token_hex(32))')}
export WINEPREFIX=${MWXR_PRIVATE_WINEPREFIX}
export WINEARCH=win64
manifest=${win_build}/macos_wine_xr_openxr.json
[[ -f ${manifest} && -x ${native_build}/macos_wine_xr_host ]] || { print -u2 'Build with build-generic-openxr.zsh first';exit 1; }
log=${MWXR_HOST_LOG:-${native_build}/host.log}
"${native_build}/macos_wine_xr_host" "${MWXR_RPC_PORT}" >"${log}" 2>&1 &
host_pid=$!
cleanup() { kill "${host_pid}" 2>/dev/null || true; wait "${host_pid}" 2>/dev/null || true; }
trap cleanup EXIT INT TERM
for _ in {1..100}; do
 kill -0 "${host_pid}" 2>/dev/null || { cat "${log}" >&2;exit 1; }
 if rg -q 'authenticated loopback' "${log}"; then break;fi
 sleep 0.05
done
rg -q 'authenticated loopback' "${log}" || { print -u2 'Native host did not become ready';exit 1; }
windows_manifest="Z:${manifest//\//\\}"
"${MWXR_WINE}" reg.exe add 'HKLM\SOFTWARE\Khronos\OpenXR\1' \
 /v ActiveRuntime /t REG_SZ /d "${windows_manifest}" /f
# Only the native host loads the native manifest. The Windows process gets the
# thin runtime manifest. Switching native runtimes does not rebuild the DLL.
XR_RUNTIME_JSON="${windows_manifest}" "${MWXR_WINE}" "$@"

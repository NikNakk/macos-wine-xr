#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
#
# End-to-end proof:
#   hello_xr/D3D11 -> current DXMT -> macos-wine-xr proxy ->
#   macos-upstream-clean Monado service -> PS VR2
#
# Required:
#   MONADO_SOURCE_DIR=/path/to/NikNakk/monado (macos-upstream-clean)
#   MONADO_BUILD_DIR=/path/to/that configured native build
#
# Optional:
#   MWXR_BOOTSTRAP_SERVICE=1  Replace the currently loaded development Monado
#                             LaunchAgent with MONADO_BUILD_DIR for this test.
#   IPC_WINE_TCP_PORT=4242

set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h}

: ${MONADO_SOURCE_DIR:?Set MONADO_SOURCE_DIR to NikNakk/monado:macos-upstream-clean}
: ${MONADO_BUILD_DIR:?Set MONADO_BUILD_DIR to the matching configured native build}

port=${IPC_WINE_TCP_PORT:-4242}
client_root=${MACOS_WINE_XR_CLIENT_ROOT:-${repo_root}/build-wine-client}
dxmt_root=${MACOS_WINE_XR_CURRENT_DXMT_ROOT:-${repo_root}/build-current-dxmt}
proxy_build=${MACOS_WINE_XR_PROXY_BUILD:-${repo_root}/build-proxy}
hello_build=${MONADO_WINE_HELLO_XR_BUILD_DIR:-${client_root}/hello-xr}
client_source=${client_root}/sources/monado-client
runtime_build=${MONADO_WINE_OPENXR_BUILD_DIR:-${client_root}/build-openxr}
wine=${dxmt_root}/bin/wine-current-dxmt
service=${MONADO_BUILD_DIR}/src/xrt/targets/service/monado-service
control=${MONADO_BUILD_DIR}/src/xrt/targets/service/monado-service-xpc-control
proxy_log=${client_root}/proxy.log
trace_host=${MONADO_WINE_TIMING_TRACE_HOST:-/tmp/macos-wine-xr-current-dxmt.csv}

mkdir -p "${client_root}"

# Build the generic native pieces first.
cmake --build "${MONADO_BUILD_DIR}" \
	--target monado-service monado-service-xpc-control monado_metal_xpc_client --parallel

if [[ ! -x ${service} || ! -x ${control} ]]; then
	print -u2 "Clean Monado service/control binaries were not produced under MONADO_BUILD_DIR."
	exit 1
fi

expected_service=$(realpath "${service}")
launch_target="gui/$(id -u)/org.freedesktop.monado.service"
loaded_service=""
if launchctl print "${launch_target}" >/tmp/mwxr-launchd.$$.txt 2>/dev/null; then
	loaded_service=$(sed -n 's/^[[:space:]]*program = //p' /tmp/mwxr-launchd.$$.txt | head -1)
	[[ -n ${loaded_service} ]] && loaded_service=$(realpath "${loaded_service}" 2>/dev/null || print "${loaded_service}")
fi
rm -f /tmp/mwxr-launchd.$$.txt

if [[ ${loaded_service} != ${expected_service} ]]; then
	if [[ ${MWXR_BOOTSTRAP_SERVICE:-0} != 1 ]]; then
		print -u2 "The loaded Monado LaunchAgent is not this clean build."
		print -u2 "  loaded:   ${loaded_service:-<none>}"
		print -u2 "  expected: ${expected_service}"
		print -u2 ""
		print -u2 "Either bootstrap it yourself, or rerun once with:"
		print -u2 "  MWXR_BOOTSTRAP_SERVICE=1 ..."
		exit 2
	fi
	print "Registering macos-upstream-clean as the development Monado LaunchAgent..."
	"${control}" bootstrap
fi

# Current DXMT and the bridge-owned Windows frontend.
if [[ ! -x ${wine} ]]; then
	"${script_dir}/build-current-dxmt.zsh"
fi
"${script_dir}/build-wine-openxr-client.zsh"

# Build Khronos hello_xr from the disposable Monado client checkout.
MONADO_WINE_HELLO_XR_BUILD_DIR="${hello_build}" \
	"${client_source}/scripts/macos/build-wine-hello-xr-d3d11.zsh" >/dev/null
hello_exe=${hello_build}/khr_hello_xr_d3d11.exe
if [[ ! -f ${hello_exe} ]]; then
	print -u2 "hello_xr was not built: ${hello_exe}"
	exit 1
fi

runtime_dll=""
for candidate in \
	"${runtime_build}/src/xrt/targets/openxr/libopenxr_monado.dll" \
	"${runtime_build}/src/xrt/targets/openxr/openxr_monado.dll"
do
	if [[ -f ${candidate} ]]; then
		runtime_dll=${candidate}
		break
	fi
done
if [[ -z ${runtime_dll} ]]; then
	print -u2 "Could not find transitional Win64 Monado runtime."
	exit 1
fi

# Shared token for the old Windows byte-stream client and the new proxy.
if [[ -z ${IPC_WINE_TCP_TOKEN:-} ]]; then
	export IPC_WINE_TCP_TOKEN=$(python3 - <<'PY'
import secrets
print(secrets.token_hex(32))
PY
)
fi
if [[ ! ${IPC_WINE_TCP_TOKEN} =~ '^[0-9a-f]{64}$' ]]; then
	print -u2 "IPC_WINE_TCP_TOKEN must be 64 lowercase hexadecimal characters"
	exit 2
fi

# Start the standalone proxy. It connects to the normal Unix-socket service and
# republishes current-DXMT Metal resources through the normal XPC broker.
export IPC_WINE_TCP_PORT=${port}
export MONADO_METAL_XPC_CLIENT=${MONADO_METAL_XPC_CLIENT:-${MONADO_BUILD_DIR}/src/xrt/ipc/libmonado_metal_xpc_client.dylib}
MONADO_SOURCE_DIR="${MONADO_SOURCE_DIR}" \
MONADO_BUILD_DIR="${MONADO_BUILD_DIR}" \
IPC_WINE_TCP_PORT="${port}" \
IPC_WINE_TCP_TOKEN="${IPC_WINE_TCP_TOKEN}" \
MONADO_METAL_XPC_CLIENT="${MONADO_METAL_XPC_CLIENT}" \
	"${script_dir}/run-proxy.zsh" >"${proxy_log}" 2>&1 &
proxy_pid=$!

cleanup()
{
	if kill -0 "${proxy_pid}" >/dev/null 2>&1; then
		kill "${proxy_pid}" >/dev/null 2>&1 || true
	fi
	wait "${proxy_pid}" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

for _ in {1..200}; do
	if ! kill -0 "${proxy_pid}" >/dev/null 2>&1; then
		print -u2 "Proxy exited during startup:"
		cat "${proxy_log}" >&2
		exit 1
	fi
	if command -v lsof >/dev/null 2>&1 &&
	   lsof -nP -a -p "${proxy_pid}" -iTCP@"127.0.0.1:${port}" -sTCP:LISTEN 2>/dev/null | grep -q LISTEN; then
		break
	fi
	if grep -q "proxy listening on 127.0.0.1:${port}" "${proxy_log}" 2>/dev/null; then
		break
	fi
	sleep 0.1
done

if ! grep -q "proxy listening on 127.0.0.1:${port}" "${proxy_log}" 2>/dev/null; then
	print -u2 "Proxy did not become ready:"
	cat "${proxy_log}" >&2
	exit 1
fi

# Wine's Windows OpenXR loader ignores XR_RUNTIME_JSON in the high-integrity
# context we observed, so register the private runtime in this private prefix.
run_dir=${client_root}/wine-run
mkdir -p "${run_dir}"
manifest=${run_dir}/openxr_monado-current-dxmt.json
windows_runtime="Z:${runtime_dll//\//\\}"
windows_manifest="Z:${manifest//\//\\}"
escaped_runtime=${windows_runtime//\\/\\\\}
cat > "${manifest}" <<EOF
{
    "file_format_version": "1.0.0",
    "runtime": {
        "library_path": "${escaped_runtime}"
    }
}
EOF

openxr_registry_key='HKLM\SOFTWARE\Khronos\OpenXR\1'
"${wine}" reg.exe add "${openxr_registry_key}" \
	/v ActiveRuntime /t REG_SZ /d "${windows_manifest}" /f >/dev/null

trace_windows="Z:${trace_host//\//\\}"

print ""
print "Running current-DXMT standalone-bridge hello_xr test"
print "  clean Monado: ${expected_service}"
print "  current DXMT: ${dxmt_root}"
print "  PE runtime:   ${runtime_dll}"
print "  proxy:        127.0.0.1:${port}"
print "  proxy log:    ${proxy_log}"
print "  timing trace: ${trace_host}"
print ""

# The transitional PE frontend comes from a different Monado commit while the
# split is being proven. Wire structs/IDs are deliberately kept compatible.
IPC_IGNORE_VERSION=1 \
MONADO_WINE_TCP_PORT="${port}" \
IPC_WINE_TCP_TOKEN="${IPC_WINE_TCP_TOKEN}" \
MONADO_WINE_GPU_SYNC="${MONADO_WINE_GPU_SYNC:-1}" \
MONADO_WINE_TIMING_TRACE="${trace_windows}" \
	"${wine}" "${hello_exe}" --graphics D3D11 --space Local --verbose

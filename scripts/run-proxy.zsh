#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
#
# Build and run the standalone authenticated Wine -> native Monado proxy.
#
# Required:
#   MONADO_SOURCE_DIR=/path/to/NikNakk/monado
#   MONADO_BUILD_DIR=/path/to/that/branch's/configured/build
#
# Optional:
#   IPC_WINE_TCP_PORT=4242
#   IPC_WINE_TCP_TOKEN=<64 lowercase hex chars>
#   MONADO_METAL_XPC_CLIENT=/path/to/libmonado_metal_xpc_client.dylib

set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h}

: ${MONADO_SOURCE_DIR:?Set MONADO_SOURCE_DIR to NikNakk/monado:macos-upstream-clean}
: ${MONADO_BUILD_DIR:?Set MONADO_BUILD_DIR to its configured build directory}

port=${IPC_WINE_TCP_PORT:-4242}
build_dir=${MACOS_WINE_XR_PROXY_BUILD:-${repo_root}/build-proxy}

if [[ -z ${IPC_WINE_TCP_TOKEN:-} ]]; then
	cache_root=${XDG_CACHE_HOME:-${HOME}/Library/Caches}
	token_dir=${cache_root}/macos-wine-xr
	token_file=${token_dir}/wine-tcp-token
	mkdir -p "${token_dir}"
	chmod 700 "${token_dir}"
	if [[ ! -f ${token_file} ]]; then
		umask 077
		python3 - <<'PY' > "${token_file}"
import secrets
print(secrets.token_hex(32))
PY
	fi
	export IPC_WINE_TCP_TOKEN=$(tr -d '\r\n' < "${token_file}")
fi

if [[ ! ${IPC_WINE_TCP_TOKEN} =~ '^[0-9a-f]{64}$' ]]; then
	print -u2 "IPC_WINE_TCP_TOKEN must be 64 lowercase hexadecimal characters"
	exit 2
fi

if [[ -n ${MONADO_METAL_XPC_CLIENT:-} && ! -f ${MONADO_METAL_XPC_CLIENT} ]]; then
	print -u2 "MONADO_METAL_XPC_CLIENT does not exist: ${MONADO_METAL_XPC_CLIENT}"
	exit 1
fi

if [[ -z ${MONADO_METAL_XPC_CLIENT:-} ]]; then
	candidate=$(find "${MONADO_BUILD_DIR}" -name libmonado_metal_xpc_client.dylib -type f -print -quit)
	if [[ -z ${candidate} ]]; then
		print "Building Monado external Metal XPC client..."
		cmake --build "${MONADO_BUILD_DIR}" --target monado_metal_xpc_client --parallel
		candidate=$(find "${MONADO_BUILD_DIR}" -name libmonado_metal_xpc_client.dylib -type f -print -quit)
	fi
	if [[ -z ${candidate} ]]; then
		print -u2 "Could not find libmonado_metal_xpc_client.dylib after build"
		exit 1
	fi
	export MONADO_METAL_XPC_CLIENT=${candidate}
fi

cmake -S "${repo_root}" -B "${build_dir}" \
	-DCMAKE_BUILD_TYPE=Release \
	-DMONADO_SOURCE_DIR="${MONADO_SOURCE_DIR}" \
	-DMONADO_BUILD_DIR="${MONADO_BUILD_DIR}"
cmake --build "${build_dir}" --target macos_wine_xr_proxy --parallel

export IPC_WINE_TCP_PORT=${port}

print ""
print "Proxy environment:"
print "  IPC_WINE_TCP_PORT=${IPC_WINE_TCP_PORT}"
print "  IPC_WINE_TCP_TOKEN=<loaded>"
print "  MONADO_METAL_XPC_CLIENT=${MONADO_METAL_XPC_CLIENT}"
print ""
print "Windows/Wine client environment must contain:"
print "  MONADO_WINE_TCP_PORT=${IPC_WINE_TCP_PORT}"
print "  IPC_WINE_TCP_TOKEN=<same token>"
print ""

exec "${build_dir}/macos_wine_xr_proxy"

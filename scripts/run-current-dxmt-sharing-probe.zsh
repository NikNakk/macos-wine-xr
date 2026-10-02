#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
#
# Cross-process proof for current DXMT native sharing:
#   D3D11 Texture2DArray/fence -> DXMT private metadata -> native Metal reopen.
# If MONADO_METAL_XPC_CLIENT is supplied, also publish the reopened objects
# into Monado's generic XPC broker and print the resulting tokens.

set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h}
root=${MACOS_WINE_XR_CURRENT_DXMT_ROOT:-${repo_root}/build-current-dxmt}
dxmt_source=${DXMT_SOURCE_DIR:-${root}/sources/dxmt}
wine=${root}/bin/wine-current-dxmt
build_native=${root}/bridge-native
build_win=${root}/bridge-win
output=${root}/dxmt-native-sharing-probe.txt

if [[ ! -x ${wine} ]]; then
	print "Current DXMT stack is not built yet; building it first."
	"${script_dir}/build-current-dxmt.zsh"
fi

for tool in cmake x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++; do
	if ! command -v "${tool}" >/dev/null 2>&1; then
		print -u2 "Missing required tool: ${tool}"
		exit 1
	fi
done

cmake -S "${repo_root}" -B "${build_native}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${build_native}" --target native_metal_sharing_probe dxmt_to_monado_token_probe --parallel

cmake -S "${repo_root}" -B "${build_win}" \
	-DCMAKE_SYSTEM_NAME=Windows \
	-DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
	-DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
	-DDXMT_INCLUDE_DIR="${dxmt_source}/include"
cmake --build "${build_win}" --target dxmt_native_sharing_probe --parallel

rm -f "${output}"
MACOS_WINE_XR_PROBE_HOLD_MS=30000 "${wine}" "${build_win}/dxmt_native_sharing_probe.exe" >"${output}" 2>&1 &
producer_pid=$!

cleanup()
{
	if kill -0 "${producer_pid}" >/dev/null 2>&1; then
		kill "${producer_pid}" >/dev/null 2>&1 || true
	fi
	wait "${producer_pid}" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

texture_name=""
fence_name=""
for _ in {1..200}; do
	if [[ -f ${output} ]]; then
		texture_name=$(grep '^texture=' "${output}" | tail -1 | cut -d= -f2- || true)
		fence_name=$(grep '^fence=' "${output}" | tail -1 | cut -d= -f2- || true)
		if [[ -n ${texture_name} && -n ${fence_name} ]]; then
			break
		fi
	fi
	sleep 0.1
done

if [[ -z ${texture_name} || -z ${fence_name} ]]; then
	print -u2 "DXMT producer did not publish both native-sharing names."
	cat "${output}" >&2 || true
	exit 1
fi

print "DXMT metadata:"
print "  texture: ${texture_name}"
print "  fence:   ${fence_name}"

"${build_native}/native_metal_sharing_probe" "${texture_name}" "${fence_name}"

if [[ -n ${MONADO_METAL_XPC_CLIENT:-} ]]; then
	print ""
	print "Publishing resolved resources into Monado XPC..."
	"${build_native}/dxmt_to_monado_token_probe" \
		"${texture_name}" "${fence_name}" "${MONADO_METAL_XPC_CLIENT}"
else
	print ""
	print "Native reopen succeeded."
	print "Set MONADO_METAL_XPC_CLIENT=/path/to/libmonado_metal_xpc_client.dylib"
	print "to extend this test through Monado's generic Metal XPC broker."
fi

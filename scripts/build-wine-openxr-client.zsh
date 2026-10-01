#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
#
# Build the transitional Win64 Monado OpenXR frontend for macOS Wine XR.
#
# The native runtime is NikNakk/monado:macos-upstream-clean. This script uses a
# private disposable checkout of the last integrated Wine-client branch only as
# a build framework for st/oxr and the generated Monado IPC client. The actual
# Wine/D3D11 source files are overlaid from this repository before building.
#
# No tracked file in the user's Monado checkout is modified.

set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h}
root=${MACOS_WINE_XR_CLIENT_ROOT:-${repo_root}/build-wine-client}

readonly monado_repo=https://github.com/NikNakk/monado.git
readonly default_ref=macos-game-mode-upstream-sync-2026-10
monado_ref=${MONADO_WINE_CLIENT_REF:-${default_ref}}

source_dir=${root}/sources/monado-client
build_dir=${MONADO_WINE_OPENXR_BUILD_DIR:-${root}/build-openxr}
dxmt_source=${DXMT_SOURCE_DIR:-${repo_root}/build-current-dxmt/sources/dxmt}

for tool in git cmake ninja x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++ x86_64-w64-mingw32-windres file; do
	if ! command -v "${tool}" >/dev/null 2>&1; then
		print -u2 "Missing required tool: ${tool}"
		print -u2 "Homebrew packages typically needed: cmake ninja mingw-w64"
		exit 1
	fi
done

if [[ ! -f ${dxmt_source}/include/dxmt_native_interop.h ]]; then
	print -u2 "Current DXMT native-sharing header not found:"
	print -u2 "  ${dxmt_source}/include/dxmt_native_interop.h"
	print -u2 "Run scripts/build-current-dxmt.zsh first, or set DXMT_SOURCE_DIR."
	exit 1
fi

mkdir -p "${root}/sources"

if [[ ! -d ${source_dir}/.git ]]; then
	print "Cloning private Monado Windows-client build framework..."
	git clone --depth 1 --branch "${monado_ref}" "${monado_repo}" "${source_dir}"
else
	# This checkout lives entirely under the bridge build root and is owned by
	# this script. Discard the previous source overlay before refreshing it.
	git -C "${source_dir}" reset --hard HEAD >/dev/null
	git -C "${source_dir}" clean -fd >/dev/null
	git -C "${source_dir}" fetch --quiet origin "${monado_ref}"
	git -C "${source_dir}" checkout --quiet -B "${monado_ref}" "origin/${monado_ref}"
fi

# Overlay bridge-owned sources into the disposable checkout. These copies are
# intentionally untracked build inputs and never enter the Monado repository.
cp -f "${repo_root}/src/windows/comp_d3d11_bridge.cpp" \
	"${source_dir}/src/xrt/compositor/client/comp_d3d11_wine_client.cpp"
cp -f "${repo_root}/src/windows/oxr_d3d11_bridge.cpp" \
	"${source_dir}/src/xrt/state_trackers/oxr/oxr_d3d11_wine.cpp"
cp -f "${dxmt_source}/include/dxmt_native_interop.h" \
	"${source_dir}/src/xrt/compositor/client/dxmt_native_interop.h"

cc=${CC_MINGW:-$(command -v x86_64-w64-mingw32-gcc)}
cxx=${CXX_MINGW:-$(command -v x86_64-w64-mingw32-g++)}
windres=${WINDRES_MINGW:-$(command -v x86_64-w64-mingw32-windres)}

cmake -S "${source_dir}" -B "${build_dir}" -G Ninja \
	-UXRT_HAVE_DXGI \
	-UXRT_HAVE_D3D11 \
	-UXRT_HAVE_D3D12 \
	-DCMAKE_SYSTEM_NAME=Windows \
	-DCMAKE_SYSTEM_PROCESSOR=x86_64 \
	-DCMAKE_C_COMPILER="${cc}" \
	-DCMAKE_CXX_COMPILER="${cxx}" \
	-DCMAKE_RC_COMPILER="${windres}" \
	-DCMAKE_BUILD_TYPE=RelWithDebInfo \
	-DXRT_FEATURE_WINE_D3D11_BRIDGE=ON \
	-DXRT_FEATURE_SERVICE=OFF \
	-DXRT_FEATURE_CLIENT_WITHOUT_SERVICE=ON \
	-DXRT_MODULE_COMPOSITOR=ON \
	-DXRT_MODULE_COMPOSITOR_CLIENT=ON \
	-DXRT_MODULE_COMPOSITOR_MAIN=OFF \
	-DXRT_MODULE_COMPOSITOR_MULTI=OFF \
	-DXRT_MODULE_COMPOSITOR_NULL=OFF \
	-DXRT_MODULE_COMPOSITOR_RENDER=OFF \
	-DXRT_MODULE_COMPOSITOR_SHADERS=OFF \
	-DXRT_MODULE_COMPOSITOR_UTIL=OFF \
	-DXRT_MODULE_COMPOSITOR_MOCK=OFF \
	-DXRT_HAVE_VULKAN=OFF \
	-DXRT_HAVE_OPENGL=OFF \
	-DXRT_HAVE_OPENGLES=OFF \
	-DXRT_HAVE_SDL2=OFF \
	-DXRT_MODULE_MONADO_CLI=OFF \
	-DXRT_MODULE_MONADO_GUI=OFF \
	-DXRT_BUILD_SAMPLES=OFF \
	-DBUILD_TESTING=OFF \
	-DXRT_FEATURE_DEBUG_GUI=OFF \
	-DXRT_FEATURE_CLIENT_DEBUG_GUI=OFF \
	-DXRT_FEATURE_TRACING=OFF

cmake --build "${build_dir}" --target openxr_monado --parallel

runtime=""
for candidate in \
	"${build_dir}/src/xrt/targets/openxr/openxr_monado.dll" \
	"${build_dir}/src/xrt/targets/openxr/libopenxr_monado.dll"
do
	if [[ -f ${candidate} ]]; then
		runtime=${candidate}
		break
	fi
done

if [[ -z ${runtime} ]]; then
	print -u2 "Win64 OpenXR build completed but openxr_monado.dll was not found."
	exit 1
fi

description=$(file "${runtime}")
if [[ ${description} != *"PE32+"*"x86-64"* ]]; then
	print -u2 "Unexpected runtime architecture:"
	print -u2 "  ${description}"
	exit 1
fi

manifest=${root}/client-build.txt
cat > "${manifest}" <<EOF
macOS Wine XR transitional Win64 client
Build framework: ${monado_repo}
Build framework ref: ${monado_ref}
Build framework commit: $(git -C "${source_dir}" rev-parse HEAD)
D3D11 bridge source: macos-wine-xr/src/windows/comp_d3d11_bridge.cpp
OpenXR D3D helper: macos-wine-xr/src/windows/oxr_d3d11_bridge.cpp
DXMT native interop header: ${dxmt_source}/include/dxmt_native_interop.h
Runtime DLL: ${runtime}
EOF

print ""
print "Transitional Win64 OpenXR client built:"
print "  runtime: ${runtime}"
print "  build:   ${build_dir}"
print "  source:  private disposable checkout at ${source_dir}"
print ""
print "The native macOS runtime remains macos-upstream-clean."

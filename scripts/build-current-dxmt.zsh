#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
#
# Build current NikNakk/dxmt:macos-xr-native-sharing against the same
# x86_64 Wine/LLVM shape used by DXMT's own CI, then install it into a private
# Wine tree. This does not modify CrossOver, Whisky, /Applications, or any
# existing Wine prefix.

set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h}
root=${MACOS_WINE_XR_CURRENT_DXMT_ROOT:-${repo_root}/build-current-dxmt}

readonly dxmt_repo=https://github.com/NikNakk/dxmt.git
readonly dxmt_branch=macos-xr-native-sharing
readonly wine_version=v8.16-3shain
readonly wine_url=https://github.com/3Shain/wine/releases/download/${wine_version}/wine.tar.gz
readonly llvm_version=llvmorg-15.0.7

sources=${root}/sources
toolchains=${root}/toolchains
wine_root=${toolchains}/wine
dxmt_source=${DXMT_SOURCE_DIR:-${sources}/dxmt}
llvm_root=${DXMT_LLVM_PATH:-${toolchains}/llvm-darwin}
llvm_source=${toolchains}/llvm-project
llvm_build=${toolchains}/llvm-darwin-build
dxmt_build=${root}/dxmt-build
dxmt_install=${root}/dxmt-install
prefix=${MACOS_WINE_XR_WINEPREFIX:-${root}/prefix}
bin_dir=${root}/bin

if [[ $(uname -s) != Darwin ]]; then
	print -u2 "This script must run on macOS."
	exit 1
fi

for tool in git curl tar cmake ninja meson x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++ file shasum; do
	if ! command -v "${tool}" >/dev/null 2>&1; then
		print -u2 "Missing required tool: ${tool}"
		print -u2 "Homebrew packages typically needed: cmake ninja meson mingw-w64"
		exit 1
	fi
done

if [[ $(uname -m) == arm64 ]] && ! arch -x86_64 /usr/bin/true >/dev/null 2>&1; then
	print -u2 "Rosetta 2 is required for the x86_64 Wine/DXMT test stack."
	exit 1
fi

mkdir -p "${sources}" "${toolchains}" "${bin_dir}"

# DXMT source: use an explicitly supplied checkout untouched, otherwise manage
# a private checkout under the build root.
if [[ -z ${DXMT_SOURCE_DIR:-} ]]; then
	if [[ ! -d ${dxmt_source}/.git ]]; then
		git clone --recursive --branch "${dxmt_branch}" "${dxmt_repo}" "${dxmt_source}"
	else
		if [[ -n $(git -C "${dxmt_source}" status --porcelain --untracked-files=no) ]]; then
			print -u2 "Managed DXMT checkout has tracked modifications; refusing to reset:"
			git -C "${dxmt_source}" status --short >&2
			exit 1
		fi
		git -C "${dxmt_source}" fetch --quiet origin "${dxmt_branch}"
		git -C "${dxmt_source}" checkout --quiet "${dxmt_branch}"
		git -C "${dxmt_source}" reset --hard "origin/${dxmt_branch}"
		git -C "${dxmt_source}" submodule update --init --recursive
	fi
else
	if [[ ! -f ${dxmt_source}/include/dxmt_native_interop.h ]]; then
		print -u2 "DXMT_SOURCE_DIR does not contain the native-sharing branch: ${dxmt_source}"
		exit 1
	fi
fi

# Wine install tree, pinned to the release currently used by DXMT's x86_64 CI.
if [[ ! -x ${wine_root}/bin/wine ]]; then
	archive=${root}/wine-${wine_version}.tar.gz
	if [[ ! -f ${archive} ]]; then
		print "Downloading DXMT CI Wine ${wine_version}..."
		curl --fail --location --progress-bar -o "${archive}.partial" "${wine_url}"
		mv "${archive}.partial" "${archive}"
	fi
	rm -rf "${wine_root}"
	mkdir -p "${wine_root}"
	tar -xzf "${archive}" -C "${wine_root}"
fi

if [[ ! -x ${wine_root}/bin/winebuild ]]; then
	print -u2 "Unexpected Wine archive layout: missing ${wine_root}/bin/winebuild"
	exit 1
fi

# LLVM 15 x86_64 Darwin, matching current DXMT's documented requirement.
if [[ ! -f ${llvm_root}/lib/libLLVMCore.a && ! -f ${llvm_root}/lib/libLLVMCore.dylib ]]; then
	if [[ -n ${DXMT_LLVM_PATH:-} ]]; then
		print -u2 "DXMT_LLVM_PATH does not look like an LLVM 15 install: ${llvm_root}"
		exit 1
	fi
	if [[ ! -d ${llvm_source}/.git ]]; then
		git clone --depth 1 --branch "${llvm_version}" https://github.com/llvm/llvm-project.git "${llvm_source}"
	fi
	rm -rf "${llvm_build}"
	cmake -B "${llvm_build}" -S "${llvm_source}/llvm" \
		-DCMAKE_INSTALL_PREFIX="${llvm_root}" \
		-DCMAKE_OSX_ARCHITECTURES=x86_64 \
		-DLLVM_HOST_TRIPLE=x86_64-apple-darwin \
		-DLLVM_ENABLE_ASSERTIONS=On \
		-DLLVM_ENABLE_ZSTD=Off \
		-DCMAKE_BUILD_TYPE=Release \
		-DLLVM_TARGETS_TO_BUILD="" \
		-DLLVM_BUILD_TOOLS=Off \
		-DBUG_REPORT_URL="https://github.com/3Shain/dxmt" \
		-DPACKAGE_VENDOR="DXMT" \
		-DLLVM_VERSION_PRINTER_SHOW_HOST_TARGET_INFO=Off \
		-G Ninja
	cmake --build "${llvm_build}"
	cmake --install "${llvm_build}"
fi

rm -rf "${dxmt_build}" "${dxmt_install}"
meson setup \
	--cross-file "${dxmt_source}/build-win64.txt" \
	-Dnative_llvm_path="${llvm_root}" \
	-Dwine_install_path="${wine_root}" \
	-Dwine_builtin_dll=true \
	"${dxmt_build}" "${dxmt_source}" \
	--buildtype release \
	--prefix "${dxmt_install}" \
	--strip
meson compile -C "${dxmt_build}"
meson install -C "${dxmt_build}"

windows_dxmt=${dxmt_install}/x86_64-windows
unix_dxmt=${dxmt_install}/x86_64-unix
for artifact in d3d10core.dll d3d11.dll dxgi.dll winemetal.dll; do
	if [[ ! -s ${windows_dxmt}/${artifact} ]]; then
		print -u2 "Missing current DXMT artifact: ${windows_dxmt}/${artifact}"
		exit 1
	fi
done
if [[ ! -s ${unix_dxmt}/winemetal.so ]]; then
	print -u2 "Missing current DXMT unixlib: ${unix_dxmt}/winemetal.so"
	exit 1
fi

# Builtin-DLL install layout recommended by DXMT itself.
mkdir -p "${wine_root}/lib/wine/x86_64-windows" "${wine_root}/lib/wine/x86_64-unix"
cp -f "${windows_dxmt}/d3d10core.dll" "${wine_root}/lib/wine/x86_64-windows/"
cp -f "${windows_dxmt}/d3d11.dll" "${wine_root}/lib/wine/x86_64-windows/"
cp -f "${windows_dxmt}/dxgi.dll" "${wine_root}/lib/wine/x86_64-windows/"
cp -f "${windows_dxmt}/winemetal.dll" "${wine_root}/lib/wine/x86_64-windows/"
cp -f "${unix_dxmt}/winemetal.so" "${wine_root}/lib/wine/x86_64-unix/"

mkdir -p "${prefix}"
WINEPREFIX="${prefix}" WINEARCH=win64 WINEDEBUG=-all WINEDLLOVERRIDES= \
	"${wine_root}/bin/wineboot" -u

system32=${prefix}/drive_c/windows/system32
mkdir -p "${system32}"
cp -f "${windows_dxmt}/winemetal.dll" "${system32}/winemetal.dll"

wrapper=${bin_dir}/wine-current-dxmt
cat > "${wrapper}" <<'WRAPPER'
#!/bin/zsh
set -euo pipefail
root=${0:A:h:h}
export WINEPREFIX=${MACOS_WINE_XR_WINEPREFIX:-${root}/prefix}
export WINEARCH=win64
export WINEDEBUG=${WINEDEBUG:--all}
# Current DXMT is built as Wine builtin DLLs. Explicit native,builtin overrides
# for dxgi/d3d11/d3d10core are wrong for this configuration.
unset WINEDLLOVERRIDES
exec "${root}/toolchains/wine/bin/wine" "$@"
WRAPPER
chmod +x "${wrapper}"

manifest=${root}/manifest.txt
wine_sha=$(shasum -a 256 "${root}/wine-${wine_version}.tar.gz" | awk '{print $1}')
cat > "${manifest}" <<EOF
macOS Wine XR current-DXMT toolchain
DXMT repo: ${dxmt_repo}
DXMT branch: ${dxmt_branch}
DXMT commit: $(git -C "${dxmt_source}" rev-parse HEAD)
DXMT license: LGPL-2.1-or-later for current branch changes
Wine: 3Shain ${wine_version}
Wine archive SHA-256: ${wine_sha}
LLVM: ${llvm_version} x86_64-apple-darwin
Wine prefix: ${prefix}
DXMT install: ${dxmt_install}
EOF

print ""
print "Current DXMT toolchain installed privately."
print "  root:    ${root}"
print "  DXMT:    $(git -C "${dxmt_source}" rev-parse --short HEAD)"
print "  Wine:    ${wine_root}"
print "  prefix:  ${prefix}"
print "  wrapper: ${wrapper}"
print ""
print "No existing Wine/CrossOver/Whisky installation was modified."

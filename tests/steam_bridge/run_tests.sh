#!/bin/sh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Tests of the Steam bridge that need neither Wine nor Steam (CI, or locally):
#   - the generated method table is current (needs STEAMWORKS_HEADERS: Proton's
#     lsteamclient/steamworks_sdk_* directories; skipped otherwise);
#   - abi_test and native_test, natively on x86-64 (on Apple silicon under
#     Rosetta when it is installed);
#   - the Windows half and the stand-in compile and link with MinGW against Wine's
#     headers (WINE_INCLUDE: a Wine source tree's include directory).
set -eu
repo=$(cd "$(dirname "$0")/../.." && pwd)
src=$repo/src/steam_bridge
out=${STEAM_BRIDGE_TEST_BUILD:-$repo/build-steam-bridge-tests}
mkdir -p "$out"

if [ -n "${STEAMWORKS_HEADERS:-}" ]; then
	# shellcheck disable=SC2086
	python3 "$repo/tools/generate_steam_bridge_table.py" --output "$out/steam_methods.h" $STEAMWORKS_HEADERS
	cmp "$out/steam_methods.h" "$src/steam_methods.h" || { echo "steam_methods.h is out of date: regenerate it"; exit 1; }
fi

cc=${CC:-cc}
arch=
run=
case "$(uname -s)-$(uname -m)" in
	Darwin-arm64)
		arch="-arch x86_64"
		if arch -x86_64 /usr/bin/true 2>/dev/null; then run="arch -x86_64"; else run=skip; fi ;;
	*-x86_64) ;;
	*) run=skip ;;
esac
ldl=; [ "$(uname -s)" = Linux ] && ldl="-ldl -lpthread"
# shellcheck disable=SC2086
$cc $arch -std=c11 -O2 -Wall -Wextra -Werror -I"$src" "$src/bridge_core.c" "$src/stubs.S" "$src/sysv_invoke.S" \
	"$repo/tests/steam_bridge/abi_test.c" -o "$out/abi_test"
# shellcheck disable=SC2086
$cc $arch -shared -fPIC -O2 "$repo/tests/steam_bridge/fake_client.c" -o "$out/fake_client.so"
# shellcheck disable=SC2086
$cc $arch -std=c11 -O2 -Wall -Wextra -Werror -DMWXR_STEAM_BRIDGE_TEST -I"$src" "$src/steamclient_unix.c" \
	"$src/sysv_invoke.S" "$repo/tests/steam_bridge/native_test.c" -o "$out/native_test" $ldl
if [ "$run" = skip ]; then
	echo "steam bridge: x86-64 tests built, not run (no x86-64 execution here)"
else
	$run "$out/abi_test"
	$run "$out/native_test" "$out/fake_client.so"
fi

if [ -n "${WINE_INCLUDE:-}" ] && command -v x86_64-w64-mingw32-gcc >/dev/null; then
	mingw="x86_64-w64-mingw32-gcc -std=gnu11 -O2 -Wall -Wextra -Werror"
	wine="-D__WINESRC__ -D_WIN64 -DWINE_NO_LONG_TYPES -D_UCRT -nostdinc -isystem $WINE_INCLUDE/msvcrt -I$WINE_INCLUDE"
	wine="$wine -isystem $(x86_64-w64-mingw32-gcc -print-file-name=include)"
	for f in steamclient_main bridge_core; do
		# shellcheck disable=SC2086
		$mingw $wine -I"$src" -c "$src/$f.c" -o "$out/$f.obj"
	done
	x86_64-w64-mingw32-gcc -c "$src/stubs.S" -o "$out/stubs.obj"
	# Wine's runtime symbols are stood in for: this checks the exports and that nothing else is missing.
	cat > "$out/wine_symbols.c" <<'EOF'
#include <stdint.h>
uint64_t __wine_unixlib_handle;
long (__stdcall *__wine_unix_call_dispatcher)(uint64_t, unsigned, void *);
long __stdcall __wine_init_unix_call(void) { return 0; }
unsigned char __cdecl __wine_dbg_get_channel_flags(void *c) { (void)c; return 0; }
const char *__cdecl __wine_dbg_strdup(const char *s) { return s; }
int __cdecl __wine_dbg_output(const char *s) { (void)s; return 0; }
int __cdecl __wine_dbg_header(int a, void *b, const char *c) { (void)a, (void)b, (void)c; return 0; }
EOF
	x86_64-w64-mingw32-gcc -c "$out/wine_symbols.c" -o "$out/wine_symbols.obj"
	x86_64-w64-mingw32-gcc -shared -o "$out/steamclient64.dll" "$out/steamclient_main.obj" "$out/bridge_core.obj" \
		"$out/stubs.obj" "$out/wine_symbols.obj" "$src/steamclient64.def" -lucrtbase
	exports=$(x86_64-w64-mingw32-objdump -p "$out/steamclient64.dll" | grep -c -E '\] (Steam_|Breakpad_|CreateInterface)')
	[ "$exports" -eq 41 ] || { echo "steamclient64.dll exports $exports functions, expected 41"; exit 1; }
	# shellcheck disable=SC2086
	$mingw -municode -static "$src/steam_standin.c" -luser32 -ladvapi32 -o "$out/steam_standin.exe"
	# shellcheck disable=SC2086
	$mingw -static "$repo/tests/steam_bridge/steam_bridge_smoke.c" -o "$out/steam_bridge_smoke.exe"
	echo "steam bridge: Windows half links with 41 exports; stand-in and smoke test build"
	if [ "$(uname -s)" = Darwin ]; then
		# shellcheck disable=SC2086
		clang -arch x86_64 -shared -std=gnu11 -O2 -Wall -Wextra -Werror -DWINE_UNIX_LIB -D_WIN64 -I"$WINE_INCLUDE" \
			-I"$src" "$src/steamclient_unix.c" "$src/sysv_invoke.S" -o "$out/steamclient64.so"
		echo "steam bridge: native half builds for macOS x86_64 against Wine's headers"
	fi
fi
echo "steam bridge tests: ok"

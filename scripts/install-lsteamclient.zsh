#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Install what scripts/build-lsteamclient.zsh built into a CrossOver Wine runtime and a
# prefix, so games can use the macOS Steam client (docs/lsteamclient.md):
#   runtime: lsteamclient.dll and .so, and the ntdll with the lsteamclient redirect
#            (the runtime's ntdll is kept as *.pre-lsteamclient the first time)
#   prefix:  lsteamclient.dll in system32 (Wine loads builtins only from there once a
#            prefix exists)
# The redirect stays off unless a launch sets MWXR_LSTEAMCLIENT=1, so the runtime still
# works with a Windows Steam in the prefix.
#
# Usage: install-lsteamclient.zsh RUNTIME PREFIX
#   For a separate copy of a runtime to test with: cp -cR runtime runtime-lsteamclient
# Optional: WINE_BUILD (default ~/Code/wine-crossover/build), OUT (default
# build-in-process/lsteamclient in this repository).
set -euo pipefail
repo=${0:A:h:h}
(( $# == 2 )) || { print -u2 "usage: install-lsteamclient.zsh RUNTIME PREFIX"; exit 2; }
runtime=${1:A} prefix=${2:A}
WINE_BUILD=${WINE_BUILD:-$HOME/Code/wine-crossover/build}
OUT=${OUT:-${repo}/build-in-process/lsteamclient}

pe=${runtime}/lib/wine/x86_64-windows unix=${runtime}/lib/wine/x86_64-unix
[[ -d ${pe} && -d ${unix} ]] || { print -u2 "${runtime} is not a Wine runtime"; exit 1; }
[[ -d ${prefix}/drive_c/windows/system32 ]] || { print -u2 "${prefix} is not a Wine prefix"; exit 1; }
for f in lsteamclient.dll lsteamclient.so; do
  [[ -f ${OUT}/${f} ]] || { print -u2 "Build first: scripts/build-lsteamclient.zsh (${OUT}/${f} missing)"; exit 1; }
done
ntdll_pe=${WINE_BUILD}/dlls/ntdll/x86_64-windows/ntdll.dll ntdll_unix=${WINE_BUILD}/dlls/ntdll/ntdll.so
python3 -c 'import sys; sys.exit(0 if "MWXR_LSTEAMCLIENT".encode("utf-16-le") in open(sys.argv[1], "rb").read() else 1)' \
  "${ntdll_pe}" || { print -u2 "${ntdll_pe} lacks the lsteamclient redirect (patches/crossover/0007)"; exit 1; }
pgrep -f "${runtime}/" >/dev/null && { print -u2 "Processes from ${runtime} are running; stop them first"; exit 1; }

install_file() {  # source destination; keeps the first original as *.pre-lsteamclient
  [[ -f $2 && ! -f $2.pre-lsteamclient ]] && cp -p "$2" "$2.pre-lsteamclient"
  cp "$1" "$2.new" && mv "$2.new" "$2"
  print "installed ${2}"
}
install_file "${ntdll_pe}" "${pe}/ntdll.dll"
install_file "${ntdll_unix}" "${unix}/ntdll.so"
install_file "${OUT}/lsteamclient.dll" "${pe}/lsteamclient.dll"
install_file "${OUT}/lsteamclient.so" "${unix}/lsteamclient.so"
cp "${OUT}/lsteamclient.dll" "${prefix}/drive_c/windows/system32/lsteamclient.dll"
print "installed ${prefix}/drive_c/windows/system32/lsteamclient.dll"

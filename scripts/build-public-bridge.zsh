#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# Shared DXMT bridge for public Windows Steam or private native Steam profiles.
set -euo pipefail
repo=${0:A:h:h}
core=${repo}/components/threshold-wine-xr
[[ -f ${core}/scripts/build-in-process-gate.zsh ]] || {
 print -u2 'Initialize the public bridge: git submodule update --init components/threshold-wine-xr'; exit 2
}
export MWXR_IN_PROCESS_BUILD=${MWXR_IN_PROCESS_BUILD:-${repo}/build-in-process/public-core}
exec zsh "${core}/scripts/build-in-process-gate.zsh" "$@"

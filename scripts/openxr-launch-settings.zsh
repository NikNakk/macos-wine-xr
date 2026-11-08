# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# Sourced by run-in-process-openxr.zsh. Steam Play exports MWXR_* launch options.
# Leave inherited runtime settings alone unless explicitly overridden for this launch.
if [[ -n ${MWXR_CLIENT_COMPOSITOR+x} ]]; then
  case ${MWXR_CLIENT_COMPOSITOR} in
    0|1) export XRT_MACOS_CLIENT_COMPOSITOR=${MWXR_CLIENT_COMPOSITOR} ;;
    *) print -u2 'MWXR_CLIENT_COMPOSITOR must be 0 or 1'; return 2 ;;
  esac
fi
if [[ -n ${MWXR_VIEWPORT_SCALE_PERCENTAGE+x} ]]; then
  scale=${MWXR_VIEWPORT_SCALE_PERCENTAGE}
  if [[ ${scale} != <-> ]] || (( scale < 1 || scale > 200 )); then
    print -u2 'MWXR_VIEWPORT_SCALE_PERCENTAGE must be an integer from 1 to 200'
    return 2
  fi
  export OXR_VIEWPORT_SCALE_PERCENTAGE=${scale}
  unset scale
fi
print -u2 "OpenXR launch: client_compositor=${XRT_MACOS_CLIENT_COMPOSITOR:-runtime-default} viewport_scale=${OXR_VIEWPORT_SCALE_PERCENTAGE:-runtime-default}%"

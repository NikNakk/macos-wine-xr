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
# MWXR_FOVEATION=off|fixed|gaze, optionally ":OUTER[,CORE,EDGE[,STEP]]" (docs/foveation.md):
# stay-warped foveated rendering for a D3D11 game on DXMT. OUTER is the peripheral rate per
# axis (0.1-1, default 0.3); CORE and EDGE the angles in degrees where the rate starts to
# fall and reaches OUTER around the gaze (default 12,27); STEP the gaze quantisation.
# "fixed" keeps the centre straight ahead with a 20/35 degree core. Needs a Wine runtime
# with the foveation DXMT (MWXR_IN_PROCESS_WINE) and the matching wineopenxr build.
if [[ -n ${MWXR_FOVEATION+x} && ${MWXR_FOVEATION} != off ]]; then
  if [[ ${MWXR_FOVEATION} != (fixed|gaze)(|:<->(|.<->)(|,<->(|.<->),<->(|.<->)(|,<->(|.<->)))) ]]; then
    print -u2 'MWXR_FOVEATION must be off, fixed or gaze, optionally :OUTER[,CORE,EDGE[,STEP]]'
    return 2
  fi
  fov_numbers=${${MWXR_FOVEATION#*:}:#(fixed|gaze)}
  fov_parts=(${(s:,:)fov_numbers})
  if (( ${#fov_parts} >= 1 )) && (( fov_parts[1] < 0.1 || fov_parts[1] > 1 )); then
    print -u2 'MWXR_FOVEATION: OUTER must be from 0.1 to 1'; return 2
  fi
  if (( ${#fov_parts} >= 3 )) && (( fov_parts[2] < 1 || fov_parts[3] <= fov_parts[2] || fov_parts[3] > 90 )); then
    print -u2 'MWXR_FOVEATION: need 1 <= CORE < EDGE <= 90 degrees'; return 2
  fi
  fov_wine=${MWXR_IN_PROCESS_WINE:-${MWXR_WINE_TREE:-$HOME/Code/wine-crossover/runtime}}
  fov_root=${${(%):-%x}:A:h:h}/build-in-process
  export MWXR_GAZE_FOVEATION=${fov_numbers:-0.3}
  if [[ ${MWXR_FOVEATION} == fixed* ]]; then export MWXR_GAZE_FOVEATION_FIXED=1; else unset MWXR_GAZE_FOVEATION_FIXED; fi
  if [[ ${MWXR_VULKAN:-} == kosmickrisp ]]; then
    # A Vulkan game: KosmicKrisp foveates its multisampled eye targets with the profile
    # the bridge sends it (docs/kk-implicit-foveation.md); DXMT is not involved.
    export KK_IMPLICIT_FDM=auto MWXR_KK_FOVEATION=1
    [[ ,${MESA_KK_EXPERIMENTAL:-}, == *,fdm_rate,* ]] ||
      export MESA_KK_EXPERIMENTAL=${MESA_KK_EXPERIMENTAL:+${MESA_KK_EXPERIMENTAL},}fdm_rate
    export MWXR_KK_ROOT=${MWXR_KK_ROOT:-${fov_root}/zink-fov}
    export MWXR_IN_PROCESS_BUILD=${MWXR_FOVEATION_BUILD:-${fov_root}/vk-fov-gate}
    fov_wine="${fov_wine} (KosmicKrisp ${MWXR_KK_ROOT})"
  else
    if [[ ! -f ${fov_wine}/share/mwxr-foveation-dxmt ]]; then
      print -u2 "MWXR_FOVEATION: ${fov_wine} has no foveation DXMT (docs/foveation.md)"; return 2
    fi
    export DXMT_STAY_WARPED=external DXMT_RATE_MAP_POSITION=1 DXMT_RATE_MAP_READ=1
    export DXMT_RATE_MAP_ATTACHMENT_PROBE=1 DXMT_RATE_MAP_RESOURCE_READ=1
    # The rig wrappers name the ordinary wineopenxr build; foveation needs the one with the map.
    export MWXR_IN_PROCESS_BUILD=${MWXR_FOVEATION_BUILD:-${fov_root}/gaze-gate}
  fi
  print -u2 "OpenXR launch: foveation=${MWXR_FOVEATION%%:*} numbers=${MWXR_GAZE_FOVEATION} runtime=${fov_wine}"
  unset fov_numbers fov_parts fov_wine fov_root
fi
print -u2 "OpenXR launch: client_compositor=${XRT_MACOS_CLIENT_COMPOSITOR:-runtime-default} viewport_scale=${OXR_VIEWPORT_SCALE_PERCENTAGE:-runtime-default}%"

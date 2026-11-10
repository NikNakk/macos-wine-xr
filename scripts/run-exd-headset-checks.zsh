#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
# The PS VR2 checks that need someone wearing the headset, one timed EXD run each:
#   1 corners   Monado's default corner mask, for reference
#   2 mask09    lens mask at r2 0.9 (about 26% hidden)
#   3 mask08    lens mask at r2 0.8 (about 44% hidden)
#   4 quad      quad views on the graphics compositor, fixed inset
#   5 gaze      quad views with the inset following your eyes
#   6 scale50   50% view scale, the compositor's usual bilinear filter
#   7 sharp50   50% view scale, Catmull-Rom (green) compositor filter
#   8 depthoff  EXD submitting depth, reprojection off (needs the depth-gate bridge and
#               the build-in-process/exd-depth copy with the depth-import DXMT patch)
#   9 depthon   the same with the opt-in depth reprojection on. It has no disocclusion
#               fill, so expect the known silhouettes and trails; optional (--only 1,2,3,4,5,6,7
#               skips them).
# Before each run it says what to look for and waits for Enter; afterwards it asks for
# an optional one-line note. At the end it prints GPU time per frame with the notes.
# Usage (from the main checkout, which has the DXMT investigation's scripts):
#   scripts/run-exd-headset-checks.zsh [--only 2,3] [--seconds 90] [--no-prompt]
# Needs the Monado build from branch claude/quad-gaze-inset (service and x86_64 client,
# rebuilt together so their versions match) and the quad-gate bridge build.
# MWXR_CHECKS_SERVICE_EXTRA="K=V ..." adds service settings to every run (for a dry run
# without the headset: SIMULATED_DESKTOP=1 XRT_MACOS_REFRESH_RATE_HZ=0).
set -euo pipefail
repo=${0:A:h:h}
monado=${MWXR_CHECKS_MONADO_BUILD:-$HOME/Code/alyx-native/build}
service_bin=${monado}/monado-quad-gaze-arm64/src/xrt/targets/service/monado-service
runtime_json=${monado}/monado-quad-gaze-x86_64/openxr_monado-dev.json
only='' seconds=90 prompt=1
while (( $# )); do
 case $1 in
  --only) only=,$2,; shift 2 ;;
  --seconds) seconds=$2; shift 2 ;;
  --no-prompt) prompt=0; shift ;;
  *) print -u2 "unknown option $1"; exit 2 ;;
 esac
done
[[ -x ${service_bin} && -f ${runtime_json} ]] || { print -u2 "Monado quad-gaze build missing under ${monado}"; exit 2; }
[[ -x ${repo}/scripts/run-exd-scene-trace.zsh ]] || { print -u2 'run from the main checkout (needs scripts/run-exd-scene-trace.zsh)'; exit 2; }

# tag | game config | service environment | game environment | what to look for
steps=(
 "corners|msaa2|PSVR2_VISIBILITY_MASK=0|MWXR_QUAD_VIEWS=0|Reference. Note how far out the image reaches: look straight ahead, then move only your eyes to each edge and corner."
 "mask09|msaa2|PSVR2_VISIBILITY_MASK_R2=0.9|MWXR_QUAD_VIEWS=0|Lens mask, about a quarter hidden. Look for a black rim or a ring at the edge of the view, eyes ahead and at the edges, and while turning your head quickly."
 "mask08|msaa2|PSVR2_VISIBILITY_MASK_R2=0.8|MWXR_QUAD_VIEWS=0|Lens mask, about 44% hidden. Same as before: is the black edge visible now, and where (sides, top, bottom)?"
 "quad|msaa0|XRT_COMPOSITOR_QUAD_VIEW_FAST_PATH=1|MWXR_QUAD_CONTEXT_SCALE=0.4 MWXR_QUAD_INSET_SCALE=0.5|Quad views on the normal compositor. The centre is sharp and the outside blurrier. Look for a visible box or seam where they meet, shimmer, or anything wrong in either eye."
 "gaze|msaa0|XRT_COMPOSITOR_QUAD_VIEW_FAST_PATH=1|MWXR_QUAD_CONTEXT_SCALE=0.4 MWXR_QUAD_INSET_SCALE=0.25 OXR_QUAD_INSET_GAZE=1 OXR_QUAD_INSET_DEGREES=30 OXR_QUAD_INSET_LOG=1|Gaze-following inset (30 degrees). Look around with your eyes: does the sharp area follow, how late does it arrive, and does it pop or flicker?"
 "scale50|msaa2|XRT_COMPOSITOR_SOURCE_FILTER=bilinear|MWXR_QUAD_VIEWS=0 MWXR_VIEWPORT_SCALE_PERCENTAGE=50|Half resolution with the usual filter, for reference. Note how soft text and fine edges look at the centre and edges."
 "sharp50|msaa2|XRT_COMPOSITOR_SOURCE_FILTER=catmull-rom-green|MWXR_QUAD_VIEWS=0 MWXR_VIEWPORT_SCALE_PERCENTAGE=50|Half resolution with the sharper compositor filter. Compared with the last check: is it sharper, and is there any ringing (halos) or colour fringing at edges?"
 "depthoff|msaa2|XRT_COMPOSITOR_DEPTH_REPROJECTION=0|MWXR_QUAD_VIEWS=0 MWXR_EXD_BUILD=depth-gate MWXR_EXD_EXPERIMENT_ROOT=${repo}/build-in-process/exd-depth|Reference for depth: stand near an object with things behind it, then lean and step side to side. Note how the scene judders or swims as you move."
 "depthon|msaa2|XRT_COMPOSITOR_DEPTH_REPROJECTION=1|MWXR_QUAD_VIEWS=0 MWXR_EXD_BUILD=depth-gate MWXR_EXD_EXPERIMENT_ROOT=${repo}/build-in-process/exd-depth|Depth reprojection on (no disocclusion fill, so artefacts are expected). Lean and step the same way: how bad are the trails and doubled edges around near objects, and is anything smoother?"
)

summary=${repo}/build-in-process/exd-ngx/headset-checks-$(date +%Y%m%d-%H%M%S).txt
print "PS VR2 headset checks, $(date)" > "${summary}"
n=0
for step in "${steps[@]}"; do
 (( ++n ))
 [[ -z ${only} || ${only} == *,${n},* ]] || continue
 IFS='|' read -r tag config service_env game_env advice <<< "${step}"
 print "\n== Check ${n}: ${tag}\n${advice}"
 (( prompt )) && read -r "?Press Enter when you are ready (headset on)... "
 for i in {1..30}; do pgrep -x monado-service >/dev/null || break; sleep 1; done
 # The run script restores the game's settings and stops its private service itself.
 env MWXR_EXD_BUILD=quad-gate ${=game_env} MWXR_EXD_TAG=check-${tag} \
  MWXR_EXD_SERVICE_ENV="XRT_LOG=info PSVR2_LOG=info ${service_env} ${MWXR_CHECKS_SERVICE_EXTRA:-}" \
  MWXR_EXD_SERVICE_BIN=${service_bin} MWXR_EXD_RUNTIME_JSON=${runtime_json} \
  "${repo}/scripts/run-exd-headset-compare.zsh" ${config} ${seconds} > /dev/null 2>&1 || print "run failed (exit $?)"
 run=$(ls -td "${repo}"/build-in-process/exd-*/headset-*-check-${tag} | head -1)
 note=''
 (( prompt )) && read -r "note?What did you see? (one line, Enter to skip) "
 # GPU time over 600 frames of the loaded level, starting 300 frames after it appears.
 timing=$(python3 -I - "${repo}/tools/analyze_dxmt_tbdr_audit.py" "${run}/game/game.log" "${run}/frames.csv" <<'PY'
import csv, statistics as st, subprocess, sys
tool, log, out = sys.argv[1:]
subprocess.run([sys.executable, tool, log, '--min-frame', '0', '--max-frame', '1000000', '--frames-csv', out],
               check=True, stdout=subprocess.DEVNULL)
rows = list(csv.DictReader(open(out)))
start = next((i for i, r in enumerate(rows) if int(r['frame']) > 300 and int(r['render_after']) >= 50), None)
window = rows[start + 300:start + 900] if start is not None else []
if len(window) < 100:
    print('level not reached')
else:
    gpu = st.median(float(r['cb_interval_union_ms']) for r in window)
    t = [int(r['start_ns']) for r in window]
    period = st.median((b - a) / 1e6 for a, b in zip(t, t[1:]))
    print(f'GPU {gpu:.1f} ms, frame {period:.1f} ms (~{1000 / period:.0f} fps)')
PY
) || timing='timing unavailable'
 line="${n} ${tag}: ${timing}${note:+ | ${note}}  (${run:t})"
 print "${line}"
 print "${line}" >> "${summary}"
done
print "\nSummary in ${summary}:"
cat "${summary}"

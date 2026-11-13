# EXD map-only warning fix and repeat measurements

2026-11-11. Follow-up to [the stay-warped trial](dxmt-exd-stay-warped-trial.md).

The subsequent [shader-component ablation](dxmt-shader-ablation.md) now isolates
the texture-read shader variant as the dominant reproducible no-map penalty,
with host setup held constant and a fresh shader cache.

**The warning flood is fixed. It inflated CPU preparation work, but removing it
did not lower the observed map-only GPU interval to the ordinary baseline.**
The new flags-only controls are almost as expensive as binding the map. These
results supersede the earlier attribution of the entire 2.2–2.9 ms residual to
unavoidable Metal rate-map hardware cost.

## Fix

`dxmt_context.cpp` now warns only when the texture's layout actually differs
from the selected read layout. `DXMT_STAY_WARPED_DEBUG=3` deliberately disables
translation for identity-map diagnostics; that choice no longer falls through
to the misleading second-layout warning. Real mismatches retain the warning.
The same one-line condition change is preserved in patch 0017; applying that
patch in reverse with `git apply --reverse --check` succeeds on the fixed
private source. There is no shader translation or rendering policy change.

Rebuilt `d3d11.dll` and `dxgi.dll` in `build-in-process/dxmt-warp-build` and
copied them into **both** the private `exd-warp/wine` runtime and its prefix's
`drive_c/windows/system32`. The existing prefix DLLs differed from the runtime
copies, and runtime-only replacement did not activate the rebuilt instrumentation
in EXD. Both sets of originals are retained under
`build-in-process/exd-warp/warning-fix-backup-20261111/`, with hashes.
The installed runtime, Steam/game copies and installed LaunchAgent were not
modified. The corrected private copies remain deployed for further work.

## Retest

Apple M5, simulated desktop HMD, service compositor, 2560×1440 double-wide,
4× MSAA main scene, upscaler off. Same existing EXD comparison launcher and
90-second duration. Order: baseline, flags only, map only, map only, flags
only, baseline. Flags only uses `DXMT_STAY_WARPED=0x0`: translation-capable
variants are enabled, but there is no map. Map only uses peripheral rate 1.0
and debug bits 3: a map is bound and both coordinate translations are skipped.

Frame selection: `300 <= frame < 4000`, then `render_after >= 55` for the loaded
scene. GPU figures are median per-frame unions of completed DXMT command-buffer
intervals, including waits; they are not isolated shader-active times. CPU
preparation is the median sum of instrumented preparation work per frame.

| Run | Case | Loaded frames | GPU interval union (ms) | CPU preparation (ms) | Spurious layout warnings |
| --- | --- | ---: | ---: | ---: | ---: |
| A1 | Ordinary baseline | 3,234 | 16.483 | 0.452 | 0 |
| F1 | Flags only, no map | 3,236 | 18.942 | 0.461 | 0 |
| M1 | Identity map, translations skipped | 3,232 | 19.279 | 0.464 | 0 |
| M2 | Identity map, translations skipped | 3,223 | 19.474 | 0.468 | 0 |
| F2 | Flags only, no map | 3,222 | 19.384 | 0.486 | 0 |
| A2 | Ordinary baseline | 3,223 | 17.410 | 0.444 | 0 |

Every trial reports nominal thermal state (0), Low Power Mode false, 59 render
nodes after optimisation, seven command buffers and 409 counted fence waits
per loaded frame (medians). Only M1/M2 have mapped render nodes: 19 each.
All three private game configuration files match their pre-run hashes after
each successful trial and after the final fixture checks.

Averaging the two run medians per case gives:

| Case | GPU ms | Difference from baseline |
| --- | ---: | ---: |
| Ordinary baseline | 16.947 | — |
| Flags only, no map | 19.163 | +2.216 ms |
| Map only | 19.377 | +2.430 ms |

The incremental map-only versus flags-only difference is **0.214 ms (1.1%)**.
Their run ranges overlap, while baseline drift is 0.927 ms and flags-only drift
is 0.442 ms. This does not resolve a causal 0.214 ms map penalty or establish
an upper bound. Most of the difference from the ordinary baseline is already
present in the no-map, translation-capable path. Precisely separating shader
variant/branch/resource-binding effects from clock and scheduling variation
needs a more controlled experiment. The earlier claim that shader variants
alone cost nothing is not reproduced by these controls.

The old noisy map-only controls reported 19.25/19.29 ms, so the new 19.28/19.47
ms results show no GPU-time reduction attributable to suppressing the warnings.
CPU preparation falls from about 1.167 ms in those two old runs to **0.466 ms**
here: roughly **0.70 ms / 60% less**. The old controls produced 621,038 and
639,070 spurious warnings; both corrected controls produce zero. This is a
cross-run observation, not a new matched old/new binary A/B.

## Validation and evidence

The existing warp-read suite was rerun against the exact corrected EXD prefix:
mapped reads and the unmapped identity control pass, and the untranslated
negative control fails as expected (worst peripheral error 2048 px). The
ordinary audit-off/on fixture checks 8,192 exact pixels in each case with Metal
validation enabled and passes. Shader cache and rendering policy were unchanged
by this warning fix. Game Metal validation remains off, matching earlier EXD
performance runs; these are not new headset visual acceptance results.

Two initial attempts produced no usable audit records and are excluded. The
first also reported an inactive built-in display. The successful sequence
kept the display awake, aligned private prefix DLLs, and waited for each private
service to finish exiting between launches. One flags-only launch initially
stopped at the existing service-running guard before the sequence resumed;
only the successful repeat is included. These exclusions are not GPU outliers.

[Curated results and validation](results/dxmt-warning-fix-20261111-run3/summary.json),
[comparison](results/dxmt-warning-fix-20261111-run3/comparison.json),
[provenance](results/dxmt-warning-fix-20261111-run3/manifest.json), and saved
`runner.py` / `runner-resume.py` contain the exact configurations. Raw game logs
remain in the private run paths recorded by each result. Fixture logs are under
`build-in-process/warning-fix-read-validation-matched` and
`build-in-process/warning-fix-audit-validation-matched`.

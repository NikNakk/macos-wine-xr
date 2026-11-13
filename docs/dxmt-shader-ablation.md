# Dissecting inactive rate-map shader cost

2026-11-11. Follow-up to the [warning fix and retest](dxmt-warning-fix-retest.md).

The subsequent [selective-read prototype](dxmt-selective-read.md) recovers
about 1.90 ms in the no-map control, but does not establish a mapped EXD gain.
It remains opt-in.

**The texture-read translation shader component accounts for most of the
reproducible no-map penalty.** With a fresh shader cache, ordinary shaders plus
the rate-map host setup average 16.95 ms, read-capable shaders average 19.06 ms,
and both translation components average 19.17 ms. No rate map is bound anywhere
in these trials. This narrows the problem to translated shader variants rather
than establishing an intrinsic rasterization-rate-map hardware cost.

## The coupling and the diagnostic

`getGlobalShaderFlag()` in `d3d11_shader.cpp` enables **both**
`SM50_SHADER_FLAG_RATE_MAP_POSITION` and `SM50_SHADER_FLAG_RATE_MAP_READ` whenever
`DXMT_RATE_MAP_POSITION=1`. `DXMT_RATE_MAP_READ` separately controls runtime
layout tracking. Switching off that runtime flag does not remove the read
shader code.

[Patch 0018](../patches/dxmt/0018-no-map-shader-component-diagnostic.patch)
adds `DXMT_RATE_MAP_DIAGNOSTIC_SHADER_MASK=0|1|2|3`, accepted only when the
position switch is on and `DXMT_STAY_WARPED=0x0`. It selects neither component,
position only, reads only, or both. Invalid masks or a nonzero stay-warped
profile log an error and retain the normal mask 3. No shader translator code,
layout policy, pass merging or buffer-binding code is changed by this patch.
Without the diagnostic, the original both-components behavior is preserved.

All non-baseline cases keep exactly the same host setup: position/read switches
and the attachment diagnostic enabled, a 0x0 no-map profile, and the existing
buffer bindings/layout checks. Shader selection alone differs. Host-only uses
the same ordinary shader flag bits, cache keys and generated functions as the
baseline, allowing host setup to be assessed as a group.

The selected existing compiler bits already participate in every variant's
cache digest. All trials use the new shared private directory
`build-in-process/exd-warp/shader-ablation-cache-20261111`, ensuring they do not
reuse shader variants from earlier experimental translator implementations.
The same directory is retained for the reverse-order repeats. The combined
fresh-cache runs reproduce the prior ~19.2 ms observation, so old-cache contents
are not required to reproduce the large no-map penalty.

## Measurements

Apple M5, simulated desktop HMD, private EXD service/runtime/prefix, 2560×1440,
4× MSAA main scene, upscaler off. Ten 60-second runs, in forward/reverse order.
First/cohort selection is the same as the previous investigation:
`300 <= frame < 4000`, then `render_after >= 55`. GPU times are median per-frame
unions of completed DXMT command-buffer intervals and include internal waits.
They are not isolated shader-active times.

| Case | First / reverse GPU run medians (ms) | Mean of run medians (ms) |
| --- | --- | ---: |
| Ordinary baseline | 15.927 / 17.307 | 16.617 |
| Host setup, ordinary shaders (mask 0) | 16.221 / 17.679 | 16.950 |
| Position component only (mask 1) | 17.212 / 17.580 | 17.396 |
| Texture-read component only (mask 2) | 18.988 / 19.129 | 19.058 |
| Both components (mask 3) | 19.110 / 19.240 | 19.175 |

Across 22,280 loaded frames:

- **Read component versus identical host setup: +2.108 ms** in the mean of run
  medians. Read-only's 18.988–19.129 ms range does not overlap host-only's
  16.221–17.679 ms range. It is the dominant reproducible separation.
- Both versus host: +2.225 ms. Adding position to read-capable shaders changes
  the mean by only +0.116 ms, within observed variation.
- Position-only versus host: +0.446 ms. These run ranges overlap, so this is
  not an isolated causal position cost. The earlier forward-only ~1 ms gap
  was not reproduced against the reverse host control.
- Host setup versus baseline: +0.333 ms. The control ranges overlap and drift
  by ~1.4 ms. No reliable standalone binding/bookkeeping cost is established.
- These differences are not additive. Shader scheduling, overlap and compiler
  behavior can change between variants; summing component deltas is invalid.

Every selected frame has zero mapped render nodes (maximum, not just median).
All ten runs have zero spurious layout warnings and no logged GPU command
errors. Per-run medians remain 59 render nodes after optimisation, seven
command buffers and 409 counted fence waits. Thermal samples are nominal
(state 0), Low Power Mode is false. CPU preparation stays around 0.43–0.48 ms
across cases, rather than rising with the expensive read variants. The private
save and both INI files are hash-restored after each run and after validation.
GPU clocks and fence-stall durations were not measured.

## What this does and does not isolate

The read translator adds a metadata test and control-flow join around each
supported texture-read operation. Even when an ordinary texture makes the
runtime branch skip coordinate lookup, the compiled shader still contains the
translation branch, its buffer parameter and its lookup implementation. In
these no-map runs the texture layout bits stay ordinary, so actual rate-map
lookup is not the intended active path.

The test therefore identifies the **presence of the read-translation shader
component**, including its signature and generated control flow, as the
expensive class. It does not distinguish metadata loads/branches from compiler
scheduling, register pressure, occupancy or other code-generation effects.
Those remain hypotheses until a compile-time-specialized or profiled comparison
isolates them. Read-only includes pixel and compute shaders; their individual
contributions have not been timed separately.

The clearest optimization target is to keep ordinary shader variants for passes
that neither rasterize through a map nor read mapped resources, and select
translation-capable variants only where their functionality is needed. That
requires correct per-pass/per-resource eligibility and cache/PSO keys, especially
for mixed ordinary/mapped consumers. No such production optimization is
implemented by this diagnostic, and no EXD/headset speedup is claimed.

## Validation and provenance

All four masks pass the existing no-map pixel fixture at 2048² with Metal
validation enabled. After profiling, the exact deployed prefix passes the
normal mapped chain, unmapped identity control and untranslated negative
control. A nonzero stay-warped profile rejects the diagnostic mask and retains
both shader bits. Patch 0018 passes `git apply --reverse --check` on the private
source.

Rebuilt only `d3d11.dll`, deploying it to both the private Wine runtime and the
private EXD prefix. The preceding warning-fixed DLLs and source are backed up
under `build-in-process/exd-warp/shader-ablation-backup-20261111/`. The new
selector is off by default; no installed runtime, LaunchAgent or public feature
behavior changed. No physical-headset test was performed.

[Per-run results](results/dxmt-shader-ablation-20261111/summary.json),
[comparison](results/dxmt-shader-ablation-20261111/comparison.json),
[manifest and hashes](results/dxmt-shader-ablation-20261111/manifest.json),
[validation](results/dxmt-shader-ablation-20261111/validation.json), and the saved
runner preserve the exact cases. Raw game logs and the fresh shader cache remain
under the private experiment root. Fixture logs are copied into the result's
`validation/` directory.

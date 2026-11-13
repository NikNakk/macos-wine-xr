# Per-resource read specialisation

2026-11-11. Follow-up to [selective read shaders](dxmt-selective-read.md).
Codex implemented and validated the patch; this note adds the EXD measurements
and image checks.

**Compiling each pixel shader for the exact texture slots that are mapped at a
draw roughly halves the cost of the stay-warped mechanism in EXD.** At rate 1.0,
where nothing is saved, GPU time falls from 19.9 to 18.1 ms against a 16.5 ms
baseline, so the overhead drops from about 3.4 ms (21%) to about 1.6 ms (10%).
With the feature flags on but no map bound, the cost falls from 1.7 ms to
0.2 ms. The rate-0.5 runs sit at the simulator's 60 Hz cap and show no change.
Simulated headset, one scene; not run on the PS VR2.

## What it does

[Patch 0020](../patches/dxmt/0020-resource-read-shader-variants.patch), on top
of 0019, is opt-in with `DXMT_RATE_MAP_RESOURCE_READ=1` (and
`DXMT_RATE_MAP_POSITION=1`).

- After a draw's bindings are encoded, DXMT knows which pixel-shader texture
  slots hold mapped textures. It records that as a 128-bit slot mask.
- The pipeline is recompiled for that mask: a mapped slot translates
  unconditionally, an ordinary slot has no translation code at all. The runtime
  test of texture metadata, which the [ablation](dxmt-shader-ablation.md) found
  to be the dominant cost, disappears from both.
- The mask is part of the shader variant key, the shader cache digest and the
  pipeline cache key, so variants cannot alias.
- A draw falls back to the full dynamic pipeline if the variant cannot be
  built, if a pipeline already has 32 masks, or for stream-output, geometry,
  tessellation and compute pipelines. Slots in unknown binding ranges keep the
  dynamic test.
- It supersedes patch 0019's per-pass selection when both are enabled.

Each specialised draw gets its own set-pipeline command. Building a new variant
blocks the encoding thread once per mask; the measurements below exclude
warm-up.

## Correctness

On the rebuilt private runtime and prefix, with Metal validation:

- **Pixel fixture matrix**, option off and on: ordinary, mapped, a mapped
  source bound late, bound in reverse order, mixed mapped and ordinary sources
  in one draw, swapped between draws, and a pass that merges. All pass, with
  no fallbacks; the untranslated negative control still fails.
  `scripts/test-dxmt-resource-read.py`.
- **MSAA/HDR/stencil chain**, six cases (2×/4× MSAA, rates 1, 0.5, 0.75):
  centre exact, full rate exact, 402 slots kept and 1,151 pruned per run, no
  fallbacks. `scripts/test-dxmt-resource-read-msaa.py`.
- **Automatic-foveation suite**: all ten cases pass.
- **EXD image**, 1792×1008, 2× MSAA: at rate 0.5 the specialised image differs
  from the unspecialised warped one by a mean of 0.31 (8-bit), below the 0.83
  between two ordinary baselines. At rate 1.0 it matches the baseline outside
  the animated light shaft (0.71).

## EXD measurements

2560×1440, 4× MSAA, loaded-level frames, fourteen 60-second trials in
off/on/on/off order with a baseline at each end, a private shader cache, and
Codex's trial harness (thermal state nominal throughout, settings
hash-restored, zero layout warnings).

| Case | Specialisation off | Specialisation on |
|---|---:|---:|
| Ordinary baseline (no feature flags) | 16.19, 16.77 ms | — |
| Flags on, no map bound | 17.98, 18.44 ms | 16.57, 16.71 ms |
| Mapped, rate 1.0 | 19.59, 20.14 ms | 17.93, 18.22 ms |
| Mapped, rate 0.5 (at the 60 Hz cap) | 14.22, 14.20 ms | 14.09, 14.10 ms |

- **No map:** 18.21 → 16.64 ms. The remaining 0.16 ms over the baseline is
  inside the baseline pair's own spread.
- **Rate 1.0:** 19.87 → 18.07 ms, a 1.8 ms (9%) reduction with separated
  ranges. What remains over the baseline is about 1.6 ms: position
  translation, the translations that are genuinely needed, and any cost of the
  map itself, not separated here.
- **Rate 0.5:** frame period is pinned at 16.66 ms in all four runs, so the
  GPU-interval figure is not a throughput measure and the 0.1 ms difference
  means nothing either way.
- Per loaded frame with the option on and a map bound: 118 specialised draws,
  196 slots kept as mapped, 632 pruned, 60 draws mixing both, no fallbacks.
  CPU preparation time is unchanged at about 0.45–0.48 ms.

These figures use a different session and cache from the
[first trial](dxmt-exd-stay-warped-trial.md), whose rate-1.0 runs were about
21 ms; compare within this table.

## Limits

- The benefit at reduced rates could not be measured on the simulator. It
  should be at least the rate-1.0 saving scaled by the pixels still shaded,
  but that is an expectation, not a result.
- Compute, geometry and tessellation pipelines are not specialised.
- Variant count is bounded at 32 masks per pipeline; a game that cycles many
  binding patterns would fall back to the dynamic shader.
- First use of each mask compiles on the encoding thread, so new scenes will
  hitch until the cache is warm.
- One scene, a still view, simulated headset.

## Reproduce

```sh
export MWXR_IN_PROCESS_WINE="$PWD/build-in-process/exd-warp/wine"
scripts/test-dxmt-resource-read.py
scripts/test-dxmt-resource-read-msaa.py
python3 docs/results/dxmt-resource-read-20261111/exd/runner.py   # fourteen EXD trials, about 20 minutes
```

For a single run add `DXMT_RATE_MAP_RESOURCE_READ=1` to the stay-warped command
in [the trial note](dxmt-exd-stay-warped-trial.md).
[Trial data](results/dxmt-resource-read-20261111/exd/summary.json),
[image check](results/dxmt-resource-read-20261111/exd/image-check.json),
[fixture results](results/dxmt-resource-read-20261111/validation/summary.json).

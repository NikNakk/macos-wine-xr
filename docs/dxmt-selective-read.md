# Selective read shaders for ordinary passes

2026-11-11. Follow-up to [the shader-component ablation](dxmt-shader-ablation.md).

**The opt-in optimization recovers about 1.90 ms (10%) in EXD's flags-enabled,
no-map control. It does not establish a performance gain with actual mapped
rendering:** rate 0.5 and all-1.0 mapped comparisons are unchanged within run
variation. Keep it opt-in; the remaining mapped-frame cost needs finer-grained
work inside passes that mix ordinary and mapped texture reads.

## Implementation

[Patch 0019](../patches/dxmt/0019-selective-read-shader-variants.patch), on top of
0018, enables `DXMT_RATE_MAP_SELECTIVE_READ=1` for ordinary graphics pipelines
when the existing position feature is enabled. It is off by default and does
not combine with the earlier shader-component diagnostic override.

The ordinary graphics path prepares the original pipeline and an alternative
without the read-translation compiler bit. It retains the position compiler
bit, so rendering through a non-identity rate map still receives logical
`SV_Position`. The read choice participates in both the graphics PSO's hash and
equality and the in-memory pixel-shader variant key. The actual compiler flags
also determine the disk shader digest and the flags supplied to compilation.
The alternative cannot alias a full read-capable shader under the same cache
key.

Selection happens on the encoding side **after all resource bindings are
recorded and render passes have been merged**. A pass that has any mapped read
keeps every original read-capable pipeline. A pass with no mapped read switches
eligible pipeline commands to the alternative. This intentionally errs toward
keeping translation; a later SRV binding in the same pass cannot unexpectedly
require code that was removed earlier.

Pipeline pairs survive pass merging. Selection scans actual set-pipeline
commands, covering the pipeline restoration commands inserted by tile barriers
as well as normal binds. Pipeline-cache ownership retains the handles for the
existing command lifetime. If alternative compilation fails, the original
pipeline remains. Geometry/tessellation paths and compute retain their original
pipelines; they are not optimized by this patch. No attachment actions, map
bindings, resource tags, synchronization or OpenXR protocol is changed.

The new `read_ordinary` / `read_capable` audit counters count eligible pipeline
commands at native encoding. They verify selection without inferring it from
an environment variable. The alternative shaders/PSOs add startup compilation
and cache entries; warmup is excluded from steady-state measurements.

## Correctness checks

The exact rebuilt private EXD prefix passes:

- Existing mapped-read chain, ordinary identity and untranslated negative
  controls with selection enabled.
- The new [selection regression](../scripts/test-dxmt-selective-read.zsh) with
  selection off and on. The source fixture gains `--late-read`, which first
  binds an ordinary source, then a mapped source without changing the pixel
  shader or target; its final pixels remain correct. Its negative read-disabled
  control still fails.
- A `--unfoveated --late-read --split-pass` case that creates six render nodes
  and merges them to five. All six eligible pipeline commands choose ordinary
  read shaders, confirming that the selection records survive the merge.
- The MSAA/HDR/depth/stencil chain at 2×/4× and rates 1.0, 0.75 and 0.5. All
  reduced-rate centre checks pass and both full-rate outputs have zero flagged
  final pixels. Counters show ordinary and full read shaders in each case.
- The existing automatic-foveation suite: ten selected/fallback/default cases,
  4,194,304 exact checked pixels per case. A temporary runner wrapper selects
  the exact private EXD prefix; the original installed/test prefixes are not
  modified.

Fixture Metal validation is enabled. The game comparisons use the earlier
validation-off configuration; they do not add headset visual acceptance.
Patch 0019 passes `git apply --reverse --check` on the private source.

## Measurements

Apple M5, simulated desktop HMD, private EXD service/runtime/prefix,
2560×1440 double-wide, 4× MSAA main scene, upscaler off. Each case is a matched
60-second off/on/on/off sequence with one binary and a fresh shared private
shader cache. The no-map case enables both shader features with the 0x0
profile. The two mapped cases use a real 2560×1440 map at rate 0.5 or 1.0;
coordinate translation remains enabled in both. **The full-rate case is the
complete foveation mechanism, not the earlier debug-bit-3 map-only test.**

Loaded frames use `300 <= frame < 4000`, then `render_after >= 55`. Figures are
means of two run medians of the per-frame union of DXMT command-buffer GPU
intervals, including waits, and are not isolated shader-active times.

| Workload | Selection off (ms) | Selection on (ms) | Saved |
| --- | ---: | ---: | ---: |
| Flags enabled, no map | 18.927 | 17.027 | 1.901 ms / 10.0% |
| Actual mapped rate 0.5 | 12.729 | 12.782 | No demonstrated gain |
| Actual mapped rate 1.0 | 20.408 | 20.430 | No demonstrated gain |

Individual run medians:

| Workload | Off first / last (ms) | On first / second (ms) |
| --- | --- | --- |
| No map | 18.824 / 19.030 | 16.926 / 17.127 |
| Rate 0.5 | 12.937 / 12.521 | 12.862 / 12.702 |
| Rate 1.0 | 20.139 / 20.677 | 20.335 / 20.525 |

No-map eligible pipeline-command medians are 93 ordinary / 0 full when enabled.
Actual mapped runs select 41 ordinary / 52 full commands. The latter retain 19
mapped render nodes. All twelve runs have medians of 59 render nodes, seven
command buffers and 409 counted fence waits per loaded frame. Across 26,355
loaded frames, no spurious layout warnings or GPU command errors are logged.
Thermal state stays nominal (0), Low Power Mode false. Private game settings
are hash-restored after every run and again checked after fixtures.

Rate 0.5 reaches the simulator's ~60 Hz cap in both cases. The full-rate mapped
case stays GPU-bound, with source-frame periods around 20.4–20.9 ms off and
20.6–20.8 ms on; it also shows no benefit beyond variation. The no-map source
periods improve from about 19.1–19.3 ms to 17.4–17.5 ms. These are simulated
source-frame observations, not PS VR2 physical cadence or headset FPS.

## Conclusion and next scope

The mechanism correctly avoids read-translation shaders when an entire pass
has no mapped reads and removes much of the idle-feature penalty. It is useful
as a bounded candidate, but should not be presented as an EXD foveation
speedup: mapped-frame timings do not demonstrate one.

Most of the costly mapped work still uses full variants. One mapped texture
keeps translation code for every supported texture read in that pass, including
ordinary textures. A next experiment should specialize eligible shader reads
per resource (with correct rebinding, pass/PSO/cache keys and mixed-layout
handling), or directly reduce the active lookup/code-generation cost. This
patch deliberately does neither. Per-pass counters do not establish which
individual texture reads dominate; a per-resource audit is needed before that
change. Register pressure and occupancy remain unmeasured.

## Reproduce and provenance

The rebuilt `d3d11.dll` / `dxgi.dll` are in both the private Wine runtime and
private EXD prefix. Their previous binaries and all changed DXMT source files
are backed up under `build-in-process/exd-warp/selective-read-backup-20261111/`.
Installed runtimes, the installed LaunchAgent, Steam and installed game copies
are unchanged. The new option is off by default; no hardware run was made.

```sh
# Existing prepared private EXD experiment; selection remains explicitly opt-in.
MWXR_EXD_EXPERIMENT_ROOT="$PWD/build-in-process/exd-warp" \
MWXR_EXD_SERVICE_ENV="SIMULATED_DESKTOP=1 XRT_MACOS_REFRESH_RATE_HZ=0" \
MWXR_VIEWPORT_SCALE_PERCENTAGE=143 \
MWXR_EXD_STAY_WARPED=2560x1440 MWXR_EXD_STAY_WARPED_RATE=0.5 \
DXMT_RATE_MAP_SELECTIVE_READ=1 scripts/run-exd-headset-compare.zsh msaa4 60

# Rebuild the warp-read probe first using its existing build script.
MWXR_IN_PROCESS_WINE="$PWD/build-in-process/exd-warp/wine" \
MWXR_WARP_READ_PREFIX="$PWD/build-in-process/exd-warp/prefix" \
zsh scripts/test-dxmt-selective-read.zsh
```

[Results](results/dxmt-selective-read-20261111/summary.json),
[comparison](results/dxmt-selective-read-20261111/comparison.json),
[manifest](results/dxmt-selective-read-20261111/manifest.json),
[validation](results/dxmt-selective-read-20261111/validation.json), saved runner,
and the regression directories preserve the cases and provenance. The manifest
lists the original eight cases and the four additional identity-map cases.
Raw game logs and the cache remain under the private experiment root. The
all-1.0 mapped control was added after rate 0.5 proved capped, before drawing a
performance conclusion.

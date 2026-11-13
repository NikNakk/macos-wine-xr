# EXD rendered warped: first trial on the simulated headset

2026-11-11. Follow-up to the [layout audit](dxmt-exd-layout-audit.md).

**Later correction:** the [warning-fix retest](dxmt-warning-fix-retest.md) removes
the map-only diagnostic warning flood. Its repeated flags-only/no-map controls
are almost as expensive as map-only, so the earlier attribution below of the
entire residual to unavoidable Metal map cost is superseded. The old numbers
are retained as historical evidence.

**EXD's loaded level renders correctly with every scene-sized pass rasterised
through a fixed two-peak rate map.** The unmodified game ran for 90 seconds per
configuration without a crash or Metal error. At peripheral rate 0.5 the
centre of each eye matches the ordinary render to within run-to-run noise and
the periphery is visibly coarser, as intended. In the one GPU-bound
configuration available on the simulator, GPU time per frame fell from 17.0 to
12.9 ms (24%), and the run reached the 60 Hz cap.

The mechanism itself is expensive: at rate 1.0, where nothing is saved, the
same run took 22.0 ms, 29% more than the baseline, and about 21.0 ms (25%)
after two translation optimisations. This is a simulated
headset with a still view of one scene, fixed foveation only. It was not run on
the PS VR2, and nobody has looked at it in a headset.

## What was added

[Patch 0017](../patches/dxmt/0017-automatic-stay-warped-stereo-profile.patch),
on top of 0015 and 0016:

- **Automatic profile.** `DXMT_STAY_WARPED=WIDTHxHEIGHT` binds one fixed map
  for the session. Only render passes of exactly that size use it; there is no
  shader or format heuristic. `DXMT_STAY_WARPED_RATE` is the peripheral rate
  per axis.
- **Two-peak map.** Sixteen columns, eight per eye, with the middle two of
  each eye at full rate; eight rows with the middle two at full rate. The
  foveal centres are the middle of each half, not the lens centres.
- **Multi-target passes** are mapped and all their targets tagged.
- **Compute-stage read translation**, using the same branch as pixel shaders.
- **Swapchain passes stay ordinary.** A pass whose target is an imported native
  texture (the OpenXR swapchain image) is not mapped, so its reads of mapped
  textures restore ordinary layout. EXD's last pass uses `Load`, so this
  restore is nearest-neighbour.
- **Image dump.** `DXMT_DEBUG_DUMP_PATH` writes the last rendered swapchain
  image at a chosen Present, because this session cannot capture the screen.

`scripts/run-exd-headset-compare.zsh` gained `MWXR_EXD_STAY_WARPED` and
`MWXR_EXD_STAY_WARPED_RATE`. Without them it behaves as before.

## Correctness

1792×1008, 2× MSAA, image dumped at Present 1400. Differences are the largest
channel difference from the baseline dump, in 8-bit units.

| Run | Mean difference | Centre of each eye | Periphery | Pixels over 16 |
|---|---:|---:|---:|---:|
| Second baseline (noise floor) | 0.83 | 0.95 | 0.82 | 0.07% |
| Warped, rate 1.0 | 0.56 | 0.54 | 0.56 | 0.03% |
| Warped, rate 0.5 | 2.32 | 1.27 | 2.39 | 1.77% |

- At rate 1.0 every mapped pass, translated read and compute read is active
  with an identity map, and the image is inside the noise floor.
- At rate 0.5 the differences are along edges in the periphery, where the
  image is made of 2×2 blocks. Fog, the light shaft, the logo and the ground
  decal are in the right places in both eyes.
- An audit of frame 1390 at rate 0.5 shows 22 of 24 scene-sized passes mapped,
  the two swapchain passes ordinary, no other-sized pass mapped, and all 167
  reads of mapped textures translated, including both compute reads.
- The first rate-0.5 dump differed from the baseline in a vertical light
  shaft. A repeat run did not, and a fixture test confirmed scissor rectangles
  stay in logical coordinates under a rate map. The shaft animates; scene time
  is not aligned between runs, so single-frame comparisons carry that noise.

The dumps are game images and stay under
`build-in-process/exd-warp/dumps` (`baseline.png`, `warped-r050-b.png` and
crops).

## GPU time

The simulator caps at 60 Hz and the M5 reaches that at 2× MSAA, so those runs
are not GPU-bound. With 4× MSAA at the simulator's largest view size
(2560×1440 double-wide) the baseline falls below the cap. Loaded-level frames
only (at least 55 render passes), about 3,170 frames per run, one run each:

| 2560×1440, 4× MSAA | GPU per frame (median, p10–p90) | Frame period | Against baseline |
|---|---:|---:|---:|
| Baseline | 17.0 ms (15.5–18.1) | 17.6 ms | — |
| Warped, rate 0.5 | 12.9 ms (12.1–14.8) | 16.7 ms (cap) | **24% less** |
| Warped, rate 0.75 | 15.6 ms (14.4–17.0) | 16.9 ms | 8% less |
| Warped, rate 1.0 | 22.0 ms (19.9–22.8) | 22.2 ms | 29% more |

GPU time is the union of DXMT command-buffer intervals per frame, as in the
earlier audits. It includes waits, and the rate-0.5 run is at the cap, so its
saving is a lower bound for this scene. At the cap with 2× MSAA the same
measure moved from 14.3 to 12.0 ms at 1792×1008; that pair is not a throughput
comparison.

### The overhead

At rate 1.0 nothing is saved, so the run measures what the mechanism costs.
`DXMT_STAY_WARPED_DEBUG` switches the two translations off individually (the
image stays right at rate 1.0 because the map is an identity). Same
configuration, one 90-second run each, about 3,170 loaded frames:

| Rate 1.0, 2560×1440, 4× MSAA | GPU per frame | Added over baseline |
|---|---:|---:|
| Baseline (repeat) | 17.06 ms | — |
| Translating shader variants, no map | 16.70 ms | none |
| Map bound, no translation | 19.25 ms | 2.2 ms |
| Map and `SV_Position` translation | 19.75 ms | 2.7 ms |
| Map and read translation | 21.57 ms | 4.5 ms |
| Everything | 22.27 ms | 5.2 ms |

- **The rate map itself costs about 2.2 ms (13%)** with no translation at all.
  This was the initial interpretation; the later [warning-fix controls](dxmt-warning-fix-retest.md)
  do not isolate that increase as Metal hardware cost.
- **Read translation costs about 2.3–2.5 ms.** It is four table reads and two
  interpolations for each of the 167 reads, several of them inside fullscreen
  shaders with many taps.
- **`SV_Position` translation costs about 0.5–0.7 ms.**
- **The shader variants alone cost nothing** when no map is bound, so ordinary
  passes are unaffected. The two baselines differ by 0.05 ms.

The two translations together are a little over half the overhead and are the
part that can be optimised. Candidates: a direct integer table for `Load`
(one read per axis, no interpolation), and skipping the round trip when a
mapped pass loads a same-map texture at its own pixel, which is the common
post-processing pattern. Neither was attempted.

### Translation optimisations

Two changes followed the breakdown:

- **Integer table for `Load`.** The map's buffer gained the physical texel of
  every logical pixel centre per axis, so an integer load is one table read
  per axis with no interpolation.
- **No round trip for a same-pixel load.** The translator tracks, within a
  basic block, a temporary that holds `int(SV_Position.xy)`. When such a
  temporary is the address of a `Load` and the texture shares the mapped pass's
  layout (a second metadata bit set at bind time), the untranslated position is
  used directly. In the audited frame 160 of the 167 mapped reads share the
  pass's layout; how many use this address pattern was not counted.

Fixture, before → after: 2× MSAA 41.6% → 43.6% at rate 0.5, 17.1% → 19.4% at
0.75, and the full-rate control from 11.4% slower to 7.4% slower; 4× MSAA
41.9% → 44.8%, 16.1% → 20.4%, and 12.7% → 6.8% slower. Image errors are
unchanged. All fixture suites pass.

EXD, same 2560×1440 4× MSAA configuration:

| Run | GPU per frame |
|---|---:|
| Baseline, three runs | 17.01, 17.06, 16.41 ms |
| Rate 1.0 before, two runs | 21.98, 22.27 ms |
| Rate 1.0 after, five runs | 17.99, 21.23, 20.62, 20.97, 21.19 ms |
| Map only, two runs | 19.25, 19.29 ms |
| Rate 0.75 before → after | 15.58 → 14.40 ms (now at the cap) |
| Rate 0.5 after | 12.81 ms (at the cap, as before) |

Four of the five optimised full-rate runs agree at about 21.0 ms, roughly
1.1 ms below the unoptimised runs. The first run, 17.99 ms, is an unexplained
outlier and is not used. So the full-rate overhead is now about 4.2 ms (25%)
over the baseline instead of 5.2 ms, of which about 2.4 ms is the map itself.
At 1792×1008 the optimised rate-1.0 image is within the noise floor of the
baseline (mean difference 0.85), and the rate-0.5 image differs from the
earlier warped one only by the animated light shaft and edge noise.

**Single runs are less reliable than the earlier tables suggest.** The outlier
shows one run can be off by 3 ms, and GPU time drifts upward by about 1 ms
within a run. The rate-0.5 and rate-0.75 runs sit at the 60 Hz cap, so this
simulator cannot show how much the optimisations add there.

### Another scene

Not achieved. The simulated headset has no controllers, so a save cannot be
loaded by playing. A map name on the command line was ignored (the title scene
rendered, as expected of a shipping build), and setting `GameDefaultMap` to the
save's level name in the private `Engine.ini` produced no frames; the file was
restored. A different scene needs someone in the headset with controllers.

## Limits

- One scene, a still view, about 90 seconds per run, on a simulated headset.
- Fixed foveation centred on each half of the target. The PS VR2's lens
  centres and gaze are not used.
- The periphery is restored with nearest sampling. On the headset the
  compositor could reconstruct with filtering instead.
- The 4× MSAA dumps are not time-aligned with their baseline (the level fades
  in and the runs have different frame rates), so image correctness rests on
  the 2× MSAA runs.
- Game Metal validation was off, as in every earlier EXD run.
- Menus, loading screens, other levels, DLSS mode and the quad-view path were
  not exercised. Anything that copies a scene target, reads one from a vertex
  shader or uses an untranslated sample form would be wrong, and the audit only
  rules those out for this scene.

## Next step

1. Cut the translation cost: an integer table for `Load`, and no round trip
   for a same-map load at the pass's own pixel.
2. A PS VR2 run at its native 5600×2856, where the baseline is 71 ms and the
   GPU is the limit throughout. This needs someone wearing the headset to
   judge the periphery.
3. Map centres from the headset's lens calibration, then gaze.

## Reproduce

```sh
# Private clone and runtime as in docs/dxmt-exd-layout-audit.md, with patches 0001-0017.
common=(MWXR_EXD_EXPERIMENT_ROOT="$PWD/build-in-process/exd-warp"
        "MWXR_EXD_SERVICE_ENV=SIMULATED_DESKTOP=1 XRT_MACOS_REFRESH_RATE_HZ=0")
env "${common[@]}" MWXR_EXD_TAG=baseline scripts/run-exd-headset-compare.zsh msaa2 90
env "${common[@]}" MWXR_EXD_TAG=warped MWXR_EXD_STAY_WARPED=1792x1008 MWXR_EXD_STAY_WARPED_RATE=0.5 \
  DXMT_DEBUG_DUMP_PATH="Z:$PWD/build-in-process/exd-warp/dumps/warped.ppm" DXMT_DEBUG_DUMP_FRAME=1400 \
  scripts/run-exd-headset-compare.zsh msaa2 90
# GPU-bound pair: add MWXR_VIEWPORT_SCALE_PERCENTAGE=143, use msaa4 and MWXR_EXD_STAY_WARPED=2560x1440.
python3 tools/analyze_dxmt_tbdr_audit.py /path/to/run/game/game.log --min-frame 300 --max-frame 4000 --frames-csv frames.csv
```

The target size must match the game's scene target exactly; read it from a
`DXMT_LAYOUT_AUDIT` run. [Numbers](results/dxmt-exd-stay-warped-20261111/summary.json).
The fixture suites (read translation, stay-warped MSAA chain, automatic
foveation, explicit-map probes) pass on the final build. Each EXD run restored
the private save and settings and stopped its private service.

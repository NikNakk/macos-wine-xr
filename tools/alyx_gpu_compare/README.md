# Matched Alyx GPU comparison

Measures how much GPU time Half-Life: Alyx needs per frame on each graphics
path, under the same conditions, so DXMT's cost can be separated from the
Mac's limits. See section 1 of
[the performance write-up](../../docs/windows-vr-performance-opportunities.md).

| Variant | Path |
| --- | --- |
| `wine-dxmt` | Windows Alyx, D3D11 → DXMT → Metal (CrossOver rig, xrizer, in-process wineopenxr) |
| `wine-vulkan` | The same with `-vulkan`: Vulkan → winevulkan → MoltenVK. A check only. **Not runnable yet**: wineopenxr offers only D3D11 (Metal-backed) sessions, so it would first need `XR_KHR_vulkan_enable` |
| `native-mvk` | Linux ARM64 Alyx (Klepton, native xrizer) → MoltenVK |

## What is held equal

- **One Monado build** for every run: `alyx-native/build/monado-upstream-clean-desktop`,
  started fresh per run by `kk-tests/with-matching-monado-headset.py` in
  offscreen mode, with its remote driver (`P_OVERRIDE_ACTIVE_CONFIG=remote`) at
  4000×2040 and 120 Hz (`XRT_COMPOSITOR_DEFAULT_FRAMERATE`; the offscreen
  target otherwise runs at 60 Hz). Both games are offered the same
  recommended render size. Windows Alyx reaches the service through an x86_64
  client built from the same tree
  (`build/monado-upstream-clean-desktop-x86_64`, client only, Rift drivers off),
  which reports the same version, so no version check is bypassed.
- **The same save** (`runs/crash-repro/save-backup-20261026`, `+load autosave`),
  copied into each game's save folder and checked by hash. Before the first
  change, the game's own save folder, `machine_convars_vr.vcfg`, any fidelity
  override and (Wine) `openvrpaths.vrpath` are copied to
  `<output>/original-save-<native|wine>/`, and they are put back after every
  run even if the run fails. If the runner itself is killed, they can be put
  back from there (`original-files.json` lists where each file belongs).
  Steam Cloud for Alyx only syncs `user_vr.vcfg`, `user_keys_vr.vcfg` and
  `voice_ban.dt`, not saves.
- **The same render size and MSAA.** Each game's own render size comes from
  the runtime's recommendation (`--viewport-percent`, 50% gives 1400×1428 per
  eye; `--viewport-percent-native`, 53%, because the native build renders a
  square from it, about 1440×1440). The runner writes a one-level
  `vr_fidelity_levels_override.csv` at scale 1 with `--msaa` (default 2) into
  each game and passes `+vr_msaa N`. Without `+vr_msaa` the Windows build
  ignores the table and renders at full size with 8× MSAA; with a table scale
  below 1 its submit bounds fail xrizer's check (`InvalidBounds`, no frames).
  The size each game renders at is read from its console ("Level 0: Scale =
  … Resolution = …") and reported with GPU ms per megapixel. The OpenXR
  swapchain size Monado sees is the recommendation, not the render size.
- **The same settings:** fidelity auto off, `+vr_symmetric_fov 0`, muted, and
  `+vr_fdm 0` on the native build (D3D11 has no fragment density map).
- **The same motion:** optional, `--replay session.jsonl` streams a recording
  from the Monado session recorder into each run; without it the head stays
  still.
- **Run order** alternates (A B B A, or A B C C B A ...), and each run waits at
  least `--cooldown` seconds and for the thermal state to return to nominal.

## What is measured

For `--capture` seconds after `--warmup`:

- An xctrace recording of all processes with only the GPU instrument
  (`--trace-template gpu`; a full `'Metal System Trace'` is far larger, 40 to
  110 MB a second with Alyx running). The game's GPU busy time is the union of
  its GPU intervals, so overlapping vertex, fragment and compute work counts
  once. Elapsed GPU time, not occupancy. After each run the trace is reduced to
  `gpu-intervals.csv.gz` (a few MB) and `trace-info.json`, and deleted unless
  `--keep-traces`. A capture is skipped below `--min-free-gb` (20) free.
  xctrace also leaves its raw kernel recording (`instruments*.ktrace`, 0.1 to
  4 GB) in the user temp folder (`getconf DARWIN_USER_TEMP_DIR`) and never
  removes it; the runner deletes the ones its own capture created, once
  nothing has them open.
- Monado's app pacing trace (`PSVR2_TIMING_TRACE=1`) for the same window:
  delivered frames per second, frame intervals, refreshes per frame and gaps
  over 50 ms.
- **GPU ms per frame** = the game's GPU busy ms per second ÷ its frames per
  second. This is the comparison number.

Results: `summary.md` and `results.json` in the output directory, plus each
run's folder (`run.json`, logs, traces). `--analyze-only DIR` recomputes them.

## Before running

- Native variant: macOS Steam running and signed in.
- Wine variants: the rig's Windows Steam running and signed in
  (`~/Windows/run-steam.zsh`). The runner launches `hlvr.exe` directly and
  stops it with `taskkill`, leaving Steam running.
- Nothing else using the GPU heavily; the machine cool.

The runner refuses to start when a prerequisite is missing; `--dry-run`
prints every run's command and environment.

## Run

```sh
python3 tools/alyx_gpu_compare/compare.py --dry-run
python3 tools/alyx_gpu_compare/compare.py --variants wine-dxmt,native-mvk --repetitions 2
python3 tools/alyx_gpu_compare/compare.py --variants wine-dxmt,native-mvk --repetitions 2 \
  --replay ~/Code/alyx-native/runs/<recording>-session/session.jsonl --replay-from 60
```

Each run takes about `--warmup` + `--capture` + shutdown, plus the cool-down.
Defaults: 120 s warm-up (150 s is safer for Wine, which loads the save more
slowly), 15 s capture, 120 s minimum cool-down.

## A single-frame Metal capture of the Windows build

Needs the DXMT frame boundary (DXMT `claude/xr-frame-boundary` and the
wineopenxr build in `build-in-process/gate-frame-boundary`); without it DXMT
never counts a frame in VR. The first run asks for macOS developer-tools
permission and the Wine processes wait until it is answered. About 5 GB per
frame at scale 0.5.

```sh
python3 tools/alyx_gpu_compare/compare.py --variants wine-dxmt --repetitions 1 --warmup 120 \
  --trace-template none --in-process-build build-in-process/gate-frame-boundary \
  --game-env MWXR_DXMT_FRAME_BOUNDARY=1 --game-env MTL_CAPTURE_ENABLED=1 \
  --game-env DXMT_CAPTURE_EXECUTABLE=hlvr --game-env DXMT_CAPTURE_FRAME=3000 \
  --until-file "$HOME/Windows/prefix/drive_c/Program Files (x86)/Steam/steamapps/common/Half-Life Alyx/game/bin/win64/hlvr_F.*.gputrace"
```

`DXMT_CAPTURE_EXECUTABLE` is the name without `.exe`. Move the capture out
of the game folder afterwards. Run the runner in the foreground, or Ctrl+C
will not reach it (a background shell makes Python ignore SIGINT); its
restore also runs when a wait times out.

## Steam for the Windows build

The Wine variants use the rig's Windows Steam. Answering the game's Steam
calls from the native macOS Steam instead, as the native build does through
Klepton's `kl_steambridge.c`, needs a bridge from the Windows ABI to
`steamclient.dylib` (which has an x86_64 slice). That is Proton's
`lsteamclient`. NotProton (2026-10-09, GPL-3.0, lsteamclient under the
Steamworks SDK licence) already has a macOS port of it for CrossOver 26.3, so
it is the starting point if Windows Steam's CPU and memory use become a
problem. Not needed for GPU measurements.

## Fidelity tables and adaptive fidelity

`--fidelity-table FILE` writes FILE as the game's
`vr_fidelity_levels_override.csv` instead of the one-level table. With
`--render-scale 0 --viewport-percent 100 --game-arg +vr_fidelity_level_auto
--game-arg 1` the game picks its own level from the table (add
`--game-arg +vr_fidelity_level_spew --game-arg 1` to log its GPU readings and
level changes in `console.log`). Adaptive fidelity honours the table's MSAA
without `+vr_msaa`. `scripts/alyx/fidelity-levels.csv` is the table
`scripts/run-alyx-xrizer.zsh` uses on the headset (2x MSAA, scale 0.50 to 0.72).

`wine-dxmt-pfv-always` runs the Windows build with
`DXMT_PIXEL_FORMAT_VIEW=always`, DXMT's behaviour before on-demand
`PixelFormatView`, for A/B runs.

## CPU and thread profiles

```sh
python3 tools/alyx_gpu_compare/compare.py --variants wine-dxmt,native-mvk --repetitions 1 --capture 10 --trace-template cpu
python3 tools/alyx_gpu_compare/compare.py --variants wine-dxmt,native-mvk --repetitions 1 --capture 8 --trace-template threads
```

Both attach xctrace to the game process only, and save `vmmap.txt.gz` from
the moment the capture starts.

- `cpu`: Time Profiler. After the run, `cpu_profile.py` writes
  `cpu-profile.txt` and `.json`: CPU ms per second by thread, and the
  binaries and functions its samples land in. For Wine runs it reads the raw
  `time-sample` table (exporting `time-profile` crashes xctrace on Wine
  stacks) and names addresses from the vmmap, including Wine-loaded PE
  modules such as `d3d11.dll`. About 20 MB for 10 s.
- `threads`: Thread Activity, kept as `threads.trace` (about 0.5 GB for 8 s).
  `thread_waits.py TRACE` reports each thread's running, runnable and blocked
  time, and splits its blocked time by the thread that woke it.

Recording every process instead breaks the export while iOS simulators are
booted.

## Tests

```sh
cd tools/alyx_gpu_compare && python3 -m unittest test_analysis test_cpu_profile
```

## Limits

- The two builds are not byte-identical games: different executables and
  xrizer builds, and the native build's renderer may differ in small ways.
  `wine-vulkan` would show how much of any gap is Source 2's D3D11 renderer
  rather than DXMT, once wineopenxr supports Vulkan sessions. Until then, a
  Metal System Trace with the Metal Application instrument
  (`--trace-template 'Metal System Trace' --keep-traces`) and
  `kk-tests/perf/gpu_passes2.py` show where DXMT's GPU time goes by encoder.
- The Windows build needs `+map startup` before `+load` (the runner adds
  it); otherwise it discards the save and stays in the start-up map. Each
  run records the map its server spawned, and the summary flags runs whose
  maps differ.
- Desktop offscreen, not the PS VR2: Monado's distortion and present costs
  differ from headset runs, but they are the same for all variants.
- One save position. Use `--replay` and several repetitions before drawing
  conclusions from small differences.

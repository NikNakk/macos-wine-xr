# DXMT TBDR audit, 9 November 2026

The separate TBDR workstream now has opt-in passive diagnostics and an ordinary EXD baseline. **No new render optimisation or EXD speedup is claimed.** The [depth-resolve follow-up](dxmt-native-depth-resolve.md) now identifies that shader and tests native Metal replacement semantics and GPU cost. Blanket pass merging or changing attachment load/store actions is not justified by these traces.

## Scope and reproducibility

Test host: Apple M5, 24 GB, macOS 27.0.1 (26A434). Runs used the private EXD game/prefix/Wine copies under `build-in-process/exd-ngx`, the existing service compositor, and a simulated headset capped at 60 Hz. The preserved pre-NGX settings select upscaler off and 2× MSAA. Base foveation, mapped-position translation, attachment experiments and NGX subrects were disabled. Telemetry confirms zero mapped render passes and zero temporal-upscale nodes in the analysed frames. NGX DLL availability is not evidence that upscaling ran.

The installed Steam/game/prefix/runtime were not changed. Each run restored the private settings file. Source is the existing private DXMT fork at `4c3a2f5ea661bd258c14cf8ccb6924791cc75a77`, with patches 0001–0009. [Patch 0009](../patches/dxmt/0009-passive-tbdr-audit.patch) adds diagnostics only. Applying the complete patch series to that pinned base reproduces all 307 source files byte for byte. [Source/runtime hashes](results/dxmt-tbdr-audit-20261109/source-runtime-sha256.json) identify the build used here.

Game Metal validation was disabled for EXD because the preceding investigation found an existing linear/sRGB blit assertion. Consequently these performance runs **do not establish EXD rendering correctness**. The separate ordinary D3D11 regression below uses Metal validation. Scene phases advance automatically after launch; the frame windows below are observed cohorts, not a deterministic replay or a controlled optimisation A/B benchmark.

## What the instrumentation measures

`DXMT_TBDR_AUDIT=1` records existing completed command-buffer start/end times from the normal completion thread, per-buffer encoder counts before/after the existing optimiser, CPU preparation/encoding time, selected fence/event activity and attachment properties. It inserts no timestamp queries, command encoders, flushes or waits. Detail mode adds existing encoder labels, attachment/allocation identities, dimensions, samples, formats, usage and load/store actions. Defaults remain off.

The Python analyser groups the primary DXMT queue by the existing Present frame ID and computes the **union** of overlapping command-buffer intervals. This avoids adding overlapping buffers twice. These intervals include internal waits: they are neither shader-active time nor complete application/compositor GPU frame time. The gap metric is elapsed time between those intervals, not a diagnosed stall. A timestamp node already present in EXD is counted; the audit does not create it.

Attachment byte counts are nominal, uncompressed load/store/resolve **proxies**, not observed memory transactions or DRAM bandwidth. Combined depth/stencil is counted once with an 8-byte nominal packed payload; actual Metal storage may differ. Fence counts are calls observed at instrumented DXMT sites, not stall durations or a complete native synchronisation inventory. Merge rejection counts are optimiser attempts, not removable pass counts.

## Results

Values are medians per selected frame. Detail and profiler runs are kept separate from the summary baseline.

| Observation | Initial phase: frames 120–599, 480 frames | Loaded phase: frames 900–1599, 700 frames |
|---|---:|---:|
| DXMT command-buffer interval union | 3.836 ms | 14.013 ms |
| Union p10–p90 | 3.473–4.100 ms | 12.065–14.473 ms |
| Interval envelope | 16.242 ms | 16.445 ms |
| Inter-buffer gaps | 12.428 ms | 2.384 ms |
| Present-frame start period | 16.676 ms | 16.677 ms |
| Render nodes before → after optimisation | 46 → 45 | 65 → 61 |
| Clear nodes before → after | 4 → 1 | 12 → 0 |
| Standalone resolve nodes before → after | 0 → 0 | 1 → 0 |
| Nominal attachment load proxy | 120.2 MiB | 334.1 MiB |
| Nominal attachment store proxy | 196.0 MiB | 460.6 MiB |
| DXMT CPU encoding work | 2.568 ms | 1.747 ms |

The loaded phase consumes most of the simulated 16.67 ms budget in completed DXMT intervals. The initial phase is substantially simpler and must not stand in for EXD generally. CPU figures cover the instrumented worker segment, not game CPU frame time or a measured audit-overhead delta. The 60 Hz cap prevents treating presentation rate as uncapped rendering throughput.

A separate eager `DXMT_PIXEL_FORMAT_VIEW=always` initial-phase run produced a 3.829 ms median union versus 3.836 ms for the default policy, with identical attachment proxies/pass counts. This single comparison shows no meaningful benefit; it is not a compression benchmark.

## EXD attachment findings

The 30-frame late detail window, frames 1200–1229, has 61–63 render nodes after optimisation and no standalone clears or resolves. It identifies:

- A 1792×1008, 2× MSAA depth/stencil prepass (25 indexed draws in this window), followed by a single-sample depth-only fullscreen draw. The latter is consistent with shader-based depth reduction/resolve, but its shader arithmetic, stencil behaviour and inputs have **not** been proven.
- A 1792×1008, 2× MSAA `RG11B10Float` scene-colour pass with 27 indexed draws and the same MSAA depth allocation used by the prepass. Further colour/depth passes reload those resources.
- A single-sample pass with three `BGRA8Unorm` attachments and depth, plus other intermediate, geometry-shader-emulation and post-processing work. This is not evidence for a wholly deferred or wholly forward renderer; the main colour path uses MSAA, while the purpose of the three-target pass requires shader inspection.
- A 2× MSAA `RGBA16Float` colour/depth pass, post-processing at full and reduced sizes, and a 2× MSAA `BGRA8Unorm` pass using combined store-and-resolve. The existing optimiser already fuses that explicit resolve.
- Most large colour allocations have private storage and `ShaderRead | RenderTarget` usage. Only selected targets require ShaderWrite. Depth/stencil attachments carry PixelFormatView usage, and actual stencil-plane reinterpretation is logged. Removing this flag globally would break required views or force reallocation.

The loaded detail window records roughly 64 attachment and 41 layout rejection attempts per frame, with zero argument-buffer, load/store-action and dependency rejections. The existing argument-buffer merge restriction is therefore not a demonstrated EXD bottleneck. Different resources/layouts cannot safely be merged by simply ignoring these checks.

## Native GPU evidence

Two bounded 10-second Metal System Traces attached only to the private EXD process. The labelled run spans a scene transition, so signatures are reported separately rather than averaged into one scene result. Representative fragment-channel interval medians are:

| Signature | Samples | Median interval |
|---|---:|---:|
| MSAA RG11B10 scene colour, 2 draws | 436 | 0.761 ms |
| Same attachment pattern, 26 draws / ~309k vertices | 72 | 4.018 ms |
| Same attachment pattern, 26 draws / ~302k vertices | 60 | 4.891 ms |
| Same attachment pattern, 28 draws / ~571k vertices | 73 | 3.763 ms |
| Single-sample depth-only fullscreen draw | 559 | 0.459 ms |
| MSAA RGBA16 colour/depth, 4 draws | 100 | 2.293 ms |

These are **channel interval durations**, not isolated shader costs, predicted savings or additive frame timings. Tiler/vertex, renderer/fragment and compute work overlap. The apparent expensive depth fullscreen pass makes shader identification worthwhile, but replacing it cannot be assumed to save its entire interval.

The trace recorded only the `RT Unit Active` hardware counter. It did **not** record DRAM bandwidth or attachment-compression counters. Metal device allocated-size observations ranged approximately 1.13–2.03 GB in the first trace; this is allocation accounting during a profiled run, not resident VRAM, an audit memory-overhead measurement or evidence of a leak.

One native Metal capture of frame 300 was also produced and its metadata reports exactly one captured frame. It is an early-phase capture, not a loaded-scene capture. The ~1.1 GB bundle and raw Instruments traces remain private under `build-in-process`; they contain game resources and broader process metadata. Only curated telemetry and aggregate results are archived in this repository.

## Prioritised experiments

1. **Identify the depth fullscreen shader and its resource lineage.** Inspect a loaded-scene GPU capture or add a bounded shader-hash diagnostic. Determine whether it performs min/max MSAA depth reduction, stencil processing, or additional work. Compare native depth-resolve output including reverse-Z, all pixels, depth discontinuities, stencil, per-sample consumers and later depth tests. Prototype replacement only if semantics match. Measure complete GPU intervals with and without the change; retain the original path otherwise. The EXD depth prepass also informs foveation experiments, but does not prove that reconstructed depth supports equality/per-sample testing.
2. **Prove attachment discard opportunities.** Existing `DiscardResource`/`DiscardView1` are no-ops in `d3d11_context_impl.cpp` with explicit TODOs for DontCare. They are a generic TBDR opportunity, but no corresponding calls were observed in this EXD audit. A bounded implementation needs subresource/rect correctness, alias tracking and tests before claiming EXD benefit. Inferring full overwrite from one fullscreen draw is unsafe with blending, discard, depth/stencil and sample masks.
3. **Investigate repeated MSAA depth/stencil stores/loads at actual consumer boundaries.** Capture shader/resource reads before considering liveness-driven store elimination or keeping work in one encoder. Memoryless storage is unsuitable for resources read by later passes. Retain attachment and ordering checks.
4. **Measure compression and synchronisation before changing policy.** Current colour usage is already fairly narrow; depth/stencil views are real. Obtain appropriate hardware counters or a controlled usage-only experiment before claiming compression savings. Likewise, hundreds of fence calls do not establish hundreds of stalls. No fence removal is justified by these counts.

No general native-depth-resolve substitution, global discard heuristic, global memoryless allocation, relaxed merge guard or fence removal was introduced. The next change should be one independently toggled optimisation with a correctness comparison and a net GPU-time result.

## Validation and commands

The focused ordinary D3D11 regression draws to normal and 2× MSAA textures, resolves, and checks all 8,192 output pixels in each audit-off/on run. Both pass with Metal validation. Audit-on reports three render nodes merged into two, two clears merged into their renders, one resolve fused, and zero timestamp queries or mapped passes. Six analyser tests cover overlapping intervals, gaps, incomplete frames, duplicate/multiple queues, failed buffers and streaming XML references. The existing automatic-foveation suite also passes all ten cases, including eight fallback/default controls; each checks 4,194,304 pixels. Its synthetic timings are not EXD results.

```sh
# Requires the existing prepared private EXD copies and paired runtime.
MWXR_TBDR_SECONDS=60 scripts/run-exd-tbdr-audit.zsh summary
MWXR_TBDR_SECONDS=60 DXMT_TBDR_AUDIT_START_FRAME=1200 \
  DXMT_TBDR_AUDIT_FRAME_COUNT=30 scripts/run-exd-tbdr-audit.zsh detail
python3 tools/analyze_dxmt_tbdr_audit.py /absolute/run/game/game.log \
  --min-frame 900 --max-frame 1600 --frames-csv /tmp/exd-frames.csv

# Optional native capture; substantial disk usage, separate from perf baselines.
MWXR_TBDR_SECONDS=60 MWXR_TBDR_CAPTURE_FRAME=1200 \
  scripts/run-exd-tbdr-audit.zsh capture
# Requires process profiling permissions. Default starts at frame 180.
MWXR_TBDR_TRACE_START_FRAME=900 python3 tools/capture_private_exd_metal_trace.py

MWXR_IN_PROCESS_WINE="$PWD/build-in-process/exd-ngx/wine" \
  scripts/test-dxmt-tbdr-audit.zsh
python3 -m unittest discover -s tests/macos -p '*tbdr*.py'
```

To reproduce trace aggregation, export the `metal-gpu-intervals`, `metal-application-encoders-list`, `metal-current-allocated-size` and `gpu-counter-info` tables using `xcrun xctrace export --input metal.trace --xpath '/trace-toc/run[@number="1"]/data/table[@schema="TABLE"]' --output TABLE.xml`, then run `tools/analyze_metal_tbdr_trace.py` on that directory. Retain `xctrace.log` for the attached-PID filter. Use the trace TOC to verify available table names locally; do not publish its broad environment contents.

[Curated results and run manifest](results/dxmt-tbdr-audit-20261109/runs.json) preserve separate baseline, detail, eager-view and native-profiler aggregates. This audit is independent of foveation. Future comparisons should keep baseline, TBDR optimisation only, foveation only, and both as separate configurations, with the same loaded scene and resolution.

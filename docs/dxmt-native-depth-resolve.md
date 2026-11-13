# EXD depth resolve: shader identification and native experiment

9 November 2026. Follow-up to the [TBDR audit](dxmt-tbdr-audit.md).

**EXD's observed depth-only pass computes the maximum of its two MSAA depth samples. A native Metal implementation is correct in the focused experiment, including stencil and MSAA-source preservation. Its measured benefit at 2× is only about 0.023 ms for the synthetic seed-plus-resolve chain; 4× is slower. No automatic DXMT replacement or EXD speedup is claimed.**

## Identified EXD pass

[Patch 0010](../patches/dxmt/0010-depth-resolve-shader-diagnostics.patch) extends the existing opt-in audit with ordinary graphics-pipeline shader hashes/state snapshots and pixel texture-binding provenance. An explicit `DXMT_TBDR_SHADER_DUMP` directory optionally retains original DXBC at shader-cache insertion, bounded to 1 MiB per shader and 64 MiB per device cache. It does not alter shaders. No new queries, encoders, flushes or waits are added. These diagnostics remain disabled by default.

The observed shader pair is:

- Vertex SHA1: `836dd050c282a6dafa85886f38e72b5bc759e8bb`.
- Pixel SHA1: `7950e3c282723300c7868b44f76ea750bacd1403`.

The private pixel bytecode loads samples 0 and 1 from a multisampled texture at slot 0 and writes their floating-point maximum to the depth output. It converts interpolated coordinates to integers; it has no colour output or other resource reads. State at pipeline setup enables depth writing, comparison Always, and disables stencil testing.

The source allocation matches the preceding 1792×1008, 2× MSAA `Depth32Float_Stencil8` prepass. The depth SRV is a distinct Metal view, so joins use **resource plus allocation identity**, rather than matching the native view handle to the attachment texture. The destination is a different single-sample allocation of the same format/dimensions. Subsequent passes reattach the original MSAA allocation and sample the single-sample destination. Discarding the MSAA source is therefore unsafe.

The first diagnostic window found 22 consistent candidates before the existing 20,000-row detail budget was exhausted. The final eight-frame window identifies the same pair, matching source allocation and later MSAA attachment use in all eight frames, without exhausting that budget. The analyser checks the one-direct-draw label, depth-only state, source provenance and later source attachment use; it deliberately calls these **candidates**, not automatically safe replacements.

### Coverage is not established by hashes

The vertex shader generates rectangle endpoints from vertex ID, with separate clip-space and texture-coordinate rectangles supplied by constant buffer 0. It is not an unconditional pixel-coordinate shader. Even this exact VS/PS pair may scale, offset, flip or restrict the output through its constants, viewport, scissors, draw arguments or topology.

A safe recogniser must establish full coverage and one-to-one addressing from the actual draw state and constants. Hashes alone are insufficient. Additional constraints include no other draws or side effects, compatible source/destination views and subresources, sample count, source sample positions, depth/stencil state and preserved later consumers. State snapshots on pipeline setup are not a substitute for tracking every later state change.

Original game DXBC and full disassembly stay private in `build-in-process/exd-ngx/tbdr-depth-shaders`. The CPU-only [disassembly utility](../tests/windows/dxbc_disassemble.cpp) is supplied for reproduction. No game bytecode or disassembly is included in the results archive.

## Focused native Metal experiment

The standalone [probe](../tests/native/metal_depth_resolve_probe.m) uses ordinary Metal, independent of Wine, DXMT, OpenXR or foveation. It creates private combined depth/stencil textures at EXD's observed 1792×1008 size, and tests 2× and 4× MSAA. A sample-frequency seed draw produces distinct, deterministic finite depth values in every sample, alternating which sample contains the maximum across neighbouring pixels.

It compares three complete GPU chains:

1. Seed render, store MSAA source, then a fullscreen shader that reduces all samples with max into the existing single-sample depth target.
2. Seed render with native maximum-depth resolve attached to that render pass, retaining the MSAA source using `StoreAndMultisampleResolve`.
3. Seed render followed by a separate no-draw render pass that loads the source and performs native maximum-depth resolve, also retaining the source.

Only the depth attachment resolves. The destination stencil is pre-initialised to `0xa7`; source stencil is `0x35`. Compute readback verifies every destination depth, destination stencil, original MSAA depth sample and original MSAA stencil sample. No source/store elimination is assumed. All three paths pass at both sample counts with Metal validation enabled. The same checks pass during performance runs.

A fourth path deliberately uses native sample-0 resolve: it produces 903,168 depth mismatches, exactly half the destination pixels, while preserving source/stencil. This negative control confirms that the validator detects an incorrect reduction mode.

Apple documents [depth resolve filters](https://developer.apple.com/documentation/metal/mtlrenderpassdepthattachmentdescriptor/depthresolvefilter), including the sample-0 default, and lists combined depth/stencil resolve support in its [Metal feature tables](https://developer.apple.com/metal/feature-sets/). The M5 behaviour described here is additionally validated locally; it is not a claim that every Metal device supports this path. In particular, relying on the default filter would implement the wrong EXD operation.

## Performance

Host remains Apple M5 / macOS 27.0.1. Each trial rotates the four cases' order per batch, warms up 10 batches, then measures 15 batches of eight complete seed-plus-resolve repetitions. GPU command-buffer duration is divided by eight. CPU completion waits, validation readback and compilation are outside the measured GPU interval. Three trials run with validation enabled, followed by three with validation disabled; the table uses validation-disabled runs.

| MSAA | Shader chain, trial medians | Fused native max | Separate native max | Median paired saving: fused / separate |
|---|---|---|---|---|
| 2× | 0.2121 / 0.2128 / 0.2104 ms | 0.1882 / 0.1898 / 0.1875 ms | 0.1924 / 0.1935 / 0.1925 ms | +0.0230 / +0.0192 ms |
| 4× | 0.4472 / 0.4490 / 0.4486 ms | 0.4599 / 0.4606 / 0.4608 ms | 0.4705 / 0.4718 / 0.4714 ms | −0.0121 / −0.0228 ms |

This proves a small net GPU benefit for this 2× workload and a regression at 4×. It does not isolate the resolve unit's cost or predict EXD's result. Earlier sequential-case runs varied markedly with warm-up/clock state; the final repeated, rotated-case results supersede those exploratory numbers. The earlier ~0.459 ms Instruments fragment-channel interval for EXD's depth pass must not be treated as recoverable time.

## Implementation boundary and next decision

DXMT's current Winemetal render-pass bridge sets depth texture/load/store/clear fields but does not transport a depth resolve texture/filter. Colour resolve support is not sufficient. A future prototype would need a paired bridge change and resource lifetime/dependency tracking, plus a conservative pass replacement or fusion stage. It must keep MSAA depth/stencil stores for the later scene passes, preserve destination stencil, and decline scaled/partial/ambiguous draws.

The smallest next experiment is bounded inspection of the actual rectangle constants, viewport/scissor, topology and draw arguments for this candidate, without GPU readback waits. If those data can be proven at translation time, an explicit test profile could attempt replacement and compare the full EXD GPU frame against baseline. If constants are only available on the GPU, declining the optimisation is preferable to speculative readback or broad shader rewriting.

Given a measured native saving of only ~0.023 ms in this test and a 4× regression, this is **not yet a high-priority general DXMT optimisation**. Do not build an elaborate recogniser on the assumption that hardware resolve is automatically faster. Repeated depth/stencil attachment traffic and other pass costs remain separate TBDR investigation targets. Existing foveation behaviour is unchanged.

## Reproduction and evidence

```sh
# Native test: builds and runs all 48 correctness/control cases across 12 trials.
scripts/test-metal-depth-resolve.zsh

# Prepared private EXD runtime only; simulated headset, existing settings restored.
mkdir -p "$PWD/build-in-process/exd-ngx/tbdr-depth-shaders"
MWXR_TBDR_SECONDS=60 DXMT_TBDR_AUDIT_START_FRAME=1200 \
  DXMT_TBDR_AUDIT_FRAME_COUNT=8 \
  DXMT_TBDR_SHADER_DUMP="Z:$PWD/build-in-process/exd-ngx/tbdr-depth-shaders" \
  scripts/run-exd-tbdr-audit.zsh detail
python3 tools/analyze_dxmt_depth_resolve.py /absolute/run/game/game.log

python3 -m unittest discover -s tests/macos -p '*depth_resolve*.py'
```

[Curated results](results/dxmt-native-depth-resolve-20261109/summary.json) include all native trial rows and paired comparisons. Shader/resource identities, bounded trace rows, provenance hashes and regression results are archived alongside them. Raw game resources remain under the private build directory.

The final build passes the existing D3D11 audit-off/on pixel and merge/resolve regression with diagnostic detail enabled, and the ten-case automatic-foveation suite. Nine analyser tests pass. Applying patches 0001–0010 reproduces all 307 source files from the pinned base. Private EXD settings were restored and temporary EXD/service processes stopped. The audit was confined to the private source/runtime copies; the original DXMT checkout remains clean.

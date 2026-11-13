# EXD layout audit: who reads the scene targets

2026-11-11. Follow-up to [stay-warped read translation](dxmt-stay-warped-read.md).

**In the loaded level, almost every use of EXD's scene-sized targets is something
the stay-warped mechanism already handles or can be extended to handle.** Of 218
uses per frame, 205 are attachments, clears, resolves or pixel-shader reads in
scene-sized single-target passes. The exceptions are two compute dispatches that read the
resolved depth, two three-target (MRT) passes, three reads from smaller passes,
and the hand-off of the final image to OpenXR. Nothing copies the scene targets,
reads them from a vertex shader or writes them from compute.

This is a passive trace of two frames on the simulated headset. No foveation
was enabled and no rendering was changed.

## Method

[Patch 0016](../patches/dxmt/0016-passive-layout-audit.patch) adds
`DXMT_LAYOUT_AUDIT=1`. In a window of Present frames it logs every texture
access with the encoder kind (render, compute, blit, clear, resolve, present,
upscale), the shader stage, and whether it was a shader binding or an
attachment/operand, plus each render pass's size, target count and sample
count. It adds no queries, encoders, flushes or waits.

`tools/analyze_dxmt_layout_audit.py` takes the largest MSAA colour+depth pass
as the scene size, collects every texture written as an attachment by a pass of
that size (and their resolve destinations), and classifies each use.

Run: private clone `build-in-process/exd-warp` (APFS clones of the `exd-ngx`
game and prefix, runtime with patches 0001–0016), private simulated Monado
service, upscaler off, 2× MSAA, 90 s, frames 1200 and 1201. Both frames have 63
render passes and identical totals, so this is the loaded level. Game Metal
validation was off, as in the earlier EXD traces.

## Result

Scene size is 1792×1008: one double-wide target holding both eyes. Sixteen
textures belong to the scene family, including the 2× MSAA `RG11B10Float`
colour and `Depth32Float_Stencil8`, their single-sample counterparts, a 2× MSAA
`RGBA16Float` pair, three single-sample `BGRA8Unorm` targets and the final
`RGBA8Unorm_sRGB` image.

| Use of a scene-family texture, per frame | Count | Status with patch 0015 |
|---|---:|---|
| Pixel-shader read in a scene-sized pass | 163 | Translated, if the instruction is `Load`, `Sample` or `SampleLevel` |
| Attachment in a scene-sized single-target pass | 29 | Mapped (colour, depth/stencil, 1× or 2× MSAA) |
| Clear | 11 | Layout-neutral |
| Attachment in a scene-sized three-target pass | 8 | **Not mapped**: DXMT's rule allows one colour target |
| Pixel-shader read in a smaller pass | 3 | Works if those passes stay ordinary |
| Compute-shader read | 2 | **Not translated** |
| Resolve | 2 | Tag follows the resolve |
| Blit or copy, vertex-stage read, UAV write | 0 | — |

Scene-sized passes per frame: seven 2× MSAA colour+depth, two 2× MSAA
depth-only, one single-sample depth-only (the depth max-resolve), two
single-sample three-target passes with depth, and twelve single-sample
fullscreen colour passes.

### Read instructions

A second run also logged the shader hash with each binding and dumped the
shaders' DXBC. `tools/analyze_dxmt_layout_reads.py` joins each scene-family
binding to the instructions its shader uses on that slot. In both frames there
are 162 such bindings from 26 distinct shaders (this run had 157 pixel reads in
scene-sized passes against 163 in the first; the other totals are identical).

| Instruction on a scene-family texture | Bindings | Patch 0015 |
|---|---:|---|
| `SampleLevel` (`sample_l`) | 94 | Translated (2 of these are the compute reads, whose stage is not) |
| `Load` (`ld`) | 58 | Translated |
| `GetDimensions` (`resinfo`) | 19 | No coordinates |
| `Sample` | 6 | Translated |
| Per-sample `Load` (`ld2dms`) | 4 | Translated |
| `SampleBias`, `SampleGrad`, comparison samples, `Gather`, texel offsets | 0 | — |

All 26 shaders use only instructions the translation already covers. None uses
explicit derivative instructions. Twenty of them read `SV_Position`.

### The exceptions

- **Compute reads of depth.** Two dispatches per frame bind the single-sample
  1792×1008 depth as a shader resource and write an 896×504 `RG16Float` image.
  Their other inputs are small (16×16, 135×5, 1×1). What they compute was not
  identified. They need compute-stage read translation or an ordinary copy of
  that one depth texture.
- **Three-target passes.** Two passes per frame render into three `BGRA8Unorm`
  targets with the single-sample depth attached. Metal rate maps support
  multiple colour attachments; DXMT's mapped-pass rule and the layout tag do
  not yet.
- **Smaller passes.** Two 896×504 passes read the single-sample `RG11B10Float`
  colour, and the 1280×720 desktop mirror reads the final image. An ordinary
  pass can read a mapped texture through the translation, so these work if
  they are left unmapped.
- **OpenXR hand-off.** The final 1792×1008 `RGBA8Unorm_sRGB` image is the
  swapchain image. The compositor reads it outside DXMT, so it must either be
  restored to ordinary layout in its last pass or submitted with its map
  through the existing Monado foveation path.

## What this does not show

- Shaders that assume uniform pixel spacing: depth-to-position reconstruction,
  neighbourhood kernels (ambient occlusion, edge filters), and anything using
  derivatives. These would run but could look wrong.
- Other scenes, effects that appear only sometimes (the stencil-tested draws
  seen in one of two frames on 9 November), DLSS mode, 4× MSAA, the PS VR2's
  5600×2856 target, or a submitted depth layer.
- Deferred contexts and CPU access: the audit sees GPU texture accesses only.
- Temporal history beyond one case. In frame 1201 the single-sample depth is
  read before anything writes it, so the previous frame's depth is in use. With
  a fixed map that is harmless; a map that moves with gaze would need the old
  map kept for that read. No other scene-family texture showed this, but the
  check is two consecutive frames in one scene.

## Consequences

The double-wide target means the map needs two foveal peaks, one per eye. Rate
maps are separable, so one map can express that.

For a bounded EXD trial the missing pieces are, in order of how much each
blocks:

1. Mapped three-target passes.
2. Compute-stage read translation, or one ordinary depth copy for the two
   compute dispatches.
3. A two-peak map and the final restore or Monado hand-off.

The instruction tally removes the largest unknown: no EXD shader in these
frames needs a read form that is not translated. What remains unknown is
whether the 26 shaders' arithmetic tolerates non-uniform pixel spacing, which
only shows up by running them warped and looking.

## Reproduce

```sh
# Private clone with the patched runtime; see docs/dxmt-stay-warped-read.md for the build.
MWXR_EXD_EXPERIMENT_ROOT="$PWD/build-in-process/exd-warp" \
MWXR_EXD_SERVICE_ENV="SIMULATED_DESKTOP=1 XRT_MACOS_REFRESH_RATE_HZ=0" MWXR_EXD_TAG=layout \
DXMT_LAYOUT_AUDIT=1 DXMT_LAYOUT_AUDIT_START_FRAME=1200 DXMT_LAYOUT_AUDIT_FRAME_COUNT=2 \
  scripts/run-exd-headset-compare.zsh msaa2 90
python3 tools/analyze_dxmt_layout_audit.py /path/to/run/game/game.log --frame 1200

# Read instructions: same run plus DXMT_TBDR_SHADER_DUMP="Z:/absolute/shader-directory", then
# dxbc_disassemble.exe --directory Z:/absolute/shader-directory in the private Wine runtime.
python3 tools/analyze_dxmt_layout_reads.py /path/to/run/game/game.log /absolute/shader-directory --frame 1200
```

[Frame 1200 summary](results/dxmt-exd-layout-audit-20261111/frame-1200.txt),
[frame 1200 JSON](results/dxmt-exd-layout-audit-20261111/frame-1200.json),
[frame 1201 JSON](results/dxmt-exd-layout-audit-20261111/frame-1201.json),
[read instructions](results/dxmt-exd-layout-audit-20261111/reads-frame-1200.txt).
Game shader bytecode and disassembly stay private; only counts and hashes are archived.
The raw game log stays under `build-in-process`. The script restored the
private save and settings and stopped the private service afterwards.

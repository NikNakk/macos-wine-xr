# EXD replay audit and HDR/stencil compatibility gate

9 November 2026. Follow-up to the [joined mapped-prepass experiment](dxmt-joined-prepass-foveation.md).

**EXD needs more than the extra depth prepass tested previously.** Its masked prepass shaders must retain discard and sample-coverage behaviour, and its ordinary stencil buffer must receive main-pass material bits that later passes test. A new D3D11 HDR/combined-stencil fixture reproduces both failures and verifies a stencil-only replay control. No EXD foveation profile is enabled and no new performance benefit is claimed.

The [joined HDR/stencil timing follow-up](dxmt-hdr-joined-foveation.md) now
measures that fourth traversal with sampled alpha, native HDR resolves and the
actual stencil-tested consumer. The next remaining boundary is EXD's later use
of the original MSAA colour source.

## Actual EXD evidence

The private EXD clone ran with the simulated headset and the preserved upscaler-off, 2× MSAA settings. Base foveation and mapped rendering remained off. The final bounded audit captures frames 1200–1201 and restores the private settings afterwards. Game Metal validation remains disabled for the previously documented unrelated blit assertion, so this game trace is architectural evidence, not an EXD image-correctness pass.

[Patch 0012](../patches/dxmt/0012-per-draw-depth-stencil-audit.patch) adds opt-in `DXMT_TBDR_DRAW_STATE=1` snapshots under the existing audit/detail controls. It records shader identities, depth/stencil operations and references, DSV flags, coverage output, alpha-to-coverage and selected potential-effect bindings at each PreDraw, including state changes that do not create a new pipeline. It inserts no GPU queries, encoders or waits. The analyser joins resource/allocation provenance and verifies snapshot counts against both selected pass labels.

Both final frames identify the same 1792×1008, 2× MSAA `RG11B10Float` main scene pass and its earlier combined `Depth32Float_Stencil8` prepass:

| Observation per captured frame | Result |
|---|---|
| Depth prepass | 25 draws |
| Existing null pixel shader | 18 draws |
| Masked pixel shaders retained | 7 draws, two PS identities |
| Main colour pass | 27 draws |
| Main PS coverage output | 7 draws |
| Main depth state | Greater-or-Equal, writes disabled |
| Main DSV flags | Read-only depth, **writable stencil** |
| Main stencil | Front/back Always; Keep on stencil/depth failure; Replace on pass |
| Stencil write mask | `0xf0` |
| Stencil references | `0x80` for 21 draws, `0xc0` for four, `0x40` for two |

The main pass and prepass use the same underlying MSAA depth/stencil allocation. Shader views can have distinct native Metal handles; resource plus allocation identity is used for joins.

The private DXBC inspection confirms that both non-null prepass pixel shaders contain **discard instructions and an explicit coverage output**, with no depth output or UAV declarations found. They each sample a texture. Their corresponding main pixel shaders also use discard/coverage. Removing those prepass shaders changes depth/sample coverage and cannot be justified merely because the pass has no colour target.

No GS/HS/DS, UAV-slot or stream-output binding was observed in the selected draws. That narrows this experiment but does not prove replay safety: input/resource versions, predication, queries, instancing, draw arguments, derivatives and lifetime semantics still require preservation.

In frame 1201, three later draws reattach the same ordinary depth/stencil allocation and compare stencil using **Equal, read mask `0x81`, reference `0x81`**. This comparison observes bit 7, which the main pass writes. Such a consumer is not present in the corresponding selected later attachments in frame 1200; one frame's absence cannot be used as a discard guarantee. We have not identified the visual purpose of this effect.

The selected prepass pixel shaders have 35/37 static instruction lines; selected main pixel shaders span roughly 197–1014. Vertex shaders are also substantially larger than the preceding trivial fixture. These counts are diagnostic facts, **not GPU instruction costs or a prediction of replay time**. Original game DXBC and full disassembly remain private; only hashes and derived facts are archived.

## Focused HDR/stencil fixture

[Patch 0013](../patches/dxmt/0013-explicit-combined-stencil-diagnostic.patch) extends only the existing opt-in attachment diagnostic to a combined D32Float/Stencil8 texture when depth and stencil reference the same attachment, retaining the sample-count, map-size and layer restrictions. Automatic foveation still rejects depth/MSAA. No generic stencil materialisation or pass replay is added.

The [new fixture](../tests/windows/dxmt_hdr_stencil_probe.h) creates ordinary D3D11-owned `R11G11B10_FLOAT` MSAA colour and typeless combined depth/stencil resources. It uses read-only-depth DSVs for shading and the stencil-tested consumer, with stencil still writable where appropriate. This avoids expanding the native texture-import interface.

At 512² and 2×/4× MSAA, it renders background, a masked foreground triangle and a thin strip. The pixel shaders use discard and explicit coverage output. Main shading uses Greater-or-Equal, read-only depth, and Replace with high-nibble stencil masks/references. The low nibble is deliberately initialised to `0x05` to test preservation of unrelated bits; this is a controlled fixture value, not a claim about EXD's clear value.

The fixture checks:

1. Ordinary one-pass HDR/depth/stencil against ordinary prepass plus main shading.
2. Mapped one-pass output against a mapped prepass plus mapped main shading, including every active depth/stencil sample.
3. Full-rate mapped identity against ordinary rendering.
4. Dropping the masked prepass PS as an expected failure.
5. Retaining ordinary prepass depth while only the hidden target receives main shading, leaving ordinary stencil stale.
6. A later **actual D3D11 stencil-tested marker draw**, using Equal/read-mask `0x81`/reference `0x81`.
7. A full-resolution stencil-only replay using the original coverage shader and colour writes disabled. It must restore stencil and the marker while leaving ordinary colour and depth unchanged.
8. Native MSAA HDR colour resolve followed by an ordinary shader read, confirming values above 1 survive.

Six cases cover both sample counts and peripheral axis rates 1, 0.5 and 0.75, with Metal validation enabled. An independent audit verifies three actually mapped render passes per case; this does not rely on pre-guard labels.

| Control/counterexample | 2× MSAA | 4× MSAA |
|---|---:|---:|
| Ordinary and matching mapped prepass errors | 0 | 0 |
| Full-rate HDR/depth/stencil errors | 0 | 0 |
| Changed colour/depth samples after dropping mask PS | 66,220 | 160,680 |
| Retained ordinary depth errors | 0 | 0 |
| Stale ordinary stencil samples without main replay | 524,288 | 1,048,576 |
| Reference marker samples | 88,738 | 149,243 |
| Marker samples with stale stencil | **0** | **0** |
| Stencil errors after stencil-only replay | 0 | 0 |
| Colour/depth changes caused by that replay | 0 | 0 |
| Marker samples restored | 88,738 | 149,243 |
| Peak HDR value before/after native resolve | 4.0 / 4.0 | 4.0 / 4.0 |

The negative controls are independent of peripheral rate: they examine the ordinary full-resolution side of the design. The mapped-layout controls pass at every tested rate. This is a correctness gate, not a final HDR reconstruction/image-quality or whole-frame performance measurement.

## Consequences for implementation

The preceding ~43–45% synthetic joined GPU reduction **does not include the stencil-preservation work now shown to be necessary for EXD's selected pass**. It remains valid for that restricted D32/no-stencil workload, but cannot be treated as an EXD projection.

A straightforward compatible candidate now has four geometry traversals: retained ordinary depth prepass, hidden mapped depth prepass, mapped colour shading, and ordinary stencil-only replay. Masked coverage must survive in the relevant prepass/replay shaders. The new control proves this can restore stencil in the restricted scene; its cost is not measured here.

Colour writes disabled do not establish that colour shading becomes free. The current `ShaderVariantPixel` construction in `src/d3d11/d3d11_pipeline.cpp` passes sample mask, dual-source blending, depth presence and UNORM output information; it has no explicit colour-output-suppression key. Metal's resulting pipeline may optimise unused work, but that must be measured rather than assumed. A coverage-only variant would need to preserve discard, depth/coverage and other observable effects while eliminating only unused colour computations.

**The smallest next performance experiment is the joined four-traversal path**, including original mask/coverage work, native HDR resolve and reconstruction. Compare the colour-write-disabled replay with an explicit restricted coverage-only control to establish whether output elimination is worth investigating. Only after a net benefit survives should we build any capture/replay machinery for EXD.

Writing material stencil early during the original prepass is another possible engine-specific optimisation, but it requires proving draw/material correspondence and absence of intervening consumers or writes. The differing shaders/draw counts mean that simply pairing draws by order or shader hash would be unsafe. It was not attempted.

The original MSAA colour representation, aliases, later source reads, stereo subrects, temporal effects and gaze remain separate barriers. A correct final colour or stencil output does not solve those resource semantics automatically.

## Reproduction and evidence

```sh
# Prepared private game/prefix/runtime; patches 0001–0012 for this audit.
mkdir -p "$PWD/build-in-process/exd-ngx/tbdr-depth-shaders"
MWXR_TBDR_SECONDS=60 DXMT_TBDR_AUDIT_START_FRAME=1200 \
  DXMT_TBDR_AUDIT_FRAME_COUNT=2 DXMT_TBDR_DRAW_STATE=1 \
  DXMT_TBDR_SHADER_DUMP="Z:$PWD/build-in-process/exd-ngx/tbdr-depth-shaders" \
  scripts/run-exd-tbdr-audit.zsh detail
# Build/run tests/windows/dxbc_disassemble.cpp in the private Wine runtime:
# dxbc_disassemble.exe --directory Z:/absolute/private/shader-directory
python3 tools/analyze_dxbc_replay.py /absolute/private/shader-directory > /tmp/shader-facts.json
python3 tools/analyze_dxmt_replay_audit.py /absolute/run/game/game.log \
  /tmp/shader-facts.json --size 1792x1008 --color-format 92

# Paired private runtime with patches 0001–0013 and rebuilt helper/executable:
scripts/build-dxmt-depth-foveation-probe.zsh
scripts/test-dxmt-hdr-stencil.zsh
python3 -m unittest discover -s tests/macos -p '*replay*.py'
```

Use the same build variables as the preceding [joined test](dxmt-joined-prepass-foveation.md). [Curated audit, shader facts and fixture results](results/dxmt-exd-replay-audit-20261109/exd-audit.json) include separate provenance for the patch-12 game audit and patch-13 fixture. Raw game shaders, settings and broad process metadata are excluded.

The per-draw diagnostic passes the ordinary audit-off/on pixel and merge/resolve regression. The 16-case joined-prepass suite, depth/MSAA controls and ten-case automatic-foveation suite also pass. Five shader/stencil analysis tests pass. All thirteen patches reproduce the private source byte for byte from the pinned base. Private EXD settings were restored and temporary game/service processes stopped; installed Steam/game/runtime settings were not changed.

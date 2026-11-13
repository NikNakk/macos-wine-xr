# Fixed foveation using an existing depth prepass: compatibility gate

9 November 2026. This follows the [TBDR audit](dxmt-tbdr-audit.md) and [depth-resolve identification](dxmt-native-depth-resolve.md).

**Copying ordinary full-resolution prepass depth into a Metal rate-map layout does not preserve the tested colour pass's depth tests.** A matching mapped depth prepass works, but the tested nearest/minimum/maximum copies produce missing shading and incorrect visibility. EXD's main pass has not been redirected; no new EXD performance result is claimed.

The [joined-prepass follow-up](dxmt-joined-prepass-foveation.md) now measures the
extra mapped depth prepass and reconstruction in one D3D11 GPU interval. It
retains a net saving in the restricted fixture, with resource/stencil barriers
still preventing automatic EXD use.

## Why this experiment

The existing bounded EXD trace identifies its 1792×1008, 2× MSAA main colour/depth pass with 27 indexed draws in one sampled frame. The recorded pipeline setup states use D3D11 `GREATER_EQUAL` depth comparison, disable depth writes, enable stencil, and use stencil write mask 240. They reattach the MSAA allocation written by the earlier depth prepass. These snapshots identify relevant behaviour; they do not certify every subsequent draw/state mutation.

This makes a cheap reuse proposal attractive: keep the game's exact ordinary depth for later consumers, populate hidden mapped depth from it, then shade colour at lower peripheral density. The new test checks the depth part of that proposal before any EXD redirection or stencil/HDR implementation.

## Implementation

The existing D3D11 depth/MSAA probe and native helper were extended. The original eight-cell rate maps, full-rate central two cells, sample-position queries, native imports and resource ownership are reused. No new gaze or rate-map infrastructure was introduced.

The scene retains the existing sloped plane, overlapping triangle, central/peripheral thin strips and background. This mode adds a far triangle drawn last to expose incorrect visibility. It uses reverse depth values, `GREATER_EQUAL`, and read-only depth in colour shading. The colour shader remains the restricted, depth-invariant synthetic shader; the depth prepass has no pixel shader. Formats remain RGBA8 plus D32Float, with 2×/4× MSAA.

Controls compare:

1. Ordinary one-pass shading against an ordinary depth prepass followed by depth-read-only shading.
2. Mapped one-pass shading against a depth prepass and read-only shading both using the same map.
3. Full-rate mapped depth seeded from the ordinary prepass, followed by mapped read-only shading.

For the candidate path, a test-only native render pass writes hidden MSAA depth in physical coordinates. Its cached-for-call axis data uses the map's physical-to-logical conversion and the device's MSAA sample positions. It reads the matching ordinary source sample plane with nearest selection or a minimum/maximum over participating neighbouring taps. It does not resolve away sample planes. Depth-only fragment output writes every active physical sample; colour is subsequently shaded through ordinary D3D11/DXMT.

Comparison is **per MSAA sample in the active physical allocation**, against the matching mapped prepass reference. It happens before colour reconstruction, so it isolates incorrect depth reuse from ordinary foveation image loss. The original full-resolution depth and hidden seeded depth are checked for unwanted changes.

### Diagnostic guard correction

The first reduced-rate control caught an existing diagnostic limitation: with position translation enabled, DXMT disabled rate maps for depth-only passes because it required exactly one colour attachment. Consequently the supposed mapped prepass was ordinary, and the control correctly failed.

[Patch 0011](../patches/dxmt/0011-explicit-depth-only-rate-map-diagnostic.patch) admits zero-colour depth passes **only under the existing explicit attachment diagnostic switch**, with the existing D32Float, no-stencil, 2×/4×, single-layer and logical-size checks. It does not expand the automatic profile or select a game pass. Actual post-guard audit counts now verify **144 mapped D3D render passes per case**. Checking only the earlier SCENE row, logged before that guard, would not establish this.

## Results

M5 / macOS 27.0.1, Metal validation enabled. Six final cases cover logical 1024², both sample counts, and peripheral axis rates 1.0, 0.5 and 0.75. Active physical sizes are 1024², 696² and 864² respectively. All ordinary-prepass, matching-mapped-prepass and full-rate identity controls pass with zero colour errors. Source depth and read-only shading depth remain unchanged in every case.

At rate 0.5:

| MSAA | Seed method | Incorrect active colour samples | Percent | Missing-shading samples | Other colour mismatches |
|---|---|---:|---:|---:|---:|
| 2× | Nearest | 235,608 / 968,832 | 24.32% | 235,603 | 5 |
| 2× | Minimum | 15,601 / 968,832 | 1.61% | 15,268 | 333 |
| 2× | Maximum | 456,994 / 968,832 | 47.17% | 456,994 | 0 |
| 4× | Nearest | 471,541 / 1,937,664 | 24.34% | 471,538 | 3 |
| 4× | Minimum | 31,156 / 1,937,664 | 1.61% | 30,689 | 467 |
| 4× | Maximum | 913,266 / 1,937,664 | 47.13% | 913,266 | 0 |

Colour errors exceed 3/255 in at least one RGB channel. Missing shading means a reference-coloured sample becomes effectively black; other mismatches can include changed visible surfaces. These are sample counts, not final display-pixel or perceptual percentages. All three methods also fail at rate 0.75. Lower error from minimum does not make it compatible, and reverse-Z minimum is the more permissive depth choice here, not a generic conservative-occlusion guarantee.

A central physical ROI also has mismatches. A full-rate region within a varying map must not be assumed to align perfectly with the ordinary prepass's sample lattice. The test does not attribute all error to a specific interpolation/precision mechanism, nor does it test every possible reconstruction method. It demonstrates that these cheap copies do not reproduce rasterising depth in the mapped layout.

The matching mapped-prepass control remains exact, which points to rerasterising depth in that layout as the credible next route. Changing the game's depth comparison, adding an arbitrary bias or ignoring these holes would alter its rendering semantics and was not attempted.

[Matching mapped reference](results/dxmt-prepass-reuse-20261109/images/2x-reference.png), [minimum-depth seed output](results/dxmt-prepass-reuse-20261109/images/2x-filter1.png), [difference mask](results/dxmt-prepass-reuse-20261109/images/2x-filter1-difference.png). Images average MSAA colour samples in the **physical layout**; they are not reconstructed headset images. The black area outside the 696² active region is unused allocation space. Within that region, inspection shows missing strips and patterned shading holes even for the least erroneous tested filter. Orange masks mark average-pixel differences; the table retains the stricter per-sample comparison.

## What remains viable

A second, hidden mapped depth prepass could use the existing prepass geometry while preserving ordinary full-resolution depth for later consumers. The positive control establishes this only for the restricted fixture. Before implementing game redirection, measure one complete queued sequence containing the retained ordinary prepass, additional mapped prepass, mapped colour shading and colour reconstruction. Compare against the same scene's ordinary prepass plus colour rendering; do not add separate component medians and call them frame time.

Real EXD additionally requires safe handling of alpha-tested depth shaders, skinning, instancing, indirect/query/UAV side effects, combined depth/stencil updates, HDR colour, repeated colour loads, aliases, MSAA source reads and stereo subrects. Duplicating arbitrary draw streams is unsafe. Rerasterisation adds geometry work, so synthetic cheap-vertex savings cannot establish EXD benefit.

This experiment stops before a whole-path performance claim because the inexpensive depth-copy route fails correctness. The existing automatic colour-only profile remains unchanged. EXD and Steam were not launched or modified in this stage; all runs used the private probe prefix/runtime.

## Reproduction and validation

```sh
export DXMT_SOURCE_DIR="$PWD/build-in-process/dxmt-foveation-source"
export MWXR_WINE_SDK=/Users/nickkennedy/Code/wine-crossover/runtime
export MWXR_WINE_SOURCE=/Users/nickkennedy/Code/wine-crossover/sources/wine
export MWXR_IN_PROCESS_WINE="$PWD/build-in-process/exd-ngx/wine"
# Private DXMT patches 0001–0011, matching PE/Unix binaries.
scripts/build-dxmt-depth-foveation-probe.zsh
scripts/test-dxmt-prepass-reuse.zsh
```

Rebuild both helper halves and the executable together: the test parameter structure has an appended source-depth handle and a new operation. Native queue completion/CPU readback is deliberately synchronous and test-only. It must not be copied into a production path. Shader/resource/LUT construction and the fixture's existing timestamp operations are not a whole-frame timing protocol for this gate.

[Curated logs, summary, images and hashes](results/dxmt-prepass-reuse-20261109/summary.json) record the six cases separately. The existing 16-case resolve-first suite passes against the extended helper. The automatic and depth/MSAA regression outcomes are recorded alongside the result archive. The complete private patch series still reproduces all 307 source files from its pinned base.

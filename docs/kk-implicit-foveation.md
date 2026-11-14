# Implicit foveation in KosmicKrisp (prototype)

2026-11-12. Goal: foveate Vulkan and OpenGL (Zink) applications that do not use
fragment density maps, as DXMT's stay-warped path does for D3D11
([gaze-directed stay-warped foveation](dxmt-gaze-foveation.md)).

**State: Google Earth VR is foveated and its submitted image matches the
unfoveated one, on the simulator at 896×1007 and at 2800×2856 per eye.** The
game's GPU work per eye falls from 13.3 ms to 10.3 ms at the larger size. Fixed
map only (no gaze), one application, one dark scene, not worn, nothing
installed. The two faults of the first version are found and fixed.

## What exists already

The KosmicKrisp branch (`~/Code/alyx-native/workspace/kk-zink`) has experimental
`VK_EXT_fragment_density_map` on Metal rate maps
(`MESA_KK_EXPERIMENTAL=fdm_rate`, `kk_fdm.c`), written for native Alyx. Both
shader translations are decided at run time, per image:

- sampled and fetched reads translate their coordinates when the image's
  descriptor carries rate parameters marked valid
  (`kk_nir_lower_descriptors.c`);
- the fragment position is converted when the pass publishes parameters in the
  root table (`kk_shader.c`).

So an application that does not opt in needs only a policy and the cases the
extension lets a driver skip.

## Google Earth VR's frame

Through Zink, per frame (B is one 4× multisampled colour target holding both
eyes side by side, D its depth, T the single-sample texture Earth VR submits,
F another single-sample texture of the same size):

1. render F; render B and D; render F
2. render B and D with a resolve attachment into T
3. render B and D again
4. `vkCmdResolveImage` B into T
5. render T

The final image comes from step 4. xrizer imports T into DXMT and flips it into
the swapchain ([zink-kosmickrisp.md](zink-kosmickrisp.md)).

## The prototype

Private copy `build-in-process/kk-fov-src`, build `kk-fov-x86_64`, Zink root
`build-in-process/zink-fov`; change in
`patches/kosmickrisp/0001-implicit-foveation-prototype.patch`. Run with
`build-in-process/kk-fov-tools/earth.zsh TAG [SECONDS]`; `FOV=1792x1007` turns
it on, `HIRES=1 FOV=5600x2856` uses the high-resolution simulator.

`KK_IMPLICIT_FDM=WIDTHxHEIGHT[,OUTER[,CORE,EDGE]]` with
`MESA_KK_EXPERIMENTAL=fdm_rate`:

- multisampled attachment images of exactly that size are created as subsampled
  images; nothing else is foveated, so the submitted texture stays ordinary and
  xrizer, DXMT and the compositor are unchanged;
- a pass over them with no application density map gets a driver-built one: two
  full-rate cores side by side, OUTER the peripheral rate per axis (0.3), CORE
  and EDGE the full-rate and outer radii as fractions of half an eye's width
  (0.35, 0.75);
- every pipeline gets the fragment-position conversion and every sampler the
  coordinate translation;
- `vkCmdResolveImage` from such an image into an ordinary one un-warps with no
  new code: the common draw-based resolve fetches texels by position and the
  existing fetch translation sends each fetch through the map (nearest physical
  texel, all samples averaged);
- a resolve *attachment* into an ordinary image is redirected to a hidden
  single-sample image in the same foveated layout, so the pass stays foveated;
  after the pass a sampled blit copies it, through the map and with linear
  filtering, into the application's target.

## Results

| | Ordinary | Foveated |
|---|---|---|
| 896×1007 per eye: image, mean difference from ordinary (0–255) | – | 0.10 (two ordinary-path runs differ by 0.09) |
| 2800×2856 per eye: image, compared at 1000 px | – | 0.01; centre 0.00 |
| 2800×2856 per eye: xrizer's wait for the game's GL work, per eye | 13.3–13.4 ms | 10.2–10.4 ms (9.3–9.9 ms without the blit after the first resolve, which Earth VR does not need) |
| 2800×2856 per eye: frames per second | 27.8–28.9 | 28.7–30.0 |
| Physical size of the foveated target | 5600×2856 | 3968×2000 (49.6% of the pixels) |

All passes over the multisampled target were rate-mapped (none at full rate).
The frame rate hardly moves because the simulator paces at 60 Hz and both runs
sit at half of that; the GL wait is the measure. The scene is the night side of
the globe, dark and low in detail, so the image comparison is a weak test of
the periphery.

## Faults found on the way

- **Push descriptors were not preserved across the driver's internal draws.**
  `kk_meta_begin` saved the pointer to the application's pushed set 0 and
  `kk_meta_end` restored the pointer, but the internal draw pushes into the same
  object, so the application's descriptors were overwritten. Zink keeps using
  its pushed set from one pass to the next, so any internal draw at the end of a
  pass (the blit above, or the common resolve) left its later draws with the
  wrong resources and the frame black, with or without foveation. The contents
  are now saved and restored. This is a bug in the branch itself, not only in
  this prototype; applications are not told about an internal draw at the end
  of a pass and cannot be expected to push again.
- **The fragment-position conversion was not applied.** The first version set
  the shader key but not the condition that runs the lowering, so
  `gl_FragCoord` stayed in physical pixels. One Zink shader (a multisample copy
  that fetches at the fragment position) then had its fetch mapped twice, which
  gave a mosaic; with the conversion applied, translating the application's
  fetches is correct and is on.
- A resolve at the end of a pass is not the last word: Earth VR resolves again
  explicitly (step 4), which hid the blit's result. With that explicit resolve
  skipped (`KK_IMPLICIT_SKIP_EXPLICIT=1`) the blit alone gives the same image
  (mean difference 0.10).

## Open

- The map is fixed, sized by an environment variable, and assumes both eyes
  side by side in one layer. The gaze hook exists (`kk_fdm_set_gaze`) but
  nothing calls it here.
- The explicit resolve un-warps by nearest texel, with no filtering.
- Only Earth VR has been run, on one dark, low-detail scene. A Vulkan
  application that renders straight into the runtime's swapchain images is not
  covered by this route.
- Single-sample eye-sized targets are left ordinary, so an application that
  renders without multisampling is not foveated.

Diagnostic switches kept in the prototype: `KK_IMPLICIT_TRACE=1` (each distinct
image operation once), `KK_IMPLICIT_SEQ=N` (60 operations in order from the
Nth), `KK_IMPLICIT_NOMAP=1`, `KK_IMPLICIT_NO_FRAGCOORD=1`,
`KK_IMPLICIT_NO_SAMPLE=1`, `KK_IMPLICIT_NO_FETCH=1`,
`KK_IMPLICIT_NO_FETCH_APP=1`, `KK_IMPLICIT_NATIVE_RESOLVE=1`,
`KK_IMPLICIT_NOFLAG=1`, `KK_IMPLICIT_SKIP_BLIT=1`, `KK_IMPLICIT_SKIP_EXPLICIT=1`,
`KK_IMPLICIT_FULL_AREA_ONLY=1`, `KK_IMPLICIT_CLEAR_TEST=1`.

## Map and gaze from the OpenXR bridge (2026-11-13)

`KK_IMPLICIT_FDM=auto` makes the driver wait for the bridge instead of reading a
size from the environment:

- every multisampled attachment image is a candidate (it gets the parameter
  buffer at creation, because descriptors copy that address when written);
- `kk_implicit_fdm_set_profile(view_width, view_height, count, horizontal0,
  horizontal1, vertical)`, exported from the driver, gives one eye's size and
  the rate at `count` positions across each eye. A candidate whose size is one
  eye, or two eyes side by side, becomes foveated at its next pass;
- a new profile is taken up when a pass does not load the target (the start of
  that target's frame), so one frame's passes share a layout.

`MWXR_KK_FOVEATION=1|flip` in the bridge (`graphics_native.m`, build
`build-in-process/kk-gaze-gate`) finds the driver in the process and sends the
same angular profile the D3D11 path uses, per eye, whenever the quantised gaze
or the eye size changes. `flip` reverses the vertical profile for OpenGL's
bottom-up images. `earth.zsh` selects this with `BRIDGE=fixed|gaze|test|hold`.

The gaze path runs only for D3D11 OpenXR sessions (Earth VR through xrizer's
OpenGL mode). The direct Vulkan sessions added on 2026-11-12
([vulkan-bridge.md](vulkan-bridge.md)) return before it, so Talos and No Man's
Sky would need the same call from that path.

### Simulator runs (2026-11-13)

Earth VR only streams detail while the view moves, so a still simulated head
leaves it on a dark loading screen. `sim-game.zsh` now takes `SIM_MOVE=wobble`
(or `rotate`), which drops `SIMULATED_DESKTOP` so the head sways by itself.
Earth VR chose a different eye size from run to run (896×1007 or 2800×2856); in
`auto` mode the bridge reported each one and the driver followed it.

| Run (`earth.zsh`, `SIM_MOVE=wobble`) | Eye | Gaze | GL wait per eye | Frames/s |
|---|---|---|---|---|
| baseline (`earth-base5`) | 2800×2856 | – | 18.5 ms | 22 |
| `BRIDGE=hold` (`earth-hold`) | 2800×2856 | held ahead, 1 map | 8.1 ms | 45 |
| baseline (`earth-base6`) | 896×1007 | – | 4.8 ms | 58 (paced) |
| `BRIDGE=hold` (`earth-hold2`) | 896×1007 | held ahead, 1 map | 3.3 ms | 59 (paced) |
| `BRIDGE=fixed` | 896×1007 | none (`valid=0`), fixed profile | 4.5 ms | 47–50 |
| `BRIDGE=test` | 896×1007 | swept to ±22°, 398 map changes | 3.6 ms | 58 |

- In every run each FDM pass was rate mapped, none fell back to full rate, and
  the log showed only the baseline's three benign lines.
- Images: the loaded scene is correct with held and swept gaze: no mosaic,
  tears or misplaced tiles, and stepped edges only away from the gaze. The
  black octagon outside the lens area is Earth VR's hidden-area mask; the
  unfoveated `earth-hi-base` capture has it too.
- With a moving head, runs are captured at different poses, so pixel
  differences against the baseline (5.7–7.7 mean, larger in the full-rate
  centre than the periphery) measure the motion, not foveation. Use a still
  head after the scene has loaded for pixel comparisons.
- The fixed environment-variable mode after the rework (`FOV=1792x1007`,
  `earth-fixed3`) still maps every pass.

## Vulkan games (2026-11-13)

The gaze profile now also reaches KosmicKrisp from wineopenxr's direct Vulkan sessions
([vulkan-bridge.md](vulkan-bridge.md)): on Talos the bridge found the driver, sent
896×1007 and the held profile, and the driver received them. Nothing was foveated,
because neither game has a target this prototype selects:

- **Talos** renders each eye single-sampled into a 16-bit HDR colour target and a depth
  target sized to the larger of the window and the eye (1920×1080 on the simulator,
  2800×2856 in the headset), then bloom, tone-mapping, and a copy into per-eye 8-bit
  images that are copied into the swapchain. `+gfx_iAntiAliasingSamples 4` is accepted
  but the targets stay single-sampled. It needs single-sample selection with a
  stay-warped boundary: map the eye-sized HDR and depth targets, translate the
  post-process reads (already done for sampled reads), and unwarp before the first
  copy or blit. Copies and blits of a warped image are the missing piece.
- **NMS** uses `VK_KHR_fragment_shading_rate` (a shading-rate image from its
  `DrawFoveationMap` compute shader), not fragment density maps. KosmicKrisp does not
  offer the extension and Metal has no variable-rate shading, so its own foveation
  cannot be switched on. Emulating it would still need the same warping and
  unwarping as implicit foveation; its rate image would only replace the bridge's
  profile. Its scene targets have not been traced yet: the main menu is a flat panel.

## Single-sample targets (2026-11-13, Talos)

`KK_IMPLICIT_SINGLE=1` (set for Talos by `scripts/steam-play-game-settings.zsh`) makes
eye-sized single-sample attachments candidates too: colour targets that can be sampled
and depth targets, not storage images or arrays, with or without mip levels.
`KK_IMPLICIT_HALF=1` adds targets of exactly half the eye size, each with its own map of
the same profile. They stay warped from pass to pass:

- **Reads** by shaders are translated, as before. For a foveated image the translation
  works in level-0 pixels whatever the view's base level.
- **A pass is foveated whole or not at all.** If any attachment cannot be (another size,
  a mip level, not a candidate), its attachments are never foveated; one that already
  was goes back to ordinary and its translation is switched off (`not foveating ...`).
  Talos's first attempt failed exactly here: a mipped 16-bit depth-derived target was
  left ordinary while the depth buffer it shares a pass with was foveated.
- **Mip levels** are made by blits of the image into itself. Those blits run "raw",
  neither translated nor rate mapped, so every level keeps level 0's normalised layout.
- **Raw copies** (`vkCmdCopyImage`, `vkCmdCopyImageToBuffer`) from a foveated image first
  blit it, translated, into a hidden ordinary image of the same format, then copy that.
  This keeps a copy's bit-for-bit meaning across formats (Talos copies UNORM eye images
  into an sRGB swapchain). Copies between foveated images of one size are copied as
  stored; copies of depth or of lower mip levels are not unwarped (logged).

Talos on the high-resolution simulator (eye 2800×2856), start screen, still head,
gaze held ahead. Its HDR target is sized to the larger of the window and the eye, so on
the 896×1007 simulator it is 1920×1080 and nothing matches; the high-resolution
simulator and the headset match.

| Run | Foveated | GPU ms per frame (median) | Eye image vs baseline |
|---|---|---|---|
| baseline | – | 20.4 | – |
| single, no rate map (`KK_IMPLICIT_NOMAP=1`) | path only | 22.0 | – |
| single, default profile (34% of pixels) | 8 eye-sized | 20.7 | centre 0.5, whole 4.4 (blockier periphery) |
| single + half | + 1 half-size | 19.4 | centre 0.5, whole 4.4 |
| single, profile 0.1,5,10 (6.5% of pixels) | 8 eye-sized | 18.7 | – |
| baseline at half resolution | – | 9.1 | – |

GPU time is from `KK_QUEUE_TIMING=1` between the game's desktop presents. The images are
right: in the periphery the same stones and edges, at a lower rate. The saving on this
screen is small. Even at 6.5% of the pixels the foveated passes save about 3 ms, against
about 1.6 ms of unwarp and translation overhead, while halving the resolution saves
11 ms, so most of the resolution-dependent cost is in passes that are not mapped. Which
ones needs per-pass GPU timing; KosmicKrisp has none yet. The start screen also may not
represent play.

The frame period here is 34–37 ms with about 20 ms of GPU work: the CPU side sets the
rate. The Vulkan bridge waits on the CPU for each frame's GPU work before release
([vulkan-bridge.md](vulkan-bridge.md)), which runs the game's CPU and GPU work back to
back; GPU-side ordering would likely matter more for Talos than foveation.

## Per-pass GPU timing (2026-11-13)

`KK_PASS_TIMING=1` (`patches/kosmickrisp/0004-pass-timing.patch`, diagnostic) writes a
precise GPU timestamp at the start of each Metal command buffer and the end of every
encoder, resolves them into a buffer at the end of the command buffer, and every five
seconds logs the time between stamps summed by pass shape (target size, colour and
depth formats, samples, colour count, rate mapped or not) per presented frame. A pass's
time is from the previous stamp, so a stall is charged to the pass that ends next (the
0.8 ms on a tiny target below appears in both runs).

Talos, high-resolution simulator, start screen, GPU-ordered release (above), ms per
frame:

| Passes | Unfoveated | Foveated (single + half, held, 34% of pixels) |
|---|---|---|
| HDR scene, eye size, with depth (5 per frame) | 5.9 | 4.3 |
| 8-bit post-processing, eye size (4) | 2.7 | 1.9 |
| half-size HDR (2) | 2.5 | 1.1 |
| depth pre-pass into the mipped 16-bit target (2) | 1.6 | 1.2 |
| unwarp blits before copies (2, new) | – | 0.7 |
| copies and other transfers | 0.2 | 0.4 |
| everything else (bloom chain, mirror, small passes) | 2.6 | 3.0 |
| **total between stamps** | **15.5** | **12.6** |

The frame-level measure agrees: 17.5 → 14.1 ms of GPU work per frame. The scene passes
fall by only 27% while drawing 34% of the pixels, so they carry fixed work that a rate
map does not reduce (geometry, five passes per frame). The unwarp blits and extra copies
cost about 0.9 ms; rendering the last post-processing target ordinary instead of
unwarping it afterwards would trade that for one full-rate pass. A narrower profile
(the headset's real gaze) and play rather than the start screen are the next
measurements.

## In play (2026-11-13)

**Talos, Cloud_1_01** (started directly, still head, gaze held ahead, high-resolution
simulator, GPU-ordered release):

| | GPU ms per frame | Frames per second |
|---|---|---|
| unfoveated | 21.3 | 40 |
| single + half, default profile | 15.8 | 51 |

| Passes (ms per frame) | Unfoveated | Foveated |
|---|---|---|
| HDR scene, eye size (4) | 7.5 | 3.6 |
| 8-bit post-processing, eye size (4) | 3.2 | 2.4 |
| half-size HDR (2) | 2.4 | 1.0 |
| depth pre-pass (2) | 2.1 | 1.8 |
| 256×256 float target (2, not foveated) | 2.2 | 1.2 |
| copies and transfers | 2.0 | 2.3 |
| unwarp before copies (2) | – | 0.7 |

In play the scene pass halves. The image is right; in the periphery a high-contrast
mosaic floor loses its thin dark grout at the outer rate and reads lighter (bottom
corners differ by 38–52 mean levels from the baseline, the centre by 12, partly
animation).

**No Man's Sky** (in play on a planet, qwerty headset at 2800×2856 per eye):
44 ms of GPU per frame (about 22 fps). One G-buffer pass into a 5600×2856 target (both
eyes side by side, three colour attachments and depth) takes 27 ms; 44 compute
dispatches take 10.7 ms; 38 small passes into 1920×1080 RGBA8 targets 1.9 ms. Nothing
is foveated: its eye-sized colour and depth targets have storage usage (its compute
passes read and write them), and the prototype leaves storage images alone. NMS's own
foveation uses `VK_KHR_fragment_shading_rate`, which KosmicKrisp lacks.

### Storage images and scaled targets (2026-11-13)

- **`KK_IMPLICIT_STORAGE=1`**: storage-usage targets can be candidates. The storage
  descriptor grew from 8 to 32 bytes (resource ID, rate parameters, level-0 size, base
  layer; zero for ordinary images and texel buffers). Integer coordinates of
  `imageLoad`, `imageStore` and atomics on a foveated image go through its rate map,
  screen to physical, behind the same run-time `valid` flag as sampled reads; several
  screen texels may store to one physical texel. Compute work still runs per screen
  texel (but see "Compute at the foveated size" below).
- **`KK_IMPLICIT_SCALED=1`**: targets that are the eye layout scaled down uniformly
  (40–100%, the two scales within 0.8%, not power-of-two sizes) are candidates too,
  each with its own map of the profile. NMS renders its scene at 3296×1696 (about 59%
  of 5600×2856) and upscales. A first, looser rule (30%, within 2%) also took
  1024×1024, 2048×2048 and 2048×1024 textures, because the eye is nearly square.
- **Cache identity**: the implicit-foveation switches and `MESA_KK_EXPERIMENTAL` now
  feed `pipelineCacheUUID`, so a pipeline cache an application saved in one mode is
  not reused in another (KosmicKrisp itself has no disk cache).
- `scripts/steam-play-game-settings.zsh` sets single, scaled and storage for NMS;
  they only act with `MWXR_FOVEATION`.

NMS in play at 2800×2856 per eye (new game each run, so a different random planet;
`NMS_FRESH=1` in `vk-game.zsh` moves the test prefix's save aside, kept under
`game-checks/nms-saves/`): about 56–60 ms of GPU per frame unfoveated, 42–46 ms
foveated with held gaze. Foveated: the G-buffer pass at 5600×2856 (three colour
attachments) 12.4 → 4.8 ms, the scaled scene pass (seven colour attachments) about
13.5 → 6.9 ms, both measured on different planets. The images are coherent in both
eyes (scene, hands, HUD and VR panels in place); the periphery shows stepped edges at
the outer rate. Compute and transfers (about 12–14 ms) are unchanged.

A later Talos recheck ran while the Mac was busier (unfoveated 27 ms, not 21.3): the
foveated run gave 18.6 ms there, the same proportion, and the same image.

### Compute at the foveated size (2026-11-13)

`KK_IMPLICIT_COMPACT=1` (with `KK_IMPLICIT_STORAGE`; patch
`0005-compact-compute-dispatch.patch`) runs some compute dispatches over a foveated
target at its physical size, one invocation per physical texel, rather than one per
screen texel.

- **Which shaders.** At compile time a compute shader qualifies if its invocations are
  independent and it writes only 2D storage images, at most four:
  - none of these: shared memory, workgroup barriers, subgroup or quad operations,
    derivatives or implicit-LOD sampling;
  - no buffer stores or buffer atomics.
  
  Image atomics count as writes that read. A qualifying shader gets a prologue: when
  the root table's `cs.compact_params` is set, each invocation takes its physical
  texel's centre through the map's physical-to-screen mapping and replaces the
  workgroup, local, global and index IDs with the ones that screen texel's invocation
  would have had. Invocations past the physical size repeat the last texel. Its loads
  and stores at that screen texel translate back to the same physical texel.
- **Which dispatches.** A direct dispatch with no base group or conditional rendering
  is compacted only if all of these hold:
  - every image it writes is foveated now: the descriptor's rate parameters belong to
    an image whose current map is the latest one;
  - all written images share one map;
  - the grid covers exactly the map's screen size;
  - the shader reads none of the images it writes.
  
  The group counts then shrink to the physical size, and the shader sees the
  application's counts.
- **Compute-only targets.** NMS writes several eye-scaled images only from compute, so
  no render pass ever gave them a map. A qualifying dispatch that rewrites such an image
  whole without reading it now gives it the profile's map, as a pass that does not load
  its target would. Every reader already translates through the image's rate
  parameters.
- **Not compacted: shaders that read what they write.** NMS's largest compute pass
  (about 13–15 ms per dispatch at 3296×1696) writes four eye images and also loads
  and atomically updates them. It ran 15.2 → 5.7 ms compacted, but in the same session
  the image showed ghosting (ship fin and hands semi-transparent) and banded clouds and
  aurora. Such shaders are refused (`KK_IMPLICIT_COMPACT_READS=1` compacts them anyway,
  for diagnosis).

NMS at 2800×2856 per eye, same session and scene, before and after switching mode
(per dispatch):

| Shader | Full size | Compacted (2008×1000 of 3296×1696) |
|---|---|---|
| A | 1.33 ms | 0.71 ms |
| B | 0.56 ms | 0.44 ms |
| C | 0.46 ms | 0.31 ms |

That is about 0.9 ms per frame, too small to show through frame-to-frame variation of
40–55 ms in total. The images in daylight and at night match full-size dispatch: clouds,
ship, hands and HUD. The remaining big compute passes are the read-write one above and
an upscale that writes the 5600×2856 target from a 3296×1696 grid (2.4–3.4 ms), which
compaction does not fit.

Diagnostics:
- `KK_PASS_TIMING_DISPATCH=1` (with `KK_PASS_TIMING`) stamps every dispatch, labelled by
  shader, grid and compaction outcome (`COMPACT`, `eligible`, or the compile-time reason
  it is not).
- `KK_IMPLICIT_COMPACT_LOG=1` logs each shader's dispatch decision once.
- `KK_IMPLICIT_COMPACT_FILE=path` switches modes while the application runs: `off`, `map`
  (map compute-only targets, dispatch at full size) or `compact`. The A/B runs above
  used it.
- `MWXR_CAPTURE_MAX` raises the bridge's capture count from 6.

`scripts/steam-play-game-settings.zsh` turns compaction on for NMS.

### Light barriers, quarter-size targets and the read-write pass (2026-11-13)

Patch `0006-light-barrier-fixes-and-runtime-switches.patch`. Comparisons across runs of
NMS are unreliable: each new game lands on a random planet, and the day passes during
a run. `KK_IMPLICIT_COMPACT_FILE` now also switches light barriers (`lb`/`nolb`) and
25% scaled targets (`quarter`) at run time, polled at presents, so these were measured
alternating within one session.

- **Light barriers** (`MESA_KK_EXPERIMENTAL=…,light_barriers`): KosmicKrisp then orders
  encoders by the application's barriers rather than waiting for all earlier work at
  every encoder end. As shipped it broke NMS: banded clouds, a hard-edged gap in the sky,
  see-through plants and HUD. Two gaps:
  - a barrier recorded while a compute encoder was open only ordered work inside that
    encoder, and render encoders start with no wait of their own, so a later pass could
    read compute results early. Fixed: such barriers also carry to the next encoder.
  - the driver's own writes of rate parameters in a compute encoder (published maps,
    cleared maps, the maps compaction gives compute-only targets) had the same gap.
    Fixed the same way.
  
  With both fixes the images are clean. Alternating every 15–20 s in one session:
  - Talos: 16.15 → 15.54 ms of GPU per frame, about 4%, consistent in every pair.
  - NMS: 51.9 → 51.9 ms; 46.4 → 47.4 frames per 5 s, within noise. Its GPU is
    saturated, so overlapping passes buys nothing.
  
  It stays off by default: it's experimental, and the gain is small.
- **Quarter-size targets** (scaled targets from 25%, which takes in NMS's 1648×848
  effects): those passes went 1.94 → 1.41 ms per frame, but two runs showed a hard
  horizontal lighting edge across the eye, sunlit above and dark below. It also matched
  an 824×848 image as one eye, which is suspect. The default stays at 40%; `quarter` in
  the control file brings 25% back for debugging. Images already judged not eye-sized
  stay ordinary, so it must be set from the start.
- **NMS's read-write compute pass** (about 13–15 ms per dispatch). The compile-time
  pass now records the access pattern: it writes four eye images and reads other
  texels of them as well as its own, with seven texture samples. It filters or
  accumulates in place across neighbours. One invocation per foveated texel changes
  what its neighbours see, which is what produced the ghosting, so compaction is not
  safe for it. The levers left there are the game's own cloud, volumetric and
  render-scale settings.

### What NMS's own VRS marks (2026-11-13)

NMS has its own foveation through `VK_KHR_fragment_shading_rate`. Its binary names
per-target shading-rate images at full, render-scale and half render-scale sizes
(`HmdVRSTexture*`, `HmdDRSVRSTexture*`, `HmdHalfDRSVRSTexture*`), compute shaders that
generate fixed and eye-tracked rate images, and AMD FidelityFX Variable Shading.
KosmicKrisp does not offer the extension, so this path never ran.

`KK_FAKE_VRS=1` (patch `0007-fake-vrs-pass-logging.patch`) advertises attachment and
pipeline shading rates, rates 1×1 to 4×4 with 8×8–32×32 tiles, and ignores the rates:
every fragment is still shaded. Every 10 s it logs each pass shape per frame with what
it asked for. `KK_VRS_CAPTURE_DIR` saves the rate images it binds, as PGM plus raw
bytes. NMS then used it at once (its `VRFoveatedRendering` setting is on); the run used
none of our foveation, so this is the game's own choice.

| Target | Passes the game shades coarsely | Passes it leaves at full rate |
|---|---|---|
| 5600×2856 (eyes) | one colour + depth pass | the three-attachment pass (c122 ×3, 12.4 ms unfoveated), the other colour passes |
| 3296×1696 (59% render scale) | the seven-attachment G-buffer, a three-attachment pass, one single-colour pass without depth | eleven other pass shapes (single colour with depth, two and six attachments) |
| 1648×848 (half render scale) | five pass shapes (c97 ×2 three times a frame, c97 ×1, c122 and c97 with depth) | ten others |
| shadows, UI, small effects | none | all |

What this says about our choices:

- **Same core pass.** The G-buffer is the game's main coarse-shaded pass, and ours.
- **We go further.** We also foveate the full-size three-attachment pass and every other
  pass at the eye and render-scale sizes. Those images are coherent, but the game's
  developers chose full rate there. The full-size three-attachment pass is our largest
  single saving, so if anything looks wrong in the headset, look there first.
- **Quarter size is meant to work.** The game coarse-shades several 1648×848 passes, so
  the lighting edge seen with 25% scaled targets is our bug, not an unsafe pass.
- **Similar strength, different shape.** Inside its render area the game's rate image
  has a full-rate circle per eye, about 62% of the eye's width across, then a 2×2 ring
  and 4×4 corners: 29% full rate, 36% 2×2, 35% 4×4, about 40% of full shading work.
  Ours leaves 2008×1000 of 3296×1696 physical texels, 36%. Its half-size image is 65%
  full rate and 35% 2×2, about 74% of the work.
- **Not covered.** VRS only coarsens shading inside render passes and keeps every texel
  stored, so it says nothing about compute (the 13–15 ms read-write pass) or about
  readers of a changed layout.

The rate images only fill the render-scale area (412×212 tiles of 700×357; the rest is
full rate), and the game also binds the render-scale image to that full-size pass.
Captures stay in `build-in-process/game-checks/vrs-cap1` (game output, not committed).

### Half precision and the quarter-size edge (2026-11-13)

Patch `0008-relaxed-count-and-size-skip.patch`.

- **Half precision.** Mesa's SPIR-V reader can turn `RelaxedPrecision` into 16-bit
  arithmetic (`mediump_16bit_alu`), which KosmicKrisp does not set. `KK_COUNT_RELAXED=1`
  counts the decorations as modules are created: NMS has 17 in about 1.5 million
  arithmetic and conversion instructions across 7,200 modules. It asks for full
  precision throughout, so this would gain nothing there.
- **The quarter-size edge comes from compaction.** With 25% scaled targets, two compute
  shaders (16×16 workgroups) write the 1648×848 images compacted to 1055×512. The edge
  sits at row 512 of 848: below it the image is not rewritten. With the images mapped
  but those dispatches at full size (`map quarter`), the edge is gone and the image is
  clean, so the quarter-size targets themselves are safe, as the game's own VRS
  suggests. Why the remap misses rows past the physical height for those two shaders
  is not yet known. `KK_IMPLICIT_SKIP=WxH,...` excludes sizes from foveation;
  excluding the 824×848 image did not help.

### First headset runs (2026-11-13)

NMS at standard graphics settings, run A without foveation and B with
`MWXR_FOVEATION=gaze`. Wall-clock rates come from KosmicKrisp's present count, the
game's desktop mirror:
- **Run A:** about 6.6 frames/s at about 85 ms of GPU per frame.
- **Run B:** about 9 frames/s at about 57 ms.

The GPU was busy only about half the time in both, so frame rate is held down by
more than GPU work, probably CPU time (the game under Rosetta and Wine) or how the
frame is synchronised.

- **Rectangles.** In B, both eyes went dark outside a rectangle 61% wide and 59% high
  of the side-by-side render-scale target: exactly the 2008×1000 physical size of the
  3296×1696 target. Compacted compute passes left everything outside it unwritten,
  the same symptom as the quarter-size edge in the simulator. NMS's settings now leave
  compaction off (`KK_IMPLICIT_COMPACT=0`) until it is understood.
- **No gaze.** The bridge reported gaze `valid=0` throughout and 3,145
  `XR_ERROR_HANDLE_INVALID` from `xrUpdateSwapchainFB` with the foveation profile, so B
  used the fixed centre, not eye tracking. Cause:
  - NMS (through xrizer) destroys its first session and creates a second, and the
    wrapper can be allocated at the same address.
  - The bridge kept its gaze state, including the eye-tracked foveation profile, keyed
    by that pointer, so it went on naming the first session's profile, which the runtime
    had destroyed with that session.
  
  Fixed in `graphics_native.m`: the gaze state is dropped (and its profile destroyed)
  when its session is, and is also keyed by the runtime session handle. Not yet
  verified in the headset.
- **Controller prompts.** NMS's action manifest has PS VR2 Sense bindings, which
  xrizer ignores ("unknown profile playstation_vr2_sense"). It presents Oculus Touch
  instead, and the game's prompts show warning triangles.

### Spectator window (2026-11-14)

NMS presents its desktop (spectator) window through KosmicKrisp every frame. With FIFO,
each present can wait for the Mac display's refresh and hold up the VR frame. Measured
on the same save in the simulator with a fixed map:
- FIFO: about 12.6 frames/s and 40 ms of GPU per frame.
- Immediate: about 15.2 frames/s and 35 ms.

That is 20% more frames. `scripts/steam-play-game-settings.zsh` now sets
`MESA_VK_WSI_PRESENT_MODE=immediate` for NMS and Talos (the window may tear);
`MWXR_SPECTATOR_VSYNC=1` in the Launch Options keeps FIFO. NMS has no setting to turn
the window off.

### A moving gaze breaks the single-sample route (2026-11-14)

With gaze now reaching KosmicKrisp in the headset, NMS showed hard-edged rectangles and
stretched content that moved with the eyes, with compaction off. The simulator reproduces
it with the synthetic sweeping gaze (`BRIDGE=test`); with a held gaze it does not. Two
faults, both invisible while the map never changes:

- **Parameters updated out of GPU order.** Each foveated image has one rate-parameter
  buffer. When a pass draws it with a new map and no compute encoder is open,
  `kk_fdm_publish` overwrites that buffer from the CPU when the pass is recorded. Work
  recorded earlier but not yet run then reads the image's old layout through the new
  map.
- **Loads across a map change.** An image drawn under one map and later loaded by a pass
  under another (within a frame, or from the previous frame) has its old content drawn
  over in the new layout.

Latching new profiles only at frame boundaries (a present, or the bridge's
`kk_implicit_fdm_frame_boundary` when the eye image is released) is in place, but
does not fix either. A real fix needs:
- every image to keep the map it was drawn with, updated in GPU order;
- a pass that loads an image drawn under another map to use that map, or to convert
  the image first.

Until then `MWXR_FOVEATION=fixed` is the working mode for Vulkan games (patch
`0009-frame-boundary-latch.patch` has the frame-boundary latch).

**A third fault at 50% render scale, with a fixed map.** At NMS's Standard graphics
settings the scene renders at 2800×1428 (50%), not 3296×1696 (59%). There, with a fixed
map and compaction off, one eye and 22% of the other are sunlit only inside a rectangle
61% wide and 59% high: the physical size of the 2800×1428 map in side-by-side layout.
Everything outside it has only ambient light. With `KK_IMPLICIT_SKIP=2800x1428` (a new
game, so another planet) the image is clean. No raw ordinary-to-foveated copies were
logged. Not yet found. At the default settings (59%) the runs were clean.

### Moving gaze and the 50% fault fixed (2026-11-14)

Patch `0010-per-image-maps-and-gather.patch`.

- **Per-image maps in GPU order.**
  - Each map's parameters (`struct kk_fdm_params`) now keep their Metal rate map, and
    each foveated image references the parameters its contents are laid out in
    (`rate_ref`).
  - New parameters reach an image's buffer only by a GPU copy (ending an open render
    encoder if need be), so earlier work still reads the old layout through the old
    parameters. Unchanged parameters are still rewritten from the CPU: the same bytes.
  - A pass that loads an attachment drawn under an older map renders in that map; the
    image takes the new map when a pass next rewrites it without loading. A pass
    loading attachments under two different maps logs a warning (seen once per run).
- **Gathers translated.** `textureGather` (`tg4`) takes normalised coordinates like
  sampling but was not translated. At NMS's Standard settings the scene renders at
  full eye size, and its half-resolution effects at 2800×1428 (50%) are foveated. Its
  full-resolution lighting gathers a half-resolution 8-bit shadow mask, so it read the
  mask's physical layout, sunlit only inside a rectangle of the physical size.
  - Found by excluding images by size and format (`KK_IMPLICIT_SKIP=WxH:FORMAT`).
  - At the default settings those effects are 1648×848 (29%), below the 40% rule, so
    the fault never showed there.
  - Depth-compare sampling is still not translated; it is normally used on shadow
    maps, which are never foveated.

Check: a fresh game at Standard settings with nothing excluded and the synthetic
sweeping gaze. Both eyes are clean: no stretching, no rectangles, lighting continuous.

## Next

1. NMS in the headset with real gaze (`MWXR_FOVEATION=gaze`) at Standard and higher
   settings.
2. NMS's own graphics settings against its read-write compute pass (clouds or
   volumetrics, from the look of it).
3. Find why compaction of NMS's two 16×16 compute shaders on its 25% targets leaves
   rows past the physical height unwritten; then turn the 25% targets on (about
   0.5 ms; clean without compaction).
4. Optionally limit NMS to the passes the game coarse-shades, as a quality fallback if
   the extra passes look wrong in the headset.
5. Avoid the unwarp blits where the last pass before a copy can render ordinary.
6. Decide whether the push-descriptor and occlusion-query fixes go into the
   `kk-zink` branch on their own.

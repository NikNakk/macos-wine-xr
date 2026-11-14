# Foveated rendering for D3D11 games (opt-in)

2026-11-12. Experimental and off by default. The design and measurements are in
[gaze-directed stay-warped foveation](dxmt-gaze-foveation.md) and the documents
it links.

## Turning it on

Set `MWXR_FOVEATION` for a launch, from the environment or the game's Steam
Launch Options (`MWXR_FOVEATION=gaze %command%`):

| Value | Effect |
|---|---|
| unset, `off` | Ordinary rendering; the normal Wine runtime and wineopenxr build. |
| `fixed` | Full rate within 20° of straight ahead, falling to the peripheral rate at 35°. |
| `gaze` | The full-rate region follows the runtime's gaze; falls back to `fixed` while gaze is lost. |
| `gaze:OUTER[,CORE,EDGE[,STEP]]` | Peripheral rate per axis (0.1–1, default 0.3), core and edge angles in degrees (default 12, 27) and gaze quantisation (default 2°). `fixed:OUTER` sets only the rate. |

An invalid value stops the launch. `scripts/openxr-launch-settings.zsh` checks
it and sets the DXMT and wineopenxr variables;
`scripts/steam-play-game-settings.zsh` selects the runtime.

## What it uses

- **Wine runtime**: `build-in-process/wine-foveation-unified`, a copy of the unified
  WoW64 runtime (`runtime-unified`, [steam-play.md](steam-play.md#wow64)) with DXMT
  built from patches 0001–0025 in `build-in-process/dxmt-warp-source`
  (`dxmt-warp-build`; the 0022 files are kept in `build-in-process/dxmt-0022-backup`,
  the files before 0025 beside the new ones as `*.pre-0025`). It replaced
  `wine-foveation` and `wine-foveation-wow64` on 2026-11-14. It carries
  `share/mwxr-foveation-dxmt`; a launch with `MWXR_FOVEATION` refuses a runtime without
  it. The ordinary runtimes are unchanged. `MWXR_IN_PROCESS_WINE` still overrides the
  choice.
- **wineopenxr**: `build-in-process/gaze-gate` (`MWXR_FOVEATION_BUILD` overrides);
  `vk-fov-gate` for the Vulkan games. Both rebuilt from current source on 2026-11-14
  (several composition layers per frame, for UEVR); the previous builds are
  `*.before-uevr-20261114`.
- **Monado**: `gaze` needs `XR_FB_foveation`, `XR_FB_swapchain_update_state`,
  `XR_FB_foveation_configuration` and `XR_META_foveation_eye_tracked`. The
  installed service and x86_64 client have had them since v25.1.0-2500
  (2026-11-12); the previous install is kept in
  `~/Code/alyx-native/build/monado-installed-backup-20261112-v2491`.
- **Shader cache**: the stay-warped translation has its own shader flags, so
  DXMT's on-disk cache keeps foveated and ordinary variants apart.

## Which games it can affect

DXMT maps a render pass when its target has exactly the size of the map and is
not the swapchain image itself. The swapchain image always ends up ordinary, so
the compositor is unchanged. The map is sized to:

- the texture the game copies into the swapchain, when it copies one (OpenVR
  games through xrizer). DXMT leaves each copy's source size and box on the
  swapchain image and wineopenxr reads them at the next acquire. This covers
  one texture per eye, both eyes in one texture, a render scale, and part of a
  larger texture under dynamic resolution;
- otherwise the swapchain, with the rectangles the game last submitted (EXD).

| How the game presents | Result |
|---|---|
| Draws into the swapchain with a shader (EXD, UE 4.27 OpenXR) | Foveated. |
| Copies eye textures into the swapchain (xrizer): per-eye or double-wide, any size | Foveated; the copy is resampled (patch 0022). Per-eye textures share one map, the higher of the two eyes' rates. |
| Dynamic resolution inside a fixed texture (Alyx adaptive fidelity) | Foveated; one map per viewport size, cached. |
| Two-layer (single-pass instanced) eye targets, multisampled: Unity's OpenXR plugin (Hyperbolica, RAGER) | Foveated (patch 0023); one map shared by both layers. |
| Multisampled texture handed straight to the swapchain | Not foveated: the layer ignores a multisampled source. |
| Depth submitted to the compositor | Colour only; a warped depth source would be copied unresampled, with a warning. |
| Vulkan, OpenGL or D3D12 games | Not affected. |

A pass that reads targets in two different layouts is left untranslated and
logged (`second layout in one pass`); that would show as a distorted effect.
The first frames, before the first copy, use the swapchain's size.

## Checked so far

See the table in [gaze-directed stay-warped foveation](dxmt-gaze-foveation.md#other-games).

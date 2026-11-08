# EXD performance investigation, 2026-11-08

EXD – Extra Dimensional, Steam app 3962020, Unreal Engine 4.27. Investigation
of the existing launches; no new game benchmark has been run yet.

## Evidence

- Steam passed the selected DX11/OpenXR launch arguments in all three runs:
  `-dx11 -hmd=OpenXRHMD -EXDOpenXROnly -DisablePlugins=OculusOpenXR`.
  There is no SteamVR compositor to remove from this route.
- The 23:57 run also received `-ExecCmds=vr.PixelDensity 0.7`; the 00:01 run
  received `-vrscale 70 -windowed -resX=64 -resY=64`. Argument receipt does
  not establish that the engine applied them. The logs contain no reliable
  eye render dimensions or measured FPS.
- Steam's captured environment for the last run contains
  `XRT_MACOS_CLIENT_COMPOSITOR=1`. The game log confirms a hosted compositor
  inside Wine, using MoltenVK, presenting through the service's PS VR2 window
  at 4000×2040 and 90 Hz. That is the headset output size, not eye render size.
- The game logs contain 7,034 / 2,526 / 10,430 occurrences of
  `vk_cmd_submit_locked failed: VK_NOT_READY`, respectively, followed by
  `renderer_submit_queue failed: VK_NOT_READY`. This is a strong reason to
  compare compositor paths, but does not quantify lost game frames.
- Current Monado source's swapchain GPU reuse submission hook returns
  `VK_NOT_READY` when its zero-time producer semaphore readiness check fails.
  This suggests a producer-readiness/presentation issue to investigate; the
  exact deployed binary behavior has not been verified against that source.
- The Wine process loads two different MoltenVK dylibs (the bridge dependency
  and Wine's runtime copy), with an Objective-C duplicate-class warning.
  Switching to service composition also removes the hosted MoltenVK compositor
  from the game process; the warning alone does not establish a slowdown.
- Saved scalability is mixed: shadows/postprocess/textures/effects at 1,
  view distance/AA/foliage/shading at 3, resolution quality at 100. EXD also
  stores custom graphics choices in `Saved/SaveGames/GameSettings.sav`, so the
  INI alone is not authoritative. Do not edit the binary save speculatively.

Evidence: `~/Library/Logs/macos-wine-xr/3962020-{20261107-235119,20261107-235733,20261108-000137}/game.log`,
`steam-play.log`, and `steam-play-run/20261108-000129-3962020/env.txt`.

## Controlled next session

The launch script now accepts two Steam Play `MWXR_*` options. They are
translated to Monado environment settings before Wine starts and logged in
`game.log`. The existing rig wrapper calls the repository script directly,
so no Steam Play reinstall is needed. No global settings have been changed.

Keep Steam's DX11/OpenXR launch choice. Replace the existing custom Launch
Options with the first line below for a comparison at the original runtime
recommendation, then the second for lower resolution:

```text
MWXR_CLIENT_COMPOSITOR=0 MWXR_VIEWPORT_SCALE_PERCENTAGE=100
MWXR_CLIENT_COMPOSITOR=0 MWXR_VIEWPORT_SCALE_PERCENTAGE=70
```

`MWXR_CLIENT_COMPOSITOR=0` overrides Steam's inherited setting for this game
launch and uses the service compositor. The scale option sets
`OXR_VIEWPORT_SCALE_PERCENTAGE`: 70% in each dimension recommends about 49%
as many pixels. Games can override that recommendation, so confirm actual
swapchain/render dimensions in a new trace. Both controls are optional and
affect only the launch; removing them restores the inherited behavior.

Compare the same loaded scene after shader warmup. Record game submission
cadence and actual headset presentation separately; 90 Hz gives an 11.1 ms
budget. Check submission errors in both modes. If service composition fixes
cadence, isolate the hosted producer-readiness problem before optimizing GPU
work. If reducing resolution improves cadence, profile GPU work next.

For a lower-quality GPU diagnostic, use EXD's graphics menu to disable MSAA
and set shadows, effects, postprocessing and volumetric fog Low, then restore
quality one setting at a time. The developer's November 3 notes say Low
volumetric fog disables its volumetric component. FSR2 is a possible later
comparison; its Wine/DXMT compatibility and benefit here are unmeasured.
Advanced DLSS foveation is tied to DLSS and is not an Apple GPU solution.
See the [developer's update notes](https://steamcommunity.com/app/3962020/allnews/).

Validation: `python3 tests/macos/test_openxr_launch_settings.py` checks inherited
settings, launch overrides, logging, and rejecting invalid values. Shell syntax
and `git diff --check` pass. A headset comparison is still required.

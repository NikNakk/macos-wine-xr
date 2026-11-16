# EXD performance investigation, 2026-11-08

## Two wearer-reported launch failures, 2026-11-16

The user reports the first failure before the menu and the second while loading.
These are different failure paths, not evidence of one repeated crash:

- **00:12:03, SteamVR route:** Steam passed `-dx11 -vr -hmd=SteamVR`
  and 50% viewport scale. The 00:12:07 Unreal crash context records
  `0xc06d007e` (delay-loaded module not found). The minidump's exception
  parameter points to a delay-load record with error 126; its DLL-name
  address, mapped into the shipping executable, identifies `openvr_api.dll`.
  That DLL is absent beside `EXD-Win64-Shipping.exe`. This run did not reach
  the OpenXR session shown in the second run.
- **00:12:36, OpenXR route:** Steam passed
  `-dx11 -hmd=OpenXRHMD -EXDOpenXROnly -DisablePlugins=OculusOpenXR`
  and `MWXR_FOVEATION=gaze`, with runtime-default viewport scale (not the
  first run's 50%). Monado connected EXD PID 89556 and imported four
  5600×2856 Metal images. The service then reports
  `Semaphore waiting failed: value=3818 result=2` and
  `Client GPU obligation failed: frame=1814 value=3818 result=2`.
  Result 2 is the timeout path; current source defaults to a 5 s producer
  obligation deadline. The 00:13:37 Unreal crash context, 58 s after startup,
  records `EXD OpenXR acquire failed ... XR_ERROR_SESSION_LOST`.
  This is session loss following an unfinished GPU obligation. The producer
  stall's cause, and whether foveation caused it, remain unproven.

Evidence: `~/Library/Logs/macos-wine-xr/3962020-20261116-{001203,001236}/game.log`,
`steam-play.log`, `~/Library/Logs/Monado/monado-service.err.log` (EXD PID 89556),
and the playing prefix's `EXD/Saved/Crashes/UE4CC-Windows-{AC77383E4352AC8B9C2BC9B3F9689027,67DF6A3E4CD0CE1E94EC428770F4D367}_0000/`.
Analysis checkouts: Monado `b6758b7d6`, bridge `a24c57e`; these identify the
analysis sources, not a verified deployed binary revision.

Next wearer-operated control: retain Steam's DX11/OpenXR launch choice and set
`MWXR_FOVEATION=off MWXR_CLIENT_COMPOSITOR=0 MWXR_VIEWPORT_SCALE_PERCENTAGE=50`.
If loading succeeds, compare gaze at the same 50% scale before testing full
resolution. Preserve the GPU-loss guard; increasing its timeout would not
identify why the producer stopped signalling. No launch settings or runtime
binaries were changed during this investigation.

EXD – Extra Dimensional, Steam app 3962020, Unreal Engine 4.27. Investigation
of the existing launches. Subsequent private simulated-headset runs are documented
in the [DXMT TBDR audit](dxmt-tbdr-audit.md), including separate initial and loaded
scene windows; the earlier launch-log observations below are historical.
PS VR2 hardware measurements of MSAA, DLSS→MetalFX, quality and resolution are in
[EXD on PS VR2](exd-headset-performance.md).

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
Advanced DLSS foveation may use DXMT’s experimental NGX-to-MetalFX bridge.
The private integration now has an opt-in, tested per-eye subrect fix; EXD
headset quality and GPU savings remain unmeasured. See
[the current DXMT investigation](dxmt-base-render-foveation.md).
See the [developer's update notes](https://steamcommunity.com/app/3962020/allnews/).

Validation: `python3 tests/macos/test_openxr_launch_settings.py` checks inherited
settings, launch overrides, logging, and rejecting invalid values. Shell syntax
and `git diff --check` pass. A headset comparison is still required.

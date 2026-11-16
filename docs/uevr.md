# UEVR

[UEVR](https://github.com/praydog/UEVR) turns flat Unreal Engine 4/5 games into VR games
by injecting a DLL into the running game. On Windows its frontend (`UEVRInjector.exe`, a
.NET WPF app) does the injection. Here the runner does it instead, with a small Win64
injector, and UEVR's OpenXR goes through the prefix's OpenXR runtime (the in-process
wineopenxr, then Monado), like any other OpenXR game.

## Use

Choose **UEVR: On (OpenXR)** on the game's Compatibility page in Steam (Properties,
Compatibility; [steam-play.md](steam-play.md#macos-wine-xrs-options)), which adds this
to its Launch Options, or add it there yourself (Properties, General):

```
MWXR_UEVR=1
```

Play starts the game as usual. Once its window is up, `mwxr-uevr-inject.exe` waits
`MWXR_UEVR_DELAY` seconds (default 5), then writes `Frontend_RequestedRuntime` into
the game's UEVR config (`~/Windows/uevr/profiles/<exe name>/config.txt`, below) and
loads the runtime DLL and `UEVRBackend.dll` into the game, the frontend's order. UEVR's
menu opens with Insert (or L3+R3 on a pad). Its log is `log.txt` beside the config.
The injector's output goes to `uevr.log` in the launch's log folder.

## Per-game configuration

Two kinds of settings, each kept where it already lives:

- **How UEVR is started** (the table below): the game's Launch Options in Steam, which
  the Compatibility page's UEVR choice edits too.
  A default that every user of a game should get, such as a longer delay, goes in
  `scripts/steam-play-game-settings.zsh` by app ID, with the other per-game quirks.
  A Launch Option still overrides it.
- **UEVR's own settings** (resolution scale, UI and camera placement, input, cvars,
  Lua scripts, UObjectHook state): UEVR saves them itself from its in-game menu, in
  `%APPDATA%\UnrealVRMod\<exe name>`. Community game profiles are zips of that folder.
  Nothing else here keeps a copy.

That folder would normally be inside each game's prefix, so a game moved between the
WoW64 and 64-bit prefixes would start with empty settings. Instead, on a `MWXR_UEVR=1`
launch the runner makes every Wine user's `UnrealVRMod` in the prefix a link to one folder,
`~/Windows/uevr/profiles` (`MWXR_UEVR_PROFILES`). Settings already in a prefix move there
the first time. If the shared folder already has that game, it is kept, and the prefix's
copy goes to `~/Windows/uevr/profile-backups`. UEVR names each game's folder after its
exe, as on Windows.

```sh
python3 tools/steam_play/uevr_profile.py folder 499520            # ~/Windows/uevr/profiles/TheTuringTest
python3 tools/steam_play/uevr_profile.py import 499520 profile.zip # a community profile
python3 tools/steam_play/uevr_profile.py list                      # games with settings
python3 tools/steam_play/uevr_profile.py link PREFIX               # what the runner does at launch
```

`folder` and `import` find the exe from Steam's install (the game's first Windows launch
entry in its macOS Steam library). An Unreal bootstrap exe at the top of the install
folder is replaced by the single `*/Binaries/Win64/*.exe` it starts. `--exe NAME`
overrides that, as `MWXR_UEVR_EXE` does for the launch. `import` accepts a zip whose
profile is at the top or in a subfolder (the first folder holding `config.txt`, cvars,
`scripts` or `plugins`), refuses entries that would land outside the folder, and moves an
existing profile to `profile-backups` first. The runtime line in an imported
`config.txt` is replaced at the next injection by `MWXR_UEVR_RUNTIME`'s.
Tests: `python3 tests/macos/test_uevr_profile.py`.

| Setting | Effect |
| --- | --- |
| `MWXR_UEVR=1` | inject UEVR after the game's window appears |
| `MWXR_UEVR_RUNTIME=openvr` | UEVR's OpenVR path (through xrizer) instead of OpenXR |
| `MWXR_UEVR_DELAY=N` | seconds between the window appearing and the injection |
| `MWXR_UEVR_EXE=Name.exe` | the process to inject into, when Steam starts a bootstrap exe that starts another |
| `MWXR_UEVR_NULLIFY=1` | also load `UEVRPluginNullifier.dll` (the frontend's "Nullify VR plugins") |
| `MWXR_UEVR_DIR` | the UEVR folder (default `~/Windows/uevr/current`) |

Setup:

```sh
scripts/build-uevr-injector.zsh            # build-in-process/uevr/mwxr-uevr-inject.exe
# UEVR itself: a release zip from github.com/praydog/UEVR-nightly, unpacked into
# ~/Windows/uevr/<build>/x, with ~/Windows/uevr/current linked to it
```

Installed 2026-11-14: nightly 01143 (`4ee5c6b`), `uevr.zip`, SHA-256
`4cd6932d1d01168178aef70499d40b101ea6f19868cf76099d8e0087ef8dc6d8` (matches
the release's `.sha256`).

## What it needed in the bridge

- **DXMT: a swapchain on the desktop window.** UEVR (like REFramework and most overlay
  hooks) finds `IDXGISwapChain`'s vtable by creating a throwaway device and swapchain
  whose `OutputWindow` is `GetDesktopWindow()`. Windows allows that. Under Wine the
  desktop window belongs to explorer, and DXMT refused it ("cross-process swapchain not
  supported yet"), so UEVR logged `Failed to create D3D11 device` every 10 s and never
  hooked. `CreateSwapChain` (`~/Code/dxmt`, `src/d3d11/d3d11_swapchain.cpp`)
  now builds such a swapchain on a hidden 1×1 window of the calling process. It has the
  same class and vtable as the game's swapchain and is never presented.
- **wineopenxr: more than one composition layer.** `xrEndFrame` took at most one
  projection layer. UEVR sends its projection layer with quad layers (its menu, the
  game's UI), and every frame failed with `XR_ERROR_LAYER_INVALID`. It now passes up to
  16 projection and quad layers, translating each swapchain handle. Gaze foveation and
  the Vulkan bridge follow the frame's first projection layer, as they did when that was
  the only one.
- `MWXR_CAPTURE_DIR`/`MWXR_CAPTURE_EVERY` (swapchain images as PNGs on release) now
  work for D3D11 sessions too, not only Vulkan ones.

## Results, 2026-11-14

The Turing Test (499520; monolithic UE4 `TheTuringTest.exe`), Monado's simulated HMD,
64-bit runtime with the patched DXMT (`MWXR_WOW64=0`, before the runtimes were unified):

- Injection: both DLLs loaded into the game. UEVR hooked D3D11 `Present` and
  `ResizeBuffers` through the stand-in swapchain, found GEngine, `UGameEngine::Tick`
  and `InitializeHMDDevice` by scanning, and set up its fake stereo rendering device.
  Its misses (Slate draw-to-VR target, `t.MaxFPS`) are scans for newer engine versions.
- OpenXR: `XR_KHR_D3D11_enable` session on "Monado: Simulated HMD", recommended
  896×1007 per eye. A 1792×1007 double-wide eye swapchain and four 1710×1107 UI
  swapchains. Session reached FOCUSED. No `xrEndFrame` failures with the layer fix.
- Captured images: the eye swapchain holds the main-menu scene rendered once per eye,
  side by side. A UI swapchain holds UEVR's menu, showing the OpenXR runtime.

After installing both changes in production (the runtimes' `d3d11.dll` and
`build-in-process/gate`, whose previous build is `gate.before-uevr-20261114`), the
Steam Play path with no overrides gave the same result: the game's WoW64 runtime and
`~/Windows/prefixes-wow64/499520` (it has a 32-bit `PrototypeLauncher.exe`), UEVR
hooked through the stand-in swapchain, OpenXR swapchains made, no `xrEndFrame` failures.

Not yet tried: a real headset (tracking, comfort, frame rate), controllers through
UEVR's bindings, the OpenVR path, and starting from Steam's Play button with the
Launch Option (the runner was called as the Steam Play tool calls it).

## First headset runs, 2026-11-14

The Turing Test with UEVR (and `MWXR_FOVEATION=gaze`) on the PS VR2: UEVR hooked and
the OpenXR session reached FOCUSED with the PS VR2 Sense controllers and UEVR's emulated
gamepad, but the game's menu could not be passed and UEVR's overlay did not work. The
game had gone full screen on the headset's own display (above), so its window and menu
were in the headset rather than on the Mac, where the keyboard and mouse that UEVR's
Insert menu needs act. The session ended after three minutes when the connection to
Monado broke (`XR_ERROR_INSTANCE_LOST`). Patch 0013 keeps the headset display from the
game; to be tried again.

## Headset runs, 2026-11-16

With patch 0013 the game opened on the Mac and was playable in the headset with UEVR
(four runs: two with gaze foveation, two without). Found:

- **The Mac's display left unscaled.** The game sets 2880x1864, the display's full pixel
  size, and the crash on quit kept it from setting it back. The runner now restores the
  modes on exit ([steam-play.md](steam-play.md#the-macs-display-mode)).
- **Crash on quit, every run.** UE's crash reporter wrote a minidump each time
  (`Saved/Logs/UE4CC-Windows-*` in the game's `AppData/Local/VenusPrototype`): an access
  violation reading address 0x2 at `TheTuringTest.exe+0x279eed` on the main thread, under
  the C runtime's exit, the same address each time. A crash on exit is common with UEVR
  on Windows too; whether this game also does it flat has not been checked.
- **The player's arms in the left eye only.** UEVR's default rendering method, Native
  Stereo (`VR_RenderingMethod=0`), draws some meshes in one eye in many games. UEVR's
  remedies, in its menu (Runtime): Native Stereo Fix, or Rendering Method Synchronized
  Sequential.
- **Gaze foveation: parts of the image in the warped layout.** 6,328 `Rate-map read:
  second layout in one pass left untranslated` warnings in a gaze run, none without
  foveation. Every gaze update makes a new rate map, and a pass that reads this frame's
  target and one rendered under an earlier map (UE's temporal AA history, say) can
  translate only one of them; the other is read in its physical layout. Fixed foveation
  has one map, so it does not hit this (the 2026-11-14 fixed run had no warnings).
  Fixed in DXMT `0482175`: a pass keeps two read layouts, each read through its own
  map's tables. The read fixture's `--two-maps` case (a scene under each of two maps,
  read together by a pass under the second) passes on the new build, worst error 3.3
  logical pixels, and fails on the old one (2,048 px). The existing fixture cases and
  the resource-read tests pass. Installed in `wine-foveation-unified`.
- **Gaze foveation, second try: no better, and gaze had no visible effect.** The run had
  no rate-map warnings, valid gaze and 128 maps, so the reads were now right. Two other
  causes:
  - UEVR sets up 2800x2856 per-eye textures (its log), most likely filled by copying each
    eye's half of the engine's 5600x2856 target with `CopySubresourceRegion` (not
    confirmed in a trace). DXMT copied part of a warped image texel for
    texel, so each eye got the squeezed physical layout. DXMT `98ea692` resamples such
    a copy through the map. The fixture's `--partial-copy` case (the right half of a
    mapped image copied into a half-width texture) fails on the previous builds (2,048
    px) and passes on the new one (worst 1.5 px). Installed in `wine-foveation-unified`.
  - At the time the game's `sg.ResolutionQuality=44.444401` (its `GameUserSettings.ini`,
    saved on its first run) was taken to mean the scene rendered at 44%, below the map's
    size. That was wrong under UEVR: the audited runs at 100 and later at 44 again (the
    game rewrote it when its graphics settings were changed) both drew every scene pass
    at 5600x2856. Under UEVR the scene size follows the OpenXR recommended size times
    UEVR's `OpenXR_ResolutionScale` (its menu), not the game's resolution quality.
- **Gaze foveation, third try: wrong lighting in parts of each eye.** With the scene at
  100%, the eyes showed whole regions dark orange next to lit ones, with a hard edge.
  An audited run (`MWXR_FOVEATION_AUDIT=1`, [foveation.md](foveation.md#diagnosing-a-game))
  showed the map changing only at frame ends and every copy using the right layout.
  The fault: a 5600x2856 RGBA16F target that UE4 writes from compute (two dispatches
  reading the G-buffer, most likely its lighting) kept a layout tag from an old gaze map,
  left by an earlier mapped render pass. Compute writes at logical coordinates, so its
  contents were ordinary, but the passes that read it went through the stale map and
  took each pixel's lighting from elsewhere. DXMT `585ce5c` clears the tag when a texture
  is bound as a UAV for writing. The fixture's `--compute-write` case (a mapped scene
  pass, then a compute shader rewriting that texture) fails on the previous build (up to
  1,040 px) and passes on the new one (2 px). Installed in `wine-foveation-unified`.
- **Gaze foveation, fourth try: much better, but glitches on turning** (by stick or head),
  and constant in some places (climbing to the docking bay): smeared blocks and doubled
  edges. The audit showed the gaze map changing every frame and, every frame, three
  5600x2856 targets drawn by mapped passes over the previous frame's contents without
  a clear. Whatever those passes blended over or left undrawn was still in the previous
  map's layout but was then read through the new one. DXMT `eea53ec` rewrites such a
  target into the pass's layout first (a copy and one full-screen draw through both
  maps' tables), and tags a fully cleared target with the current map, since a clear
  reads the same in any layout. The fixture's `--relayout` case (a scene under map 1,
  then its left half redrawn under map 2 without a clear) fails without the rewrite
  (655 px) and passes with it (2 px). Each rewrite costs a copy and a full-screen draw
  of that target. Installed in `wine-foveation-unified`.
- **A scene smaller than the eye image** (2026-11-16). A game that renders its scene
  smaller than its output (a screen percentage, then an upscale) and upscales it to the 5600x2856
  eye image, and DXMT maps only passes of the map's size. DXMT (`macos-xr-foveation`
  after `eea53ec`) now reports the render-target sizes with the most draws each frame
  (`DXMT_GUID_RATE_MAP_SCENE_SIZES` on the device), and the bridge (`gaze-gate`) builds
  its gaze maps at the busiest size with the eye image's shape (within 2%) that is
  smaller than it and more than a quarter of its area, scaling the eye rectangles to
  match. A change of size must hold for 30 frames, and is logged (`gaze foveation
  follows the scene at WxH`). `MWXR_FOVEATION_SCENE=0` keeps the output's size. The
  fixture's `--scene-sizes` case checks the report; the bridge's choice has not yet
  been seen in the game. The upscale and later full-size passes are then not foveated.
  The previous bridge is kept as `build-in-process/gaze-gate.before-scene-20261116`. In
  The Turing Test under UEVR the scene is drawn at the eye image's size whatever the
  resolution quality (above), so this does not come into play there; lowering UEVR's
  resolution scale would make the eye swapchain smaller as well, and the map with it.
- **Crash on quit, fixed** (2026-11-16). The fault is in Unreal's binned allocator during
  the C runtime's exit: freeing a pointer for which it finds no pool (the lookup returns
  null, and it reads address 0x2), consistent with the engine freeing memory UEVR
  allocated. The injector now loads `mwxr-uevr-exit-guard.dll` before UEVR
  (`src/windows/uevr_exit_guard.c`): an access violation while the game has no visible
  window ends the process with exit code 0, before Unreal's crash handler can show
  "Fatal error". During play a window is visible and nothing changes.
  `MWXR_UEVR_EXIT_GUARD=0` leaves it out. Checked with a test program under Wine (an
  access violation with no window: exit code 0 and the guard's message).
  In the game it did not catch the crash (the next run, same dump). The C runtime frames
  that suggested the crash was in `exit()` were stale stack values (`free`, a command-
  line helper), so the crash is probably during Unreal's own shutdown, with its window
  still up. The guard now also takes an access violation once Windows has begun the
  process's exit, and logs what it sees to `uevr-guard.log` in the launch's log folder
  (`MWXR_UEVR_GUARD_LOG`; its stderr never reached `game.log`). UEVR found the engine's
  GMalloc ("binned"), so its `VR_UseFMallocSceneViewExtensions` option, which allocates
  the scene view extension the engine frees at shutdown through that allocator, was
  turned on in the profile (previous `config.txt` kept as `config.txt.pre-fmalloc-20261116`).
  The next launch then crashed while loading, at the same instruction
  (`TheTuringTest.exe+0x279eed`, address 0x2), so the option was turned off again. The
  guard's log from that run showed UEVR's scans faulting thousands of times on purpose
  while the window was up, which the guard rightly let through; it now logs only
  near-null faults while a window is up.
  The next quit crashed the same way, again about 30 s after launch, with only
  `VR_GhostingFix` differing from the last good run (it was on in the earlier load crash
  too), so Ghosting Fix rather than the FMalloc option set off the crash while loading;
  it was turned off again.
  Unwinding the crash dump with the game's unwind data (the earlier caller addresses
  were stale stack values) gives the real chain: `FMallocBinned::Free` (0x140279df0,
  GMalloc's vtable slot 4), called from `FMemory::Free` (0x140279d70: loads GMalloc at
  0x142de0fe0 and calls `[rax+0x20]`), reached through engine functions called from
  UEVR's code. `Free` finds no pool for the pointer and reads the null pool's
  `TableIndex` (offset 2), before taking any lock. The guard now skips such a free: on
  a near-null fault in a function of the game's image that GMalloc's vtable slot 4
  points to, called through `call [rax+0x20]` from a short `FMemory::Free` that loads
  GMalloc, it unwinds out of `Free` (`RtlVirtualUnwind`) and resumes in `FMemory::Free`
  as if the free had finished. The block, which UEVR allocated, leaks. Each skip is
  logged. Tested under Wine with a program of the same shape (three unowned frees
  skipped, normal exit); not yet seen in the game.
- **Synchronized Sequential** (`VR_RenderingMethod=1`, as in Gruntled's community
  profile, 14 Jan 2024: settings only, no UObjectHook, scripts or plugins; it also sets
  resolution scale 0.762 and a 10.9 cm near clip): the arms show in both eyes, but the
  image was blurry, more in the left eye. Each eye is then drawn on its own at
  2800x2856, so no pass matches the 5600x2856 gaze map and nothing is foveated. UEVR's
  Ghosting Fix (`VR_GhostingFix`, for the temporal blur of alternating eyes) was turned on.

## Limits

- **Foveation.** UEVR works with `MWXR_FOVEATION` too: patch 0025 carries the
  DXMT change into the foveation DXMT (`wine-foveation-unified`), and `gaze-gate` and
  `vk-fov-gate` were rebuilt with the layer fix (2026-11-14). The Turing Test with UEVR
  and fixed foveation: UEVR hooked, its double-wide eye swapchain became the foveation
  target, DXMT compiled rate-map variants, no `xrEndFrame` failures; the eye images
  showed the scene as without foveation (single frames; the menu scene moves). EXD
  (fixed, D3D11) and No Man's Sky (fixed, Vulkan) ran on the rebuilt gates.
- Only D3D11 games. UEVR's D3D12 hook fails at its dummy device (no D3D12 here).
- UEVR's .NET frontend is not used: settings are changed in the in-game menu, and
  community profiles are added with `uevr_profile.py import`, not the frontend's
  "Import Config".
- Two games whose processes share an exe name would share a settings folder (as they
  would on Windows).
- XInput 1.4 and DirectInput hooks time out when the game doesn't load those DLLs; this
  is UEVR's normal log noise, not a failure.

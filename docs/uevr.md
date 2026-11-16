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
  - The game renders its scene at 44% resolution: `sg.ResolutionQuality=44.444401` in
    its `GameUserSettings.ini`, saved on its first run (about 1280/2880 of the Mac
    display's width). DXMT foveates only passes the size of the map (the eye swapchain,
    5600x2856), so the scene passes, at about 2489x1269, are never foveated; only the
    full-size passes after the upscale are. That would make gaze foveation look like
    it does nothing, and the 44% also costs sharpness in the headset. Set to 100 on
    2026-11-16 in both prefixes' copies (`~/Windows/prefixes/499520`, the one in use, and
    `prefixes-wow64/499520`), the previous files kept as `GameUserSettings.ini.pre-100`.
    Changing the game's own graphics settings may write it again.
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

# Steam Play in macOS Steam

macOS Steam carries Linux Steam's Steam Play machinery, switched off. With it on, Steam
installs a game's Windows build into its own library and starts it through a
compatibility tool, as Linux Steam does with Proton. Here the tool is the rig's runner:
Wine with DXMT and lsteamclient, OpenXR through the in-process wineopenxr and OpenVR
through xrizer ([steam-games.md](steam-games.md), [lsteamclient.md](lsteamclient.md)).
This replaces SteamCMD, the per-game launcher apps and the non-Steam shortcuts for the
games it is used for. Valve's files are not modified.

The approach follows `Superd22/macos-steam` (AGPL-3.0), which was read for orientation; no
code is taken from it. NotProton does the same switch-on with a library written into
Steam.app's `Info.plist`, which breaks Valve's signature, and hooks much more of the client.

## How it works

1. **Steam Play.app** (`~/Applications`, `src/macos/steam_play_launcher.c`) replaces itself
   with Valve's client, `~/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steam_osx`,
   started as arm64, adding three variables. It does not go through `/Applications/Steam.app`:
   that bundle has the hardened runtime, which drops `DYLD_*` variables; the inner client
   does not.
2. `DYLD_INSERT_LIBRARIES` loads **the compat gate** (`src/macos/steam_compat_gate.c`). When
   `steamclient.dylib` loads, it finds the one place where Steam's compatibility manager
   sets "compatibility tools enabled" from `V_strnicmp(host OS, "linux") == 0`, and makes
   it store 1 instead. The site is found from the code that loads the `"linux"` string, not
   from the field's offset, which changes between client builds (`0x7b0` in 1785187029,
   `0x810` in 1793056502). Exactly one site must match or nothing is patched. The change is
   in memory only and ends when Steam quits.
3. `STEAM_EXTRA_COMPAT_TOOLS_PATHS` registers **the macos-wine-xr-proton tool**
   (`tools/steam_play/`): `compatibilitytool.vdf` (Windows to macOS), `toolmanifest.vdf`, and
   `mwxr-steam-play`, which Steam runs as `mwxr-steam-play waitforexitandrun <exe> [args]`
   from the game's working folder with `STEAM_COMPAT_APP_ID` set. It starts
   `run-windows-game.zsh` (the rig wrapper named in the tool's `runner` file) with
   `GAME_EXE` and `GAME_WORKDIR`; the game gets its own prefix
   (`~/Windows/prefixes/<appid>`, `scripts/game-prefix.zsh`). Steam counts the game as
   running until the script returns.
4. `STEAM_COMPAT_TOOL_MAPPINGS` (`APPID 250 macos-wine-xr-proton`, comma-separated; 250 is the
   lowest priority the client applies) maps the games in the payload's `mapped` file to
   the tool: every Windows-only game in the library, every VR game with a Windows build,
   and those listed in `games`
   (Choosing games, below). A listed game that also has a macOS build (Hyperbolica does)
   installs as Windows; Steam asks which launch entry to use where there are several
   (Hyperbolica's VR mode adds `startXR`).

Do not map app ID 0 (every game), as Linux Steam's "Enable Steam Play for all other
titles" does. On 2026-10-28 that switched every installed game with a macOS build to its
Windows depot (seven games, `config changed : added depots ...` in
`logs/content_log.txt`); Steam had not downloaded them yet. Verifying each game's files
(`steam://validate/<appid>`) with the mapping removed put them back on their macOS depots
with no files changed.

## Use

```sh
scripts/install-steam-play.zsh                 # build and install; checks the installed Steam client
$EDITOR ~/Library/Application\ Support/macos-wine-xr/steam-play/games   # other games with a macOS build to play as Windows; -APPID to leave one out
python3 ~/Library/Application\ Support/macos-wine-xr/steam-play/steam_play_games.py list   # what is mapped and why
open ~/Applications/Steam\ Play.app            # quit Steam first; start Steam this way from now on
tests/macos/test_steam_compat_gate.zsh         # after a Steam client update
scripts/install-steam-play.zsh --uninstall
```

Then install and play the mapped games from Steam's library as usual.
`STEAM_PLAY_RUNNER` chooses the rig wrapper (default `~/Windows/run-windows-game.zsh`).
Steam started from Steam.app has Steam Play off: Windows-only games then cannot be
installed or started, and a listed game with a macOS build is targeted at its macOS
build again (on 2026-10-28 Hyperbolica's update started for its macOS depot and finished
on its Windows one, so nothing changed, but keep to Steam Play.app).

## Choosing games

A game's choice can be made in Steam, on the Compatibility page of its Properties (next
section), or in the payload's `games` file, which that page writes. Steam Play.app runs
`steam_play_games.py sync` before each start (0.2 s, from Steam's offline caches), and
the running client runs it again when `games` or Steam's caches change (Live mapping,
below), which writes the payload's `mapped` file from two sources:

- every game in the library (the app IDs in `appcache/librarycache`) whose app info lists
  Windows and not macOS: these cannot be played on macOS Steam otherwise, so mapping them
  changes nothing else. "Game" here, and below, takes in Steam's types game, application
  and demo (Get Lost With Luke In Nature, a VR app, is an application; on 2026-11-08 it
  and eight other apps and demos were left out until this), not DLC, soundtracks or
  Steam's "tool" entries;
- every VR game with a Windows build, macOS build or not (`steam_games.is_vr`: OpenVR,
  OpenXR or VR-only support, or a VR launch entry), since macOS builds have no VR. Such a
  game installs as Windows; one already installed as macOS would be switched (downloaded
  again) at the next Steam start;
- every game with a Windows build whose macOS build is 32-bit only: its app info's
  `common.osextended` lacks `macos64`, the flag Steam's client checks for 64-bit macOS
  (macOS has not run 32-bit apps since 10.15). All 13 installed macOS games have it; 120
  owned games, Amnesia: The Dark Descent among them, do not;
- the app IDs listed in `games`: other games that also have a macOS build and should
  install and start as Windows games. `-APPID` there leaves a game out, so a VR game with
  a macOS build goes back to it.

On 2026-10-28 that mapped 135 games: 127 Windows-only, 4 VR games with a macOS build
(InMind VR, SEGA Mega Drive & Genesis Classics, Serious Sam Fusion 2017 (beta) and
Subnautica, none installed; the two flat games with a VR mode install as Windows too
unless excluded) and the 4 listed (Hyperbolica, Assetto Corsa, Half-Life: Alyx, Audio
Trip; Hyperbolica is also a VR game). No installed macOS game changed depots. If the sync fails, the previous `mapped` (or `games` alone) is
used. Tests: `python3 tests/macos/test_steam_play_games.py`.

Logs, in `~/Library/Logs/macos-wine-xr`: `steam-compat-gate.log` (expect
`Steam Play gate open`), `steam-play.log` (each call from Steam and the runner's output)
and `<appid>-<time>/game.log`. Steam's own: `logs/compat_log.txt` (`Registering tool
macos-wine-xr-proton`, `Mapping AppID N to tool`) and `logs/content_log.txt` (depots).

## Results, 2026-10-28

Steam client 1793056502, macOS 27.0.1, Apple Silicon:

- The gate opened in the arm64 client; Steam registered the tool and the mapping.
- Hyperbolica (1256230), not previously in macOS Steam's library, installed from the
  Install button as Windows depot 1256232 (861 MB) into `steamapps/common/Hyperbolica`.
- Play, normal mode: Steam called the tool, which made the game's prefix and started it;
  D3D11 through DXMT (feature level 11.1). Steam tracked it until it was closed (exit 0).
  VR mode reached the tool with `startXR`; not run with a headset yet.

## Setup scripts

Windows and Linux Steam run each game's install script the first time it is played:
registry values, and redistributable installers from Steamworks Shared (228980), each
recorded under a "has run" registry key so it runs once. SteamCMD does not run them, so
games installed with `steam_games.py install` never had them either, and relied on Wine's
own runtime libraries.

macOS Steam asks the tool to run them, as Linux Steam asks Proton:

```
mwxr-steam-play run <client>/legacycompat/iscriptevaluator.exe legacycompat\evaluatorscript_<appid>.vdf
```

from the client's folder (`.../Steam.AppBundle/Steam/Contents/MacOS`), with the game's
`STEAM_COMPAT_APP_ID`, `STEAM_COMPAT_DATA_PATH` (`steamapps/compatdata/<appid>`),
`STEAM_COMPAT_INSTALL_PATH` and, when redistributables are involved, `STEAM_COMPAT_MOUNTS`
(the Steamworks Shared folder). macOS Steam does not ship `iscriptevaluator.exe`
(`legacycompat` is empty apart from the script during the call), and deletes the script
afterwards. The tool saves each call in `~/Library/Logs/macos-wine-xr/steam-play-run/<time>-<appid>/`
(`call.txt`, `env.txt` without the account name, the script), then runs the rig runner
with `GAME_SETUP_SCRIPT` set to the saved script. The runner makes the game's prefix if
it is new, and `tools/steam_play/install_script.py` turns the script into a `.reg` file
that `wine reg import` applies there. The result's digest is kept in the prefix
(`.mwxr-setup-<appid>.sha1`), so the call Steam makes before each start imports nothing
when nothing changed. Every step applied or skipped is logged in `steam-play.log`. The
tool returns 0 even when this fails, so a setup problem never stops the game.

The script is already resolved by Steam: one numbered entry per step, each with `appid`
(228980 for the shared redistributables), `mode`, `install_path` (a macOS path, which
`%INSTALLDIR%` stands for) and `compat_installscript`, which holds the usual install
script sections: `Registry` (key, then value type such as `string`, then name and value)
and `run process` (named entries with `hasrunkey`, `process 1`, `command 1` and
`requirement_os`). Seen on 2026-10-28:

- Mr. Robot (20710): one `Registry` step, `HKLM\SOFTWARE\Moonpod\MrRobot` `installpath`.
  Steam called it twice: `mode 2` with no language when the install finished, and
  `mode 1` with `language english` just before the game started.
- Canyon Capers (275490), when its install finished: four `run process` steps from
  Steamworks Shared, Visual C++ 2012 and 2013 (x86 and x64), DirectX June 2010
  (`DXSETUP.exe /silent`) and OpenAL (`oalinst.exe /s`).

Every one of those installers is a 32-bit exe, `vcredist_x64.exe` included (a 32-bit
bootstrapper), so none can run on the 64-bit-only runtime; games rely on Wine's own
runtime libraries, as the SteamCMD installs always have. Installers are logged and
skipped, and their `hasrunkey` values are left unset. Registry steps are applied:
`string` and `dword` values, with `%INSTALLDIR%` as the install folder's Windows path
(`Z:\Users\...`), and a block named after a language only for the script's language.

Replaying Mr. Robot's captured call through the installed tool put
`HKLM\SOFTWARE\Moonpod\MrRobot` `installpath` into `~/Windows/prefixes/20710`; a second
call imported nothing. Tests: `python3 tests/windows/test_steam_play_install_script.py`.

## The Compatibility page

macOS Steam draws no Compatibility page in a game's Properties, and its client has no
handler for the page's requests. `src/macos/steam_ui_patch.c`, built into the gate
library, supplies both, in memory:

- The page: Steam's interface is web code in `steamui/`. The Properties window adds the
  page only when `CI()`, which is `PLATFORM=="linux"`, is true (2 of its 23 uses). When
  Steam's web helper ("Steam Helper") opens `steamui/sp.js`, our interposed `open`
  (and `stat`, `lstat`, `openat`, `fopen`) gives it a copy, made once per `sp.js` in
  `~/Library/Caches/macos-wine-xr/steamui`, in which those two checks read true.
  `steam_osx`'s own size checks are not redirected: it compares its files' sizes with its
  install record, and a different size made it re-extract its package and quit at every
  start (seen while building this).
- The tool list and choice: the page asks the client `CompatManager.GetCompatTools` and
  `CompatManager.SpecifyCompatTool`, which macOS Steam answers with result 2, "no handler
  for message" (seen with DevTools). In the copy, the hook that makes those requests
  fetches `/mwxr/compat/<appid>.json` and `/mwxr/select/<appid>/<tool>.json` from
  steamloopback.host instead, and the web helper's request for those files is answered
  by the same library: the list from the `games` and `mapped` files, a choice written to
  `games`. The dropdown offers Default (automatic: Windows for Windows-only and VR games,
  the macOS build otherwise), macos-wine-xr (adds the app ID) and "None: the macOS build"
  (adds `-APPID`). A choice is shown at once. While it differs from what Steam loaded at
  its start (the answer's `mwxr_pending`), the dropdown's description reads "Applies when
  Steam restarts" with a Restart Steam button, which calls Steam's own restart. With live
  mapping on, the choice is applied at once and the note does not appear (below).
- Steam's restart: Steam ends the client with exit code 42 and leaves the new start to
  its parent, which for Valve's Steam.app is its launcher (`steam.sh`, a binary with
  `-alwaysrelaunch` and `ReLaunchCmdLine`). Steam Play.app replaces itself with the
  client, so `src/macos/steam_restart.c`, in the same library, catches `exit(42)` (and
  `_exit(42)`) in `steam_osx`. After Steam's own exit handlers have run, it brings the
  game list up to date (`steam_play_games.py sync`) and replaces the process with
  `steam_osx` again, with the same arguments, as arm64, so Steam Play stays on. This also
  covers a restart from Steam's Settings.

Steam's interface caches `sp.js` in a service worker (`config/htmlcache/Default/Service
Worker`), so the copy reaches it only when that cache is refreshed, which Steam does
after a client update. To apply a new version of the patch by hand, quit Steam and move
that folder aside; Steam rebuilds it at the next start. If `sp.js` no longer has the
expected code, the original is used and the page stays hidden.
`MWXR_STEAM_COMPATIBILITY_PAGE=0` turns the redirect off.

On 2026-10-28 (client 1792617903, beta): choosing macos-wine-xr on Amnesia: The Dark
Descent's page (a game with a macOS build, not installed) wrote `57300` to `games`, and
after a restart through Steam Play.app Steam logged `Mapping AppID 57300 to tool
"macos-wine-xr-proton" with priority 250`. Choosing "None" then showed the note and the
button; the button restarted Steam in the same process (pid unchanged, Steam Play gate
opened again) and the page showed the choice with no note.

Keep Steam Play.app in the Dock, not the running Steam. Once started, the client
registers with macOS as Valve's app (`com.valvesoftware.steam`, the bundle in
`Steam.AppBundle`), so it shows its own Dock icon; keeping that one in the Dock would
start Steam without Steam Play. Opening Steam Play.app while Steam runs brings Steam
forward.

Debugging the page: start Steam with `open -a ~/Applications/Steam\ Play.app --args
-cef-enable-debugging`, which opens Chromium's DevTools on 127.0.0.1:8080 for that run
(`/json/list` names the pages; the interface's code runs in `SharedJSContext`). Start it
normally afterwards to close the port.

## Live mapping

Steam reads `STEAM_COMPAT_TOOL_MAPPINGS` once, at its start, so before this a game bought
or chosen while Steam ran was mapped only after a restart. `src/macos/steam_live_mapping.c`,
in the gate library, maps it while Steam runs:

- At load it finds `CCompatManager::AddMapping(manager, appid, {tool, config, priority})`,
  the function Steam's startup code calls for each entry of that variable: the one called
  after the code that loads the `"STEAM_COMPAT_TOOL_MAPPINGS"` string whose first
  instructions use the `"CCompatManager::AddMapping"` profiling label
  (`steamclient+0x6bbe30` in 1794079253). Exactly one must match, and its first four
  instructions must be a plain prologue.
- Those four instructions move to a trampoline (a `MAP_JIT` page) that records the first
  argument and the calling thread, and returns to the function. Steam's own calls at
  startup record the compat manager and its thread, `IPC:CSteamEngine`.
- A watcher runs `steam_play_games.py sync --output mapped.live` when the payload's `games`
  changes (at once) or Steam's `appcache/appinfo.vdf` or `appcache/librarycache` change
  (checked every 5 s; Steam writes both while it runs, so a new game is seen once its
  app info is there). It calls `AddMapping` for each game added (priority 250, as at
  startup) and with an empty tool for each one removed, which Steam treats as an unmap.
  The calls run on Steam's engine thread, from an interposed `kevent`, which that thread
  waits in between frames. Then `mapped.live` becomes `mapped`.
- `live-mapping` in the payload holds the client's pid while this works. The web helper
  then waits up to 4 s after a Compatibility page choice for `mapped` to show it, so the
  page shows the choice as applied.

Steam logs each change in `logs/compat_log.txt` (`Mapping AppID N to tool
"macos-wine-xr-proton" with priority 250`, `Unmapping AppID N`), and the gate log has
`live mapping: on (compat manager ..., thread IPC:CSteamEngine)` and `live mapping:
+N -M`. If the function is not found, a change does not run on the recorded thread
within 30 s, or `MWXR_STEAM_LIVE_MAPPING=0` is set, the change waits for a restart as
before, and the page shows its restart note. Mapping a game whose macOS build is
installed switches it to its Windows depot at once, as a restart would. The library's
32-bit mark (`mwxrMapped`, above) is loaded once per interface load, so a newly mapped
game with a 32-bit macOS build keeps the "no entry" icon until Steam's interface reloads.

On 2026-11-08 (client 1794079253): listing Uplink (1510, a macOS game, not installed) in
`games` logged `Mapping AppID 1510` in Steam a second later, and removing it logged
`Unmapping AppID 1510`; Steam kept running. A game bought while Steam runs has not been
tried yet.

## Launch Options

Words of the form `MWXR_NAME=value` in a game's Launch Options (Steam: the game's
Properties, General) are settings for that launch: the tool exports them and does not
pass them to the game. A literal `%command%`, as Linux Steam uses, is dropped, so
`MWXR_ALYX=native %command%` works too. Each one is logged in `steam-play.log`
(`launch option ...`). They reach the launch only, not the setup call before it.

| Setting | Effect |
| --- | --- |
| `MWXR_ALYX=native` | Half-Life: Alyx starts its native ARM64 build (below) |
| `MWXR_ALYX_OPTIMIZED=1` | with it, the native launcher's `--optimized` xrizer build |
| `MWXR_WOW64=1` or `0` | the WoW64 or 64-bit runtime, whatever the game's exes are |
| `MWXR_UEVR=1` | inject UEVR into an Unreal Engine game once its window is up ([uevr.md](uevr.md)) |

Tested by calling the tool as Steam does; macOS Steam passing Launch Options to the tool
is not confirmed yet.

## Native Half-Life: Alyx

With `MWXR_ALYX=native`, Play starts `scripts/run-alyx-native.zsh`, which runs
`~/Code/alyx-native/run-alyx` (the Linux ARM64 build on its own host, against macOS
Steam) with a run folder `~/Code/alyx-native/runs/steam-<time>` and a 12-hour limit
instead of its 120-second default. The launcher already passes Alyx's `-vr -steam ...
+map startup`, so Steam's copies are dropped; any further Launch Options go to the engine.
Steam's Stop reaches the launcher as SIGINT, which stops the game's process group (a
SIGTERM would end the launcher and leave the game running). The native build runs from
its own prepared folder (`workspace/alyx-native-game-root`), not the copy Steam
installed: Steam updates do not reach it and its saves are its own. Without the setting,
Alyx keeps the Wine route. Tested with a stand-in launcher (arguments and stop); not yet
started from Steam.

## WoW64

`scripts/steam-play-game-settings.zsh` gives a game the WoW64 runtime
(`runtime-wow64`) and its own prefix root (`~/Windows/prefixes-wow64`) when any of its
Windows executables is 32-bit: the exe Steam starts, or any Windows launch entry in
Valve's app info found in the install folder (`tools/steam_play/game_arch.py`, about
0.13 s). Checking every entry gives the setup call, which has no exe, the same answer as
the launch, so both use the same prefix. On 2026-10-28: Assetto Corsa and Mr. Robot
32-bit; Hyperbolica, Audio Trip and Half-Life: Alyx 64-bit. Outside Steam Play Assetto
Corsa still uses WoW64 by app ID. `MWXR_WOW64` overrides the choice for a launch; the
setup call before it still follows the exes. The WoW64 runtime has no 32-bit
`winemetal.dll`, so a 32-bit Direct3D game would render with Wine's own Direct3D, not
DXMT; 32-bit launchers like Assetto Corsa's are what it is for so far.
Tests: `python3 tests/macos/test_steam_play_game_settings.py`.

## Steam Cloud

macOS Steam's client carries Linux Steam's Steam Play save mapping: it resolves the
Windows Cloud roots (`WinAppDataLocalLow`, `WinAppDataRoaming`, `WinMyDocuments`,
`WinSavedGames` and so on) under
`steamapps/compatdata/<appid>/pfx/drive_c/users/steamuser/...`, as Proton's prefixes are
laid out, but only when the game's compatibility tool has "proton" in its name
(`V_stristr(name, "proton")` before the paths are set, in the same function that sets the
host platform; a second check sets `STEAM_COMPAT_PROTON=1` for the game, which is the
only other effect found). The tool was called `macos-wine-xr` until 2026-10-28, and
Hyperbolica's Cloud sync logged `Unable to resolve path with root WinAppDataLocalLow
(12)` at Steam start, at launch and at exit. It is now `macos-wine-xr-proton`.

The runner links the rest: `compatdata/<appid>/pfx` to the game's own prefix, and
`drive_c/users/steamuser` in the prefix to its Wine user (`crossover`), when it makes or
uses a game's prefix for a launch or a setup call. A `pfx` that is a real folder is left
alone. Steam syncs Cloud saves after the setup call and before the launch, so a game
with a setup script gets its saves on the first start; one without gets them from its
second start (saves made in the first session still upload when it exits).

On 2026-10-28, with the new name and both links in place for Hyperbolica, Steam's sync at
start resolved `compatdata/1256230/pfx/drive_c/users/steamuser/AppData/LocalLow/...` and
downloaded `CodeParade/Hyperbolica/game.save` (447 bytes, the save from Windows Steam on
2026-10-09) into `~/Windows/prefixes/1256230`. Games that save inside their own folder
(Half-Life: Alyx) are synced from the install folder and need none of this.

Linux Steam deletes `compatdata/<appid>` when a game is uninstalled, which with a `pfx`
link could reach the prefix. macOS Steam does not: on 2026-10-28 Mr. Robot (not mapped)
and Canyon Capers (mapped) were uninstalled with `pfx` links to decoy folders, and Steam
removed the game folders and manifests and left `compatdata/<appid>`, the links and the
decoys alone. Uninstalling also calls the setup script once more, with every step's
`mode` 3; the runner recognises that and does nothing, so the prefix is neither made nor
removed (it stays, as Proton's would not, until removed by hand).

## Limits

- Setup scripts: registry values are applied, redistributable installers are not.
- Under Steam, `pgrep` does not list `steam_osx`, so the runner and
  `run-alyx-xrizer.zsh` skip their "is Steam running" checks for games Steam starts.
- Steam client updates: the gate depends on the `"linux"` comparison keeping its shape,
  live mapping on `AddMapping`'s. `install-steam-play.zsh` and the test check both
  without starting Steam (live mapping missing is a warning: changes then wait for a
  restart).
- The launch preflight checks both Wine and Steam bridge architecture support.


## Assetto Corsa WoW64 rollout, 2026-10-28

The rig wrapper `~/Windows/run-windows-game.zsh` sources
`scripts/steam-play-game-settings.zsh` before assigning its normal defaults.
For app **244210** only, absent an explicit caller override, that selects:

- `~/Code/wine-crossover/runtime-wow64`, with both Steam bridge architectures.
- `MWXR_GAME_PREFIXES=~/Windows/prefixes-wow64` and `GAME_PREFIX=pergame`.
- A fresh WoW64 template and `~/Windows/prefixes-wow64/244210`, separate from
  the existing 64-bit template/game prefixes.

Explicit `MWXR_IN_PROCESS_WINE`, `MWXR_GAME_PREFIXES` and `GAME_PREFIX` values
remain authoritative. The source line in the rig wrapper is:

```zsh
source "${bridge}/scripts/steam-play-game-settings.zsh"
```

The shared Windows Steam client directory is provisioned one DLL at a time:
`steamclient.dll` for the launcher and `steamclient64.dll` for the race engine.
An existing Win64 DLL no longer masks a missing Win32 DLL.

### Library placement

The Windows Steam installation was APFS-cloned from
`~/Windows/prefix/drive_c/Program Files (x86)/Steam/steamapps/common/assettocorsa`
to macOS Steam's `~/Library/Application Support/Steam/steamapps/common/assettocorsa`.
All **34,824 files / 44,949,470,831 logical bytes** matched by path, type and
size; SHA-256 matched for the launcher, both race engines, x64 OpenVR DLL and
native Chromium DLL. These are cloned blocks, not an independently downloaded
45 GB copy. The original directory and Windows manifest remain available.

With Steam stopped cleanly via its `-shutdown` command, its existing
`appmanifest_244210.acf` was copied into macOS Steam's `steamapps`. App 244210
was added to the Steam Play `games` list, and Steam was restarted through
`Steam Play.app`. Steam's own `libraryfolders.vdf` now lists the installed app
and its size; `compat_log.txt` records `Mapping AppID 244210 to tool
"macos-wine-xr" with priority 250` and the app-config-changed callback.
No broad mapping for other games was added.

The rig wrapper and games list were backed up as
`run-windows-game.zsh.before-ac-wow64-20261028` and
`games.before-ac-wow64-20261028` beside their originals. Rollback can restore
those files with Steam stopped; the original Windows installation was retained.

### Checks and remaining dependencies

The wrapper launched the original `AssettoCorsa.exe` from the macOS Steam
library and selected the new per-game prefix. It initially exited with 255;
`+mscoree` diagnostics identify `CLRRuntimeInfo_GetRuntimeHost: Wine Mono is
not installed`. This is a CLR dependency failure, after runtime/prefix
selection, not evidence of a Steam account or bridge failure.
Native .NET Framework 4.0 and then 4.8 were installed successfully using the
official Winetricks recipe (script SHA-256
`672a1ff4442e8691a3ffc0e6860137e201a1d1227e9b3044245f1731e9e9837e`)
and checksum-verified Microsoft installers. The first attempt lost Wine's
bundled FreeType path when launched through a system shell, producing console
font errors; it was stopped and replaced from the clean WoW64 template. The
failed-attempt backup was removed after the successful .NET/launcher retry.
The successful retry used a Wine shell wrapper which exports
`DYLD_LIBRARY_PATH` and `DYLD_FALLBACK_LIBRARY_PATH` inside the script before
execing the runtime. This avoids system-shell DYLD environment stripping.
Only Assetto Corsa's new prefix was affected; the original install and other
game prefixes were not changed. Winetricks records `dotnet40` and `dotnet48`.

The subsequent original-launcher test remained running for 40 seconds and
was stopped through the runner's normal per-game termination path. Its own
`Documents/Assetto Corsa/logs/launcher.log` records successful Steam API
initialization, screen/theme initialization, and the macOS library's intro
video URL. The CEF log reports the video playing. These were startup/log checks. The user subsequently confirmed that Assetto
Corsa runs all the way through to the menu (2026-10-28). WPF logged a non-fatal
OpenGL framebuffer error. Racing, VR, wheel input and force feedback have
not been validated.

Starting a race then failed: the launcher started `acs.exe`, which closed at once and
returned to the launcher with no message. The per-launch `game.log` showed Wine aborting
it at `d3dx11_43.dll.D3DX11CreateShaderResourceViewFromFileW`, a stub in CrossOver 26.3
(as were `CreateTextureFromFile`, `GetImageInfoFromFile` and
`CreateShaderResourceViewFromMemory`). `patches/crossover/0009-d3dx11-load-textures-from-files.patch`
implements them on Wine's existing texture loader. Wine's d3dx11 tests, run in the
244210 prefix against DXMT, pass the new file paths, with the same 7 pre-existing
memory-path failures (test 17 data, invalid load info 4–7) repeated for the file
variants. The rebuilt `d3dx11_43.dll` (both architectures) is installed in
`runtime-wow64` and the 244210 prefix, with the old copies kept as `*.pre-d3dx11-file`.
The user confirmed races start and run (2026-11-01).

A Bluetooth DualSense then worked in the launcher's controls screen but not in a race.
`cfg/controls.ini` had `INPUT_METHOD=X360` (the Gamepad method), which reads only XInput,
and this Wine (no SDL; macOS IOHID) presented only Xbox pads to XInput: the DualSense
reached games through DirectInput alone.
`patches/crossover/0010-xinput-sony-gamepads-over-iohid.patch` marks DualSense and
DualShock 4 pads as gamepads in winebus and maps Sony's raw report in xinput and
winexinput (sticks X/Y and Z/Rz, triggers Rx/Ry, Y axes pointing down, Sony button order,
hat 0–7 with 8 for centred). winebus's existing Bluetooth DualSense fixup already turns
extended `0x31` reports into report 1, the layout mapped. With the controller held, an XInput
probe in the 244210 prefix read Cross/Circle/Square/Triangle as A/B/X/Y, d-pad up, both
triggers to 255, left stick up as +Y and right stick right as +X (2026-11-01).

The same patch adds DualSense rumble to XInput. A raw DualSense has no HID haptics usages,
so `XInputSetState` sends Sony's output report the way Linux's hid-playstation does: only
the compatible-vibration valid flags set (light bar and LEDs untouched), left (heavy) motor
from `wLeftMotorSpeed`, right (light) from `wRightMotorSpeed`; report 0x31 with a sequence
tag and a CRC32 seeded with 0xa2 over Bluetooth, report 0x02 over USB. On the Bluetooth
controller the user felt heavy, light and both motors from an XInput test in the 244210
prefix, and `XInputGetCapabilities` reports force feedback with 255/255 motors
(2026-11-01). USB and DualShock 4 rumble are untested and not implemented respectively.
Installed in `runtime-wow64` (winebus.so, winexinput.sys, xinput1_1–1_4) and the
244210 prefix, with old copies kept as `*.pre-sony-pad`; `runtime-lsteamclient` does not have it.

Unit checks cover Assetto Corsa's selection, other-game defaults, explicit
overrides, architecture-specific bridge provisioning and launch preflight:

```sh
python3 tests/macos/test_steam_play_game_settings.py
python3 tests/macos/test_game_prefix_sync.py
python3 tests/windows/test_game_launch_preflight.py
```


From Steam's Play button (`steam-play.log`, 19:24): Steam ran the setup script (eight
redistributable installers, all skipped; .NET was already in the prefix from
Winetricks), then `waitforexitandrun` with `assettocorsa/AssettoCorsa.exe` from macOS
Steam's library; the runner chose `~/Windows/prefixes-wow64/244210`, and the launcher
reached its menu. It ran for about 1.5 minutes and exited with 255. Racing (the
32-bit launcher handing over to the 64-bit race engine), VR and input are not
validated.

## Games installed elsewhere

Games already installed by Windows Steam in the rig prefix (or by SteamCMD) become
installed games in macOS Steam without a download:

1. Quit Steam (`open steam://exit`).
2. Clone the game folder into macOS Steam's `steamapps/common` (`cp -cpR`: an APFS clone
   of the whole tree, which takes no space until a file changes).
3. Copy its `appmanifest_<appid>.acf` beside macOS Steam's others. Windows Steam's
   manifest names the Windows depots, so Steam has nothing to download.
4. Add the app ID to the Steam Play `games` list and start Steam through Steam Play.app.

Steam then lists the game as installed (`StateFlags` 4, and its size in
`libraryfolders.vdf`) and maps it to the tool. On 2026-10-28 this moved Assetto Corsa
(244210), Half-Life: Alyx (546560, 3,176 files, 73.5 GB) and Audio Trip (826540, 2.1 GB);
paths and sizes matched the originals, Steam queued no download, and no other game's
depots changed. Steamworks Shared then showed an update required, for the
redistributables those games use. The originals stay in the rig prefix.

Half-Life: Alyx keeps its saves in `game/hlvr/save` inside the game folder, and macOS
Steam's Cloud had already put older saves (2026-10-14) in an otherwise empty
`steamapps/common/Half-Life Alyx`. That folder was moved to
`~/Windows/backups/half-life-alyx-macos-steam-folder-20261028`, and the clone brought the
rig copy's newer saves (2026-10-27). From Steam Play, the runner points
`run-alyx-xrizer.zsh` at the copy Steam installed (`ALYX_GAME_ROOT` from
`STEAM_COMPAT_INSTALL_PATH`); its prefix and settings are still the rig prefix's. Steam
Cloud may ask which saves to keep on the first start.

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
3. `STEAM_EXTRA_COMPAT_TOOLS_PATHS` registers **the macos-wine-xr tool**
   (`tools/steam_play/`): `compatibilitytool.vdf` (Windows to macOS), `toolmanifest.vdf`, and
   `mwxr-steam-play`, which Steam runs as `mwxr-steam-play waitforexitandrun <exe> [args]`
   from the game's working folder with `STEAM_COMPAT_APP_ID` set. It starts
   `run-windows-game.zsh` (the rig wrapper named in the tool's `runner` file) with
   `GAME_EXE` and `GAME_WORKDIR`; the game gets its own prefix
   (`~/Windows/prefixes/<appid>`, `scripts/game-prefix.zsh`). Steam counts the game as
   running until the script returns.
4. `STEAM_COMPAT_TOOL_MAPPINGS` (`APPID 250 macos-wine-xr`, comma-separated; 250 is the
   lowest priority the client applies) maps **only the games listed** in
   `~/Library/Application Support/macos-wine-xr/steam-play/games` to the tool. A listed
   game that also has a macOS build (Hyperbolica does) installs as Windows; Steam asks
   which launch entry to use where there are several (Hyperbolica's VR mode adds
   `startXR`).

Do not map app ID 0 (every game), as Linux Steam's "Enable Steam Play for all other
titles" does. On 2026-10-28 that switched every installed game with a macOS build to its
Windows depot (seven games, `config changed : added depots ...` in
`logs/content_log.txt`); Steam had not downloaded them yet. Verifying each game's files
(`steam://validate/<appid>`) with the mapping removed put them back on their macOS depots
with no files changed.

## Use

```sh
scripts/install-steam-play.zsh                 # build and install; checks the installed Steam client
$EDITOR ~/Library/Application\ Support/macos-wine-xr/steam-play/games   # one app ID per line
open ~/Applications/Steam\ Play.app            # quit Steam first; start Steam this way from now on
tests/macos/test_steam_compat_gate.zsh         # after a Steam client update
scripts/install-steam-play.zsh --uninstall
```

Then install and play the listed games from Steam's library as usual.
`STEAM_PLAY_RUNNER` chooses the rig wrapper (default `~/Windows/run-windows-game.zsh`).
Steam started from Steam.app has Steam Play off: listed games then look like macOS games
that are not installed.

Logs, in `~/Library/Logs/macos-wine-xr`: `steam-compat-gate.log` (expect
`Steam Play gate open`), `steam-play.log` (each call from Steam and the runner's output)
and `<appid>-<time>/game.log`. Steam's own: `logs/compat_log.txt` (`Registering tool
macos-wine-xr`, `Mapping AppID N to tool`) and `logs/content_log.txt` (depots).

## Results, 2026-10-28

Steam client 1793056502, macOS 27.0.1, Apple Silicon:

- The gate opened in the arm64 client; Steam registered the tool and the mapping.
- Hyperbolica (1256230), not previously in macOS Steam's library, installed from the
  Install button as Windows depot 1256232 (861 MB) into `steamapps/common/Hyperbolica`.
- Play, normal mode: Steam called the tool, which made the game's prefix and started it;
  D3D11 through DXMT (feature level 11.1). Steam tracked it until it was closed (exit 0).
  VR mode reached the tool with `startXR`; not run with a headset yet.

## Limits

- Setup scripts: registry values are applied, redistributable installers are not (below).

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
- Under Steam, `pgrep` does not list `steam_osx`, so the runner skips its "is Steam
  running" check for games Steam starts.
- Half-Life: Alyx still goes to `run-alyx-xrizer.zsh` with its rig-prefix install, not the
  copy Steam installs.
- Steam client updates: the gate depends on the `"linux"` comparison keeping its shape.
  `install-steam-play.zsh` and the test check it without starting Steam.
- Runtime support is selected per game: Assetto Corsa uses the WoW64 runtime;
  other games keep the original 64-bit runtime. The launch preflight checks
  both Wine and Steam bridge architecture support.


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

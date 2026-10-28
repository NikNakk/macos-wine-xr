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

- `run` calls (Steam's first-time setup scripts, such as Steamworks Shared's
  `iscriptevaluator.exe` for redistributables) are logged and skipped.
- Under Steam, `pgrep` does not list `steam_osx`, so the runner skips its "is Steam
  running" check for games Steam starts.
- Half-Life: Alyx still goes to `run-alyx-xrizer.zsh` with its rig-prefix install, not the
  copy Steam installs.
- Steam client updates: the gate depends on the `"linux"` comparison keeping its shape.
  `install-steam-play.zsh` and the test check it without starting Steam.
- 64-bit games only, as the runtime (`tools/steam_games/check_launch.py`).

# Windows Steam games from macOS Steam, without Windows Steam

macOS Steam cannot install or start Windows games: its Steam Play support is present but
switched off. This sets up the parts around it instead, without modifying Steam:

1. **Install** the Windows build with Valve's SteamCMD into the prefix's Steam library,
   where Windows Steam would put it.
2. **Start** it through the macOS Steam client: Proton's `lsteamclient` and the
   `mwxr-steam.exe` stand-in ([lsteamclient.md](lsteamclient.md)), with OpenXR through
   the in-process wineopenxr and OpenVR through xrizer.
3. **Add** a small launcher app to macOS Steam as a non-Steam game. Steam also tracks the
   running game as the real app (its process is registered under the app ID).

On Linux, Steam does all three itself: a compatibility tool (Proton) chosen per game
makes Steam download the Windows depots into `steamapps/common`, keep a prefix in
`steamapps/compatdata/<appid>`, and launch `proton waitforexitandrun game.exe`, whose
`steam_helper` and `lsteamclient` play the parts of the stand-in and lsteamclient here.
NotProton switches macOS Steam's copy of that machinery on by injecting a library that
hooks Steam's own functions, found by byte signatures per Steam build; that is not
attempted here.

## Layout: shared where practical, cloned where not

| What | Where | How many copies |
| --- | --- | --- |
| Games installed with SteamCMD | `~/Windows/library/steamapps/common/<installdir>` | one, used by every prefix (as `Z:`) |
| Windows Steam's `steamclient64.dll`/`steamclient.dll` (loaded by `steam_api`, never run) | `~/Windows/library/steam-client` | one; copied once from the rig prefix's Steam |
| Wine runtime with DXMT and lsteamclient, wineopenxr, xrizer | the runtime, `build-in-process`, `~/Windows/openvr-runtimes` | one |
| A game's prefix (registry, `users`, Wine's system32 copies, about 0.9 GB) | `~/Windows/prefixes/<appid>` | one per game, an APFS clone of `~/Windows/prefixes/template` |
| DXMT's and lsteamclient's DLLs in each system32 | synced from the runtime at every launch | APFS clones of the runtime's files |

Wine's system32 copies cannot be shared by symlinks: Wine's own prefix updates write
through them and would overwrite the shared runtime. Clones share blocks until a game
changes a file; `du` counts every clone in full, Finder's Get Info and `diskutil apfs` do
not. Games already installed inside the rig prefix (`~/Windows/prefix`, by Windows
Steam) keep using that prefix, so their saves and settings stay where they are; new
installs go to the shared library and get their own prefix.

## Use

```sh
python3 tools/steam_games/steam_games.py library              # VR games in your Steam library with a Windows build (offline)
python3 tools/steam_games/steam_games.py library --all --json # every Windows game, as JSON for a launcher
python3 tools/steam_games/steam_games.py list                 # installed games, and which library holds them
python3 tools/steam_games/steam_games.py info 1256230         # name, folder, Windows launch entries (anonymous)
python3 tools/steam_games/steam_games.py install 1256230 [--progress]   # Windows 64-bit build into the shared library
~/Windows/run-windows-game.zsh 1256230                        # start it on macOS Steam (this rig)
scripts/make-game-app.zsh 1256230                             # ~/Applications/Hyperbolica (Windows).app
scripts/game-prefix.zsh list                                  # the game prefixes
```

- `library` reads macOS Steam's own caches: `appcache/appinfo.vdf` (Valve's app info for
  every app the client knows, binary KeyValues format 29) and `appcache/librarycache`
  (artwork for each app in your library). No sign-in and no network. A game is VR when
  its app info sets `openvrsupport`, `openxrsupport` or `onlyvrsupport`, or it has a VR
  launch entry. Each entry has the cover (`library_600x900.jpg`) and hero image paths,
  whether it is installed and where, and whether it also has a macOS build.
- `install` uses SteamCMD (`STEAMCMD`, default `~/steamcmd/steamcmd.sh`) with its cached
  login (`STEAM_USER`, default the most recent account in macOS Steam); sign in once
  yourself with `steamcmd.sh +login NAME` for the password and Steam Guard. It runs
  `+@sSteamCmdForcePlatformType windows +@sSteamCmdForcePlatformBitness 64
  +force_install_dir ~/Windows/library/steamapps/common/<installdir> +app_update APPID`.
  With `--progress` it prints one JSON object per line: `start`, then `progress` (state,
  percent, done and total bytes, from SteamCMD's "Update state" lines), then `result`
  (`ok` or `error` with SteamCMD's message). Whether a SteamCMD sign-in signs macOS Steam
  out, as Windows Steam does, is not yet tested.
- `info`, `launch-spec` and `list` keep each game's Windows launch entries in
  `~/Library/Application Support/macos-wine-xr/games/<appid>.json`; game folders are not
  touched.
- `run-windows-game.zsh` (`scripts/`, with a rig wrapper in `~/Windows`) uses the app's VR
  launch entry where it has one (`GAME_MODE=default` for the other) and the game's own
  prefix for games in the shared library (`GAME_PREFIX=pergame|rig` to choose), clones
  that prefix from the template on first use (`scripts/game-prefix.zsh`), starts the game
  from its working folder through the stand-in, and points the prefix's OpenVR path files
  at xrizer for the run, putting them back on exit. Stopping it (SIGTERM, SIGINT or SIGHUP,
  as a launcher's Stop does) closes the game: its prefix's wineserver for a per-game
  prefix, or the game's processes in the rig prefix. Half-Life: Alyx goes to
  `run-alyx-xrizer.zsh` with `ALYX_STEAM=macos` and its adaptive fidelity table.
  `GAME_DRY_RUN=1` prints the launch. Logs go to `~/Library/Logs/macos-wine-xr/`.
- The app runs the rig's launcher (`GAME_RUNNER`). In macOS Steam: Games > Add a Non-Steam
  Game to My Library, and choose it from Applications.

Needs macOS Steam running and signed in, Windows Steam not running (one account, one
client), and the runtime set up by `scripts/install-lsteamclient.zsh`.

## Prefix updates and `dll*.tmp`

The launchers used to run `wineboot -u` on every start. When a DLL it would replace was
open in a running process (Windows Steam's, typically), Wine wrote a temporary copy
(`system32/dll*.tmp`) and queued the replacement for the prefix's next restart
(`PendingFileRenameOperations`), which a long-lived prefix rarely has. By 2026-10-27 the
rig prefix held 4,734 of them, 12.9 GB of its 13 GB system32; they were deleted and the
queue cleared (registry backup in `~/Windows/backups/system.reg.before-dll-tmp-purge-20261027`).
The launchers now run `wineboot --init` only for a new prefix and otherwise leave updates
to Wine, which updates a prefix itself when its runtime's `wine.inf` changes; so a copy of
a runtime made for testing must keep that file's time (`cp -cpR`, or `touch -r`).

## Tested, 2026-10-27

- `list` found the five games in the prefix (Assetto Corsa, SteamVR, Half-Life: Alyx,
  Audio Trip, Hyperbolica); `info` and `launch-spec` read Hyperbolica's entries
  (`Hyperbolica.exe`, and `Hyperbolica.exe startXR` for VR).
- `run-windows-game.zsh 1256230` (flat) started Hyperbolica through the stand-in; macOS
  Steam tracked it as app 1256230; the OpenVR path files were restored afterwards. In its
  own prefix (`GAME_PREFIX=pergame`, a test) it also loaded the macOS Steam client and was
  tracked, and stopping the launcher left no Wine processes.
- `library` listed 17 Windows VR games in the library, 4 of them installed.
- Apps made for Hyperbolica and Half-Life: Alyx. Not yet added to Steam or started from it,
  and no game installed with `install` yet (Half-Life: Alyx was installed the same way by
  hand on 2026-09-20).

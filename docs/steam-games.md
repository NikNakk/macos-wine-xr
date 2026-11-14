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
hooks Steam's own functions, found by byte signatures per Steam build.
[steam-play.md](steam-play.md) switches it on in memory instead, for chosen games, with
this rig's runner as the compatibility tool; Steam then does all three itself.

## Layout: shared where practical, cloned where not

| What | Where | How many copies |
| --- | --- | --- |
| Games installed with SteamCMD | `~/Windows/library/steamapps/common/<installdir>` | one, used by every prefix (as `Z:`) |
| Windows Steam's `steamclient64.dll`/`steamclient.dll` (loaded by `steam_api`, never run) | `~/Windows/library/steam-client` | one; copied once from the rig prefix's Steam |
| Wine runtime with DXMT and lsteamclient, wineopenxr, xrizer | the runtime, `build-in-process`, `~/Windows/openvr-runtimes` | one |
| A game's prefix (registry, `users`, saves; Wine's DLLs in `system32` and `syswow64`, about 1.4 GB) | `~/Windows/prefixes/<appid>` | one per game, an APFS clone of `~/Windows/prefixes/template` |
| Wine's built-in DLLs in each prefix, DXMT's and lsteamclient's among them | checked at every launch (`tools/steam_games/share_prefix_dlls.py`, 0.3 s) | APFS clones of the runtime's files, so about nothing per prefix |

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

## Wine's DLLs in each prefix

A prefix holds a copy of every Wine built-in it may load: `system32` for 64-bit programs,
`syswow64` for 32-bit ones, and their `drivers` folders, about 1.4 GB with WoW64. Wine
loads built-ins from the runtime; the copies are there so programs find the files. A
prefix cloned from the template shares those blocks with it, but Wine's own prefix
update, after any runtime change, writes them as new files, so every prefix used to end
up with a private copy after a Wine update.

`game-prefix.zsh` now runs `tools/steam_games/share_prefix_dlls.py` after its update
step (`ensure`, `upgrade`, and when making the template): each file that carries Wine's
built-in marker (`Wine builtin DLL` in its DOS stub) and has the same name in the runtime
becomes an APFS clone of the runtime's file. Files without the marker, the native DLLs a
game or installer put there (Microsoft's .NET in Assetto Corsa's prefix, for example), are
left alone. A clone keeps the runtime file's size and time, so later launches skip it
without reading it; a launch with nothing to share takes 0.3 s. Prefixes that are not game
prefixes (the rig's) are shared with `game-prefix.zsh upgrade PREFIX`.

On 2026-11-14 every game prefix, the template and the rig prefix were shared with
`runtime-unified` (1,388–1,464 MB each; 0–8 native files kept per prefix), and The Turing
Test with UEVR ran from a shared copy.

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


## Launcher compatibility and OpenXR loader provisioning

`run-windows-game.zsh` validates the selected PE launch entry before starting
Wine or changing its prefix. A 32-bit entry requires the selected Wine runtime
to contain `lib/wine/i386-windows/ntdll.dll`. Without it, the runner reports an
explicit compatibility error and keeps the normal Steam entry; it does not
substitute a different game executable. With the macOS Steam bridge selected, it also requires a 32-bit `lsteamclient.dll`.
This catches Assetto Corsa's 32-bit
`AssettoCorsa.exe` launcher on the current 64-bit-only rig runtime. Running its
64-bit race engine directly is a separate, explicit configuration decision.

`run-in-process-openxr.zsh` copies the Win64 Khronos loader from
`$MWXR_XRIZER_ROOT/openxr_loader.dll` (or `MWXR_WINDOWS_OPENXR_LOADER`) into the
prefix's `drive_c/windows/system32`. xrizer loads this DLL by name; placing it
beside `vrclient_x64.dll` alone does not make it discoverable by the game process.
This is separate from the native Mac loader linked by `wineopenxr.so`.

Run `python3 tests/windows/test_game_launch_preflight.py` for PE architecture and
runtime compatibility checks. On 2026-10-27, Audio Trip (826540) passed through
the rig runner after loader provisioning, created an OpenXR/Metal session,
rendered frames and was detected by SwiftXR Shell's external-session handoff.
The test was stopped through the runner's normal termination trap.


The WoW64 Steam bridge is now verified with live 32-bit and 64-bit API probes;
see [the build, installation and evidence](lsteamclient.md#wow64--32-bit-apps-2026-10-28).
Steam Play still uses the runtime selected by its rig wrapper. Select the
installed WoW64 runtime (`MWXR_IN_PROCESS_WINE`) to use its 32-bit side;
this does not itself install .NET or the game's redistributables. Existing
64-bit prefix templates need WoW64 initialization before use with 32-bit
launchers. The game-prefix sync copies the 32-bit bridge into `syswow64`.

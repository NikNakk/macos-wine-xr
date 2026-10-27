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

## Use

```sh
python3 tools/steam_games/steam_games.py list                 # games installed in the prefix
python3 tools/steam_games/steam_games.py info 1256230          # name, folder, Windows launch entries (anonymous)
python3 tools/steam_games/steam_games.py install 1256230      # Windows 64-bit build into the prefix
~/Windows/run-windows-game.zsh 1256230                        # start it on macOS Steam (this rig)
scripts/make-game-app.zsh 1256230                             # ~/Applications/Hyperbolica (Windows).app
```

- `install` uses SteamCMD (`STEAMCMD`, default `~/steamcmd/steamcmd.sh`) with its cached
  login (`STEAM_USER`, default the most recent account in macOS Steam); sign in once
  yourself with `steamcmd.sh +login NAME` for the password and Steam Guard. It runs
  `+@sSteamCmdForcePlatformType windows +@sSteamCmdForcePlatformBitness 64
  +force_install_dir <prefix>/drive_c/Program Files (x86)/Steam/steamapps/common/<installdir>
  +app_update APPID`.
- `info`, `launch-spec` and `list` read Valve's public app info with an anonymous SteamCMD
  login, and keep each game's Windows launch entries in
  `~/Library/Application Support/macos-wine-xr/games/<appid>.json`; game folders are not
  touched.
- `run-windows-game.zsh` (`scripts/`, with a rig wrapper in `~/Windows`) uses the app's VR
  launch entry where it has one (`GAME_MODE=default` for the other), starts it from its
  working folder through the stand-in, and points the prefix's OpenVR path files at
  xrizer for the run, putting them back on exit. Half-Life: Alyx goes to
  `run-alyx-xrizer.zsh` with `ALYX_STEAM=macos` and its adaptive fidelity table.
  `GAME_DRY_RUN=1` prints the launch. Logs go to `~/Library/Logs/macos-wine-xr/`.
- The app runs the rig's launcher (`GAME_RUNNER`). In macOS Steam: Games > Add a Non-Steam
  Game to My Library, and choose it from Applications.

Needs macOS Steam running and signed in, Windows Steam not running (one account, one
client), and the runtime and prefix set up by `scripts/install-lsteamclient.zsh`.

## Tested, 2026-10-27

- `list` found the five games in the prefix (Assetto Corsa, SteamVR, Half-Life: Alyx,
  Audio Trip, Hyperbolica); `info` and `launch-spec` read Hyperbolica's entries
  (`Hyperbolica.exe`, and `Hyperbolica.exe startXR` for VR).
- `run-windows-game.zsh 1256230` (flat) started Hyperbolica through the stand-in; macOS
  Steam tracked it as app 1256230; the OpenVR path files were restored afterwards.
- Apps made for Hyperbolica and Half-Life: Alyx. Not yet added to Steam or started from it,
  and no game installed with `install` yet (Half-Life: Alyx was installed the same way by
  hand on 2026-09-20).

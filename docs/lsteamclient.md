# macOS Steam for Windows games (lsteamclient)

Windows games in the rig's Wine prefix normally need a Windows Steam client running in
that prefix. Proton's `lsteamclient` lets them use the native Steam client instead: on
macOS, the macOS Steam client's `steamclient.dylib`. That removes the second Steam
(about 700 MB and 16% of a core while idle, with its web helpers waking wineserver about
100 times a second) and the session clash between them: one account can only be signed
in to one Steam client, so starting Windows Steam signs macOS Steam out
("Session Replaced") and the other way round.

## How it works

1. A game's `steam_api64.dll` finds Steam through
   `HKCU\Software\Valve\Steam\ActiveProcess`: `pid` must name a running Steam, and
   `SteamClientDll64` names the Windows `steamclient64.dll` it loads.
   `mwxr-steam.exe` (`src/windows/steam_stand_in.c`) records itself as that process,
   starts the game and waits for it, as Proton's `steam_helper` does.
2. With `MWXR_LSTEAMCLIENT=1`, Wine's loader maps that `steamclient64.dll` but never
   runs it: each export, and the entry point, jumps to the export of the same name in
   `lsteamclient.dll`, and its `tier0_s64`/`vstdlib_s64` imports go to ntdll. This is
   Proton's redirect (`ValveSoftware/wine`, `proton_11.0`, `dlls/ntdll/loader.c` and
   `unix/loader.c`), ported to CrossOver 26.3 as
   `patches/crossover/0007-ntdll-lsteamclient-redirect.patch` (x86_64 only, and opt-in
   so the same runtime still works with Windows Steam).
3. `lsteamclient.dll` (PE) and `lsteamclient.so` (unix) translate each Steamworks call to
   the host client, which they load from `$STEAM_COMPAT_CLIENT_INSTALL_PATH/steamclient.dylib`
   (`~/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS`); its
   x86_64 slice loads in the Rosetta Wine process and talks to the running macOS Steam.

## Sources and licences

- `lsteamclient` is Valve's, from `ValveSoftware/Proton` at a pinned `proton_11.0` commit,
  under the Steamworks SDK licence. It is fetched at build time and checked against a
  digest of the extracted files, not copied into this repository.
- `patches/lsteamclient/0001-macos-host-steamclient.patch` (this repository) makes it
  build and run against macOS Steam: the macOS `steamclient.dylib` does not export
  `Steam_IsKnownInterface` or `Steam_NotifyMissingInterface` (now optional), there are no
  X11 keysyms (keyboard glyph lookups report no native key code), and libc++'s headers
  must come before Wine's (`winbase.h` redefines `wcsncpy`, `windef.h` `min`/`max`).
- NotProton (`NotProtonNot/NotProton`) ports the same component to macOS for CrossOver
  and was read for orientation; no code is taken from it. It binary-patches CrossOver's
  ntdll and builds in a separate upstream Wine 11.15 tree; this builds in the rig's own
  CrossOver source with a source patch. Its other changes (64-bit `steamclient64.dll`
  lookup for interface memory, vtable copying, the overlay) are not needed for the games
  tested here.

## Build, install, run

```sh
scripts/build-lsteamclient.zsh            # lsteamclient.dll/.so and mwxr-steam.exe in build-in-process/lsteamclient
cp -cR ~/Code/wine-crossover/runtime ~/Code/wine-crossover/runtime-lsteamclient   # a separate runtime to test with
scripts/install-lsteamclient.zsh ~/Code/wine-crossover/runtime-lsteamclient ~/Windows/prefix
```

`build-lsteamclient.zsh` needs the CrossOver source with patch 0007 applied and its
ntdll rebuilt (`make dlls/ntdll/ntdll.so dlls/ntdll/x86_64-windows/ntdll.dll` with the
26.5 SDK). It registers `dlls/lsteamclient` in the build tree's `config.status` without
rerunning `configure`, builds the PE half with the tree's make, and compiles and links
the 219 C++ unix sources itself, because CrossOver 26.3's `makedep` (Wine 11.0) has no
C++ support. `install-lsteamclient.zsh` copies the DLLs and the patched ntdll into the
runtime (keeping `*.pre-lsteamclient` copies) and `lsteamclient.dll` into the prefix's
`system32`: once a prefix exists, Wine loads builtins only from there.

Then, with macOS Steam signed in and Windows Steam not running:

```sh
ALYX_STEAM=macos ~/Windows/run-alyx-xrizer.zsh           # Half-Life: Alyx on the headset
compare.py --variants wine-dxmt-macos-steam ...           # offscreen, tools/alyx_gpu_compare
```

Any other game: `MWXR_LSTEAMCLIENT=1 STEAM_COMPAT_CLIENT_INSTALL_PATH=... SteamAppId=N
wine mwxr-steam.exe -- 'C:\path\to\game.exe' [arguments]` with the runtime copy.

## Results, 2026-10-27

- **Hyperbolica** (Unity, `steam_api64`): the redirect loaded `lsteamclient`, which
  loaded macOS Steam's library, created a pipe, connected to the user and ran Steam's
  callbacks every frame (about 4,200 in 40 s, with callbacks and API call results
  delivered). macOS Steam tracked the Wine process as app 1256230 and released it on exit.
- **Half-Life: Alyx** (offscreen, 90 Hz, minimum frame period, adaptive fidelity,
  `~/Code/alyx-native/runs/macos-steam-20261027`): loaded the save and held 90 fps from
  10 s to 130 s; macOS Steam tracked it as app 546560. Windows Steam was not running.
- Both ran while macOS Steam was signed out ("Session Replaced" by Windows Steam that
  morning): stats requests failed with "No Connection" and Cloud did not sync. Signed-in
  behaviour (stats, achievements, Cloud, overlay) is not tested yet.

## Limits

- Not yet run on the headset, or with macOS Steam signed in.
- Steam Cloud: macOS Steam maps Windows save locations (`WinAppDataLocalLow` and so on)
  to macOS folders, not to the prefix, so Cloud saves do not follow the game's files.
- No Steam overlay; Steam Input keyboard glyphs have no native key codes.
- 64-bit games only (this CrossOver build has no 32-bit side).
- Games that relaunch themselves through Steam (`SteamAPI_RestartAppIfNecessary`) need
  `SteamAppId` set, as the launchers do.

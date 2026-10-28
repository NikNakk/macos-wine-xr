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
cp -cpR ~/Code/wine-crossover/runtime ~/Code/wine-crossover/runtime-lsteamclient   # a separate runtime to test with (-p keeps times: see steam-games.md)
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
- The original runtime is 64-bit only. The WoW64 build supports 32-bit Steam calls too; see below.
- Games that relaunch themselves through Steam (`SteamAPI_RestartAppIfNecessary`) need
  `SteamAppId` set, as the launchers do.


## WoW64 / 32-bit apps, 2026-10-28

The Steam bridge now builds both PE architectures against a WoW64 Wine tree.
Both use the same x86_64 native `lsteamclient.so` and macOS Steam library.
Proton already provides generated WoW64 interface and callback conversions;
this port enables them rather than introducing a second RPC transport.

Apply `patches/crossover/0008-ntdll-lsteamclient-wow64.patch` after 0007 in the
CrossOver source. It adds the 32-bit Steam DLL names and dependency redirects,
converts the two module handles at the WoW64 boundary, and writes x86 jumps
for PE32 modules. Crucially, PE32 must reserve the native `pe_module_loaded`
Unix-call slot: otherwise its redirect calls that stub and returns
`STATUS_NOT_IMPLEMENTED`. PE32 and PE32+ images must match their target's
architecture. Redirect failures now produce a loader warning.

```sh
# From ~/Code/wine-crossover/build-wow64, after applying 0008:
make -j6 dlls/ntdll/ntdll.so dlls/ntdll/x86_64-windows/ntdll.dll dlls/ntdll/i386-windows/ntdll.dll
# From the bridge checkout:
WINE_BUILD=~/Code/wine-crossover/build-wow64 OUT=$PWD/build-in-process/lsteamclient-wow64 scripts/build-lsteamclient.zsh
WINE_BUILD=~/Code/wine-crossover/build-wow64 OUT=$PWD/build-in-process/lsteamclient-wow64 scripts/install-lsteamclient.zsh RUNTIME PREFIX
```

Use a stopped WoW64 prefix. Installation updates both runtime and prefix
ntdll copies, the Win64 bridge in `system32`, and the Win32 bridge in
`syswow64`. Updating only the runtime leaves a prefix using stale ntdll code.
The installer rejects a 64-bit-only build for a WoW64 runtime. Existing
64-bit-only prefixes need their WoW64 system files initialized with the new
runtime first. `game-prefix.zsh` syncs both bridge DLLs; an old template also
needs rebuilding with the WoW64 runtime before cloning new game prefixes.
No 32-bit DXMT or XR DLL is provided by this change.

### Verified scope

Tested from bridge base `7ad96c7` plus this uncommitted change, CrossOver 26.3
WoW64, macOS 27.0.1, with the user's signed-in Steam Play client, app 244210.
The test used an APFS-cloned runtime and a private prefix under
`build-in-process/lsteamclient-wow64/test`; it did not change the game prefix
or the Steam Play wrapper's selected runtime.
After the passing tests, the bridge and matching ntdlls were also installed in
`~/Code/wine-crossover/runtime-wow64`, retaining installer backups. Both probes
were repeated against that installed runtime and returned 0.

Both `i686` and `x86_64` versions of `tests/windows/steam_bridge_probe.cpp`
returned 0 and verified:

- The actual Windows `steamclient[64].dll` redirects to `lsteamclient.dll`.
- Native Steam pipe/user connection, valid Steam ID, persona string, app ID
  244210, and `BLoggedOn() == true`.
- `UserStatsReceived_t`: successful result, matching app and user, including
  the macOS callback's compact 20-byte payload (Windows adds trailing padding).
- `GetNumberOfCurrentPlayers`: asynchronous completion and successful result
  retrieval through `ISteamUtils`, then clean pipe release.

The probe calls the explicit MSVC return-buffer ABI for `GetSteamID`;
MinGW's C++ small-class return convention otherwise produces a misleading
invalid ID even though Steam is signed in. Native identity values are not
printed. Each architecture received four callbacks, one stats callback.

Reproduce with an owned, installed app ID and an isolated installed prefix:

```sh
python3 scripts/test-lsteamclient.py --runtime RUNTIME --prefix PREFIX --steam-dir WINDOWS_STEAM_DIR --app-id 244210 --out build-in-process/lsteamclient-wow64/probes
```

The test terminates only the explicitly selected prefix's Wine server between
architectures. Exit 3 from the probe means callback/online checks were
incomplete; it is not evidence that the user is signed out. The user confirmed on 2026-10-28 that Assetto Corsa reaches the menu with the
WoW64/.NET setup; see [the rollout evidence](steam-play.md#assetto-corsa-wow64-rollout-2026-10-28).
Racing/VR, Steam Cloud save mapping, the overlay, networking callbacks and
arbitrary game compatibility remain unverified. Its 64-bit
race engine can continue using the existing 64-bit XR bridge.

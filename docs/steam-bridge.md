# Steam bridge: Windows games on the Mac Steam client

Date: 2026-10-19. **Built and tested without Wine or Steam (see Status). Not yet
run on a Mac.**

Windows games under Wine currently need a Windows Steam client signed in inside
the prefix: `steam.exe`, its Chromium web helper and the wineserver traffic they
generate, all under Rosetta. [steamvr-home.md](steamvr-home.md) records Alyx as
"still CPU-limited at 120 Hz", and Steam is CPU time the game does not get. It
also takes 30–40 s to start and has its own Wine problems (the black sign-in
window, the GPU web helper).

The Steam bridge removes it. A Wine builtin `steamclient64.dll` hands the game's
`steam_api64.dll` the Mac Steam client's own objects, from its
`steamclient.dylib` (x86-64 slice) loaded in the game's process. The game then
uses the Steam session signed in on the Mac: ownership, the user, networking,
achievements and cloud all come from the real client. Valve's and the game's
files are unchanged; nothing of Valve's is in this repository.

```text
game.exe -> steam_api64.dll -> steamclient64.dll (builtin, Windows half)
                                   | Wine unix call
                                   v
                               steamclient64.so (native half)
                                   | dlopen, System V calls
                                   v
                     Steam.app/.../steamclient.dylib -> Mac Steam (steam_osx)
```

## Origin

The approach is VeRantyx's ([Ag3497120/VeRantyx](https://github.com/Ag3497120/VeRantyx),
MIT, `VXRuntime/src/steambridge.c`). There it forwards Half-Life: Alyx to the
Mac client on VeRantyx's own Windows runtime. It passes every argument as an
integer, with hand-written exceptions for the slots Alyx needs. Under Wine it
has to be split into a Windows half and a native half. Proton's `lsteamclient`
does the same job for Linux, with generated code per interface. This
implementation takes from both, and its code is written here:

- **Methods are forwarded by vtable slot.** A proxy's vtable is 256 small stubs
  (`stubs.S`) that save every register and stack slot an argument can be in.
- **A table generated from the Steamworks headers** (`steam_methods.h`, written by
  `tools/generate_steam_bridge_table.py`) describes the methods that need more
  than integers: 212 interface versions and 358 methods across every SDK Proton
  carries. It replaces VeRantyx's hand-listed slots. The generated slots match
  every one VeRantyx hard-codes from Alyx runs (SteamUser `GetSteamID` 2,
  SteamNetworkingSockets 12/14/27, SteamNetworkingUtils `AllocateMessage` 0).
- **Calling conventions** (`bridge_core.c`, `sysv_invoke.S`):
  - Floating-point arguments move from Microsoft's positional `xmm` registers
    to System V's sequential ones. Stack arguments are kept in order.
  - `CSteamID`/`CGameID` returned by value go from System V's `rax` into
    Microsoft's hidden result pointer.
  - Methods that pass or return other structures by value are refused and
    logged, not guessed.
- **Calls from the client into Windows code are never made from the native side.**
  As in Proton, they are queued and run on the next Windows-side call:
  - warning and debug-output hooks get a copy of their text;
  - a networking message's free function gets a copy of the message;
  - steam_api's "is this callback registered" query always answers yes;
  - other function pointers are passed as NULL and logged.
- **SteamNetworkingMessage_t** release functions are swapped in both directions.
- **Callback layout:** the Mac client packs callback structures to 4 bytes,
  Windows to 8. As in VeRantyx, `SteamNetConnectionStatusChangedCallback_t`
  (1221) is converted.
- **Loading the client** saves and puts back Wine's signal handlers and macOS
  exception ports, which the client's crash reporter would otherwise take. The
  `Breakpad_*` exports do nothing.
- **`Steam_IsKnownInterface`:** the Mac client exports neither it nor
  `Steam_NotifyMissingInterface`, so the bridge answers from the version strings
  in the client's binary.
- **`SteamAppId`** is copied from the Windows environment into the native one,
  where the client reads it (Proton does the same).

`steam_standin.exe` stands in for the Steam process. It sets
`HKCU\Software\Valve\Steam\ActiveProcess` (`pid`, `ActiveUser`,
`SteamClientDll64`), and makes the named events and hidden "Steam" window that
Proton's `steam.exe` makes. It runs until `steam_standin.exe --stop`, then puts
the previous values back, so a Windows Steam in the same prefix still works
afterwards.

## Build

```zsh
MWXR_WINE_SDK=/path/to/wine-sdk MWXR_WINE_SOURCE=/path/to/wine-source \
  scripts/build-steam-bridge.zsh        # -> build-steam-bridge/
```

These are the same Wine SDK and source that `build-in-process-gate.zsh` uses.
The native half is x86-64, like the Wine it runs in.

## Run

The Mac Steam app must be running and signed in, and a Windows Steam must not
be running in the prefix.

```zsh
# Smoke test: CreateInterface, pipe and user, GetAppID, GetSteamID (a class by
# value) and GetLocalPingLocation (a float result) against the real client. It
# loads steamclient64.dll itself, so it needs no stand-in.
SteamAppId=546560 WINEPREFIX=/path/to/prefix WINEDLLPATH=$PWD/build-steam-bridge \
  WINEDEBUG=+steamclient /path/to/wine build-steam-bridge/steam_bridge_smoke.exe

# SteamVR + Alyx without Windows Steam: Alyx is started directly, not by steam.exe.
MWXR_STEAM_BRIDGE=1 APPID=546560 MWXR_MONADO=simulated scripts/run-steamvr-mwxr.zsh

# Alyx through xrizer and the in-process runtime.
MWXR_STEAM_BRIDGE=1 scripts/run-in-process-alyx.zsh
```

`WINEDEBUG=+steamclient` logs each interface (and whether the table describes
it), each method's first call, every callback id and size, and everything
refused.

## Status

Checked in a Linux container (no Wine, no Steam, no Mac):

- `tests/steam_bridge/abi_test.c`: a Microsoft-convention call into the real
  stubs, captured, planned and made into a System V function. It checks:
  - integers across all registers and the stack;
  - floats and doubles in registers and stack slots;
  - `CSteamID` through the hidden pointer, and a float result;
  - refusal of a structure returned by value;
  - the generated table against VeRantyx's slots.

  It passes with GCC and Clang at `-O0` and `-O2`.
- `tests/steam_bridge/native_test.c` exercises the native half against
  `fake_client.c`:
  - the crash handler the fake installs at load is put back;
  - lookup and calls through the object's vtable;
  - a hook queued with its text copied, and the same function getting the same slot;
  - a message free function getting a copy;
  - known interface versions, whole strings only.
- The Windows half compiles warning-free with MinGW against Wine 11.0 and 11.20
  headers, and links with all 41 exports and unwind data for the stubs. The
  stand-in and smoke test build.
- The table regenerates identically from Proton's headers.

`tests/steam_bridge/run_tests.sh` runs all of this. CI's `steam-bridge` job runs
it on Linux and macOS (on Apple silicon the x86-64 tests run only if Rosetta is
installed).

Not yet run, in order:

1. Build on the Mac; `steam_bridge_smoke.exe` under Wine against the signed-in client.
2. Wine's loader: that `steam_api64.dll`'s load of
   `C:\windows\system32\steamclient64.dll` gets the builtin and its native half
   from `WINEDLLPATH`.
3. SteamVR started without Windows Steam (VeRantyx runs it that way on its own
   runtime).
4. Alyx through `MWXR_STEAM_BRIDGE=1` with the simulated HMD. Then compare CPU
   (`top`: `steam.exe`, `steamwebhelper.exe`, `wineserver`, `hlvr.exe`) and the
   frame rate with and without it.

## Limits

- **Callback and call-result layouts:** only 1221 is converted. Other structures
  with an 8-byte member after an odd number of 4-byte ones are read wrongly by
  Windows code. Proton generates a converter for every structure; generating
  them from the same headers is the next step if a game needs more.
  `WINEDEBUG=+steamclient` lists the ids that arrive.
- **Game objects handed to the client** (the server browser's
  `ISteamMatchmaking*Response` callbacks) are refused: the client would call
  them through Windows vtables. Proton queues those calls; this could too.
- **Function pointers inside structures** (for example
  `FnSteamNetConnectionStatusChanged` set as a config value) are not converted.
- **Interface versions newer than Proton's headers** are forwarded as integers.
  The client may hand out such versions (VeRantyx's test asks for
  `SteamUtils011`).
- **No Steam overlay**: `gameoverlayrenderer64.dll` belongs to Windows Steam.
- **Saves and Steam Cloud** go through the Mac client for the app. Check where
  Alyx's saves land before relying on cloud sync.
- **Games with anti-cheat** are out of scope.

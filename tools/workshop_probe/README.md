<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# Isolated Steam Workshop metadata probe

This Windows x64 executable uses Home's `steam_api64.dll`, AppID 250820,
`STEAMUGC_INTERFACE_VERSION013` and `SteamUtils009`. It does not load Home,
OpenVR, the Wine XR proxy, or a Monado runtime. It queries item state and UGC
metadata, retrieves children, and traverses dependencies with cycle detection.
It never subscribes, downloads, publishes, or removes items. Steam can update its
ordinary metadata cache. The existing Steam client must be running and logged
in in the chosen Wine prefix, or on the native Windows PC.

The flat exports are loaded dynamically. Pack-8 data layouts and constants
follow Valve's public [UGC header](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/public/steam/isteamugc.h)
and [RemoteStorage header](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/public/steam/isteamremotestorage.h).
The [UGC documentation](https://partner.steamgames.com/doc/api/isteamugc)
describes the query and child APIs. Layout sizes are checked at compile time;
the tested DLL successfully returned both structures at runtime. New SDK/DLL
versions may require interface/layout changes.

From the Monado workspace, build:

```sh
mkdir -p .build/steamvr-workshop-probe
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -Werror \
  -Wno-cast-function-type -static \
  .build/in-process-openxr-study/tools/workshop_probe/main.cpp \
  -o .build/steamvr-workshop-probe/workshop-probe.exe
```

Start Steam in `.build/steamvr-dxmt/prefix`, then run:

```sh
WINEPREFIX="$PWD/.build/steamvr-dxmt/prefix" WINEDEBUG=-all \
SteamAppId=250820 SteamGameId=250820 \
.build/steamvr-dxmt/wine-11.10/bin/wine \
  .build/steamvr-workshop-probe/workshop-probe.exe \
  'C:\Program Files (x86)\Steam\steamapps\common\SteamVR\tools\steamvr_environments\game\bin\win64\steam_api64.dll' \
  0
```

## Native Windows comparison

From the repository root, build in an **x64 Native Tools Command Prompt for
Visual Studio** (Desktop development with C++ tools):

```bat
cl /nologo /std:c++17 /EHsc /W4 tools\workshop_probe\main.cpp /Fe:workshop-probe.exe
```

Alternatively, an x64 MinGW installation can compile `main.cpp` with the
C++17 command above, using paths relative to this repository.

With Steam running and logged in, run in PowerShell from the repository root:

```powershell
$env:SteamAppId = "250820"
$env:SteamGameId = "250820"
$homeRoot = "C:\Program Files (x86)\Steam\steamapps\common\SteamVR\tools\steamvr_environments\game"
$apiDll = Join-Path $homeRoot "bin\win64\steam_api64.dll"
.\workshop-probe.exe $apiDll 0 | Tee-Object workshop-fresh.log
.\workshop-probe.exe $apiDll 3600 | Tee-Object workshop-cache.log
Get-FileHash $apiDll -Algorithm SHA256
Get-FileHash (Join-Path $homeRoot "steamtours\bin\win64\client.dll") -Algorithm SHA256
```

Adjust `$homeRoot` for the PC's Steam library. Home does not need to be running
for the metadata probe. Compare hashes, child IDs, item states, subscription
count and callback cache flags with the Wine evidence; matching metadata alone
does not establish matching Home traversal behaviour.

Argument 2 is maximum metadata-cache age in seconds: `0` requests fresh
metadata; `3600` permits cached metadata. Additional arguments are published
file IDs. Defaults are 3149046643 and 2289310332. Requests are sequential with
30-second timeouts; traversal is limited to 128 items and 4096 children per
item. Exit 0 means the query/graph inspection succeeded, including when a
cycle was found; exit 2 means initialization, query, or a safety bound failed.
`failure_reason=-1` means no I/O failure was reported, so the failure-reason
API was not called. `cached` comes from Steam's completion callback.

## Recorded result, 2026-10-07

Wine 11.10, same prefix/API DLL as Home's captured stack overflow:

- Fresh calls returned success, `cached=0`, reciprocal child IDs, state flags 0,
  and zero subscribed items. First run: 279/362 ms.
- Cache-allowed calls returned the same graph, `cached=1`, in 9/3 ms.
- A second fresh run verified the finalized executable.
- The probe reports `CYCLE -> 2289310332 -> 3149046643 -> 2289310332` and
  exits normally. Steam itself does not recurse or hang on these queries.

Evidence: `.build/steamvr-workshop-probe/{fresh.log,cache-allowed.log,fresh-repeat.log}`
in the Monado workspace. This isolates the graph data from Home's application
recursion; it does not exercise downloads or assert how every application
should interpret dependency cycles. See `docs/steamvr-home.md` for Home's
stack-overflow and exclusion evidence.

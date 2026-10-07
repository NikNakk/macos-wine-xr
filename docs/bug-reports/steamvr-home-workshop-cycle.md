<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# Draft: SteamVR Home recurses indefinitely through cyclic Workshop dependencies

**Status: prepared, not submitted.** Do not submit this draft without the user's
explicit instruction. Native Windows details below are user-reported; the
captured exception and disassembly evidence come from Wine.

## Proposed Valve report

SteamVR Home can enter unbounded recursion when processing two Workshop items
that declare each other as required dependencies. Home stops submitting frames;
in a captured Wine run the original main thread raised `STATUS_STACK_OVERFLOW`
(`0xc00000fd`). The user also reproduced a Home crash on native Windows.
Downloading both resources first did not eliminate the recursive cycle in the
Wine test. Temporarily excluding the two items restored continuous submission.
A generic traversal-path guard passes a real Windows debugger fixture, but
its live Home cycle-rejection acceptance test remains pending because current
metadata requests time out. It must not be presented as a verified Home fix.

Affected content:

- [Aperture Focus Glass Environment — 2289310332](https://steamcommunity.com/sharedfiles/filedetails/?id=2289310332)
  requires 3149046643.
- [Aperture Focus Glass Assets — 3149046643](https://steamcommunity.com/sharedfiles/filedetails/?id=3149046643)
  requires 2289310332.

Both the public item pages and independent `ISteamUGC` queries return those
reciprocal edges. Fresh queries return success with `cached=0`; cache-allowed
queries return the same graph with `cached=1`. The standalone metadata probe
completes normally and reports the cycle, without loading Home or a VR runtime.

### Environment

Captured runs: SteamVR build ID **25330290**, Windows x64 Home binaries under
Wine **11.10** on Apple Silicon macOS. Steam client version **1788652215**.
VR transport used a development OpenXR SteamVR driver with simulated Monado
tracking/presentation; no physical headset rendering was assessed in these runs.

Native Windows reproduction: user reports Home crashing on an XPS13 with
PS VR2 tracking and a null display. That setup initially booted Home
successfully. The Windows crash dump, exception code, binary hashes and exact
reproduction sequence have not yet been incorporated; this draft does not
assert the native Windows crash has a captured identical stack.

Home `client.dll` SHA-256:
`c331ca4fc37723dd496d06930049a5f847433aa169b70943a4511b080fa16f96`

Home `steam_api64.dll` SHA-256:
`e4971706e3b96c8cfadffffb830ff0428835ef021b83837f7b0d107ae886752d`

### Reproduction and observations

1. Run the included standalone `tools/workshop_probe` with AppID 250820,
   Home's API DLL, and the two published IDs. Observe reciprocal child IDs.
2. Launch Home in a configuration that processes both items' dependency
   metadata. In our Wine prefix there were no subscriptions; the triggering
   metadata was encountered during Home startup/browsing, not by explicitly
   launching either environment. The precise UI/query conditions should be
   narrowed for a portable native Windows reproduction.
3. Observe repeated `client.dll+0x4f1a1a` frames on the original main thread.

The captured function is
`CWorkshopContentManager::GetItemStateAndDownloadIfRequested`, at
`client.dll+0x4f14f0`, identified by its referenced assertion strings and
`steamtours_workshop.cpp`. It calls itself at `+0x4f1a15` while iterating a
cached dependency vector; `+0x4f1a1a` is the recursive return address.

The first decisive trace captured **8,008** recursive frames, 128 bytes apart,
with the same manager pointer and alternating published IDs. The original main
thread then raised first-chance `0xc00000fd`; there was no preceding forced
termination call for that thread. Subsequent Wine/Rosetta exception handling
failed, so the final thread-exit reporting is not established.

After preinstallation, Steam returned successful download callbacks and state
**0x4** (installed) for both resources, with `GetItemInstallInfo` confirming
102,539,125 and 509,098,326 bytes respectively. Home briefly submitted 146
frames per eye before re-entering the same cycle. A stack read found **1,096**
recursive frames alternating the same IDs. We stopped this run before another
stack-overflow exception, so this is evidence of recurring recursion rather
than a second captured overflow.

A process-memory exclusion of the two IDs allowed **77,334 submissions per eye**
in logged windows, without Submit errors. This establishes a working diagnostic
workaround, not physical display quality or general SteamVR compatibility.

### Expected behaviour / proposed fix

Home should detect an item already on the active dependency traversal path and
stop following that edge. An unresolved/invalid dependency result and a useful
log message would be preferable to recursive stack exhaustion. A visited-node
cache can additionally avoid repeated work on shared acyclic dependencies;
shared dependencies alone should not be treated as cycles.

Steam documents non-collection item dependencies as soft metadata exposed to
applications. Whatever content validation occurs upstream, the traversal
should tolerate a cyclic response.

### Suggested attachments

- Standalone metadata probe source and run instructions from this repository.
- A short sanitized metadata transcript: each item has one child, namely the
  other ID; fresh and cached queries both succeed.
- A representative recursive stack and the captured `0xc00000fd` event.
- The decoded alternating-ID stack evidence and binary hashes above.
- Native Windows dump/reproduction details when available.

Full raw logs remain local in `.build/steamvr-home-termination`,
`.build/steamvr-workshop-probe`, `.build/steamvr-preinstall-test` and
`.build/steamvr-cycle-guard` in the Monado workspace. They are not bundled with
this draft and should be checked for account identifiers/local paths before
sharing. The companion `docs/steamvr-home.md` records the test conditions.

## Proposed item-author message

Your Aperture Focus Glass Environment (2289310332) lists Aperture Focus Glass
Assets (3149046643) as required, and the Assets item lists the Environment back
as required. Steam's UGC API returns the same two-way dependency.

We have captured SteamVR Home recursively following these edges until its main
thread overflows its stack. Pre-downloading both resources did not prevent
that recursion in our Wine test, and we have also reproduced a Home crash on
Windows.

Could you check whether the Assets → Environment requirement is necessary?
If the assets are independent, removing that reverse requirement while keeping
Environment → Assets would break the cycle. If both requirements are needed,
we should instead clarify the content layout and report Home's missing cycle
handling to Valve.

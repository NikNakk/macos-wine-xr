# SteamVR Home GPU timing under DXMT, 2026-10-14

Diagnostics used to find why SteamVR reported zero pre-submit GPU time for
SteamVR Home. They are tied to the installed SteamVR build
(`vrclient_x64.dll`, checked by code bytes) and are kept here rather than
in the shim:

- `shim-vrclient-probes.patch`, against `tools/openvr_shim/openvr_shim.cpp`
  at `3a84a53`: reads SteamVR's compositor-client timing state (global at
  RVA 0x49cd98, CGpuTiming at +0xd68, device at +0xe00, pre/post-submit ms
  at +0x130c/+0x1310), scans for client objects, and with
  `MWXR_OPENVR_SHIM_TRACE_SUBMIT=1` places one-shot int3 breakpoints on the
  648 basic blocks of the shared-texture Submit function (RVA 0x153d20),
  handled by a vectored exception handler that records first hits in order.
- `submit_trace_blocks.h`: the generated block list.

The trace showed Home's Submit taking the D3D10 branch (internal API code
2) because DXMT's D3D11 textures answered `ID3D10Texture2D`. SteamVR's D3D10
path never initialises its GPU timing. DXMT `8624164` (branch
`claude/timestamp-queries`) exposes D3D10 interfaces only on resources of
D3D10 devices; with it, SteamVR reported pre-submit GPU of 6–8 ms and Home's
auto-fidelity settled at level 0 at about 114 frames/s (run
`run-mwxr-20261014-083339`). See Monado
`doc/macos-steamvr-keyed-ownership-followup.md`.

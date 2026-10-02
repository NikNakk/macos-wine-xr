<!-- Copyright 2026, Nick Kennedy -->
<!-- SPDX-License-Identifier: BSL-1.0 -->

# Native IPC boundary verification, 2026-10-02

Bridge migration commit: `2e46673`. Native Monado:
`6171ca5b46cbb9d154731586ab797880f969e9fc` on `macos-upstream-clean`, including
the user's follow-up bootstrap-helper deletion and original Unix receive restore.
The first bridge gate passed against unchanged native Monado `7fd7f2835` before
transitional protocol deletion. No unexpected native sender was found.

The proxy owns Wine framing, shared-memory snapshots and layer upload staging.
Nine transitional commands are gone from native Monado; native command IDs are
regenerated (137 commands become 128), while Wine IDs remain frozen. The proxy
maps the native fd, writes layer metadata into native slots and consumes native
sync replies. Textures and shared events continue through the existing XPC
publish functions and native token imports.

## Results

- Native Monado: build and all 35 macOS CTest tests pass; contribution style and
  REUSE lint pass. Linux, Windows, Android, macOS and contribution CI pass at
  `6171ca5b4`.
- Proxy: build and all six CTest tests pass, including fragmented Wine requests,
  fd mapping, native slots/sync, variable-length replies, command-ID regeneration
  and rejection of GPU-blit/missing/mixed Monado sharing paths.
- Wire checker: 136 Wine/frontend commands and 124 forwarded native schemas
  checked against the real generated headers; local transitional commands are
  excluded from forwarding.
- Production proxy with synthetic HMD: hello_xr exits 0; explicit D3D11 2D and
  array-size-2 swapchains import/acquire/wait/release all three images each.
- Public generic-host runner with the same synthetic Monado service: hello_xr
  exits 0 after 589 frames; both swapchains select shared Metal (four images each).
- GPU pattern probe: Monado 2D and array-size-2, all four images per swapchain,
  zero pixel mismatches. Meta XR Simulator: hello_xr exits 0 after 200 frames,
  existing GPU-blit fallback; both array sizes and all three images pass the
  GPU pattern check.
- Timing analyzer successfully reads `ipc_submit` and `ipc_swapchain` traces.
  Synthetic runs have no PS VR2 presentation traces.

All simulator runs enable `MTL_DEBUG_LAYER=1`. One final native-host attempt
returned `XR_ERROR_INSTANCE_LOST` during release after 216 frames, with a
roughly two-minute gap in its log. A repeat using `caffeinate -i` passed without
source changes. This records the observed timeout; it does not establish its
cause. The temporary synthetic LaunchAgent registration was restored afterward.

## Zero-copy evidence

Successful production proxy log excerpts:

```text
proxy: Monado headers revision=6171ca5b46cbb9d154731586ab797880f969e9fc protocol-sha256=ad72c52786baffa4e53855736237a47d307a6a264c424ac76ca6cea7f86e2124
proxy: native-shm mapped; layer metadata uses native shared-memory slots
proxy: shared-event import path=native-token
proxy: texture import images=3 path=shared-metal-zero-copy pixel-copies=0 gpu-blits=0
host: swapchain=131072 images=4 strategy=shared-metal-zero-copy
host: swapchain=131073 images=4 strategy=shared-metal-zero-copy
verify image=0 array=1 mismatches=0
verify image=3 array=2 mismatches=0
native-to-D3D11: 2D/array/all-runtime-images verified on GPU
```

The production proxy object's undefined symbols contain the existing texture
and shared-event resolve/publish functions and no copy/blit/command-queue API.
Pixel-import implementations are unchanged apart from selecting generated native
IDs and reporting successful imports. Layer staging copies only CPU metadata.
The GPU pattern probe's rendering/readback workload is test instrumentation;
Meta's existing fallback blits remain separate from the Monado handoff.

Local raw logs/traces are retained under the native checkout's
`.build/proxy-boundary-validation/`: `proxy.log`, `validation-summary.log`,
`monado-generic-host.log`, `monado-gpu-pattern.log`, `meta-validation-summary.log`,
`meta-gpu-pattern.log`, `timing-analysis.log` and the preserved interrupted logs.
These generated logs are not checked in.

## Reproduction

Build proxy and service against the same native commit. Reconfigure the proxy
with `-DMONADO_HEADER_REVISION=$(git -C "$MONADO_SOURCE_DIR" rev-parse HEAD)`
after changing that commit. Use the public `run-proxy.zsh` and
`run-hello-xr-current-dxmt.zsh` scripts for the proxy path. Select the simulated
HMD with `SIMULATED_ENABLE=1` and a service build with the simulated driver;
register that service's XPC broker if it differs from the development LaunchAgent.
For generic-host runs select the native runtime with `XR_RUNTIME_JSON` and set
`MWXR_EXPECT_SHARING_PATH=shared-metal-zero-copy` when using Monado. Run the
`dxmt_reverse_import_probe` with the private Wine runner, matching Win64 probe
and native Khronos loader for GPU image verification.

PS VR2 visual output, pacing and controller hardware regression remain pending.

## PID ABI follow-up

Proxy commit `a9abcf2` adds frozen Wine client-description/app-state aggregates
with `int64_t` PIDs, and converts `instance_describe_client` requests and
`system_get_client_info` replies field by field. Unrepresentable native PIDs are
rejected locally. Static layout assertions preserve the Wine offsets. Neither
aggregate is in shared memory, and no image path changes are involved.

Native Monado `0f919ce71f7b71c997d7ef22abffbbaadb9cce5f` restores upstream
`pid_t`, its client assignment and PID logging format, removes the redundant
macOS `unistd.h` include and restores the upstream author address. The per-session
minimum-frame-period flag remains for the transitional proxy; the contribution
note explicitly excludes its enum/field, pacing vtable entry, compositor hook
and OpenVR initializer from the upstream series.

All 35 native tests, contribution style and REUSE pass. All six proxy tests pass
against both the preceding native layout and the restored `pid_t` layout,
including description conversion, app-state flags, failed replies and oversized
PID rejection. The wire compatibility checker passes. End-to-end synthetic
Monado: proxy hello_xr exits 0; both 2D/array swapchains pass with three images;
generic native host exits 0 after 564 shared-metal-zero-copy frames. All eight GPU
pattern image checks (four images for each array size) report zero mismatches.

The temporary launchd job initially stalled before main while dyld resolved
libraries (captured startup sample), causing socket-startup timeouts. A diagnostic
rerun started successfully and passed without source changes. The original
LaunchAgent registration was restored. Follow-up raw logs are retained as
`pid-validation-summary.log`, `pid-native-ctest.log`, `pid-style.log` and
`pid-reuse.log` in the same local evidence directory.

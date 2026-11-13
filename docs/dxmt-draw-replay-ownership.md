# DXMT bounded draw capture: ownership gate

2026-11-09. Private local experiment on Apple M5 / macOS 27.0.1 (26A434), pinned fork `4c3a2f5ea661bd258c14cf8ccb6924791cc75a77` plus the fourteen local patches. No upstream checkout changes, game modifications, installed Steam settings or EXD launch in this stage.

## Result and boundary

A controlled immediate-context D3D11 draw can now be captured **inside DXMT**, replayed from owned commands/resources, and produce bit-identical conventional colour/depth/stencil output. This is the ownership prerequisite for the [previous MSAA materialisation experiment](dxmt-msaa-source-foveation.md). It does **not** yet replace an application pass with foveated rendering, select EXD passes, or establish a performance gain for capture/replay.

[Patch 14](../patches/dxmt/0014-bounded-draw-replay-ownership.patch) adds a separate private diagnostic interface. `DXMT_REPLAY_CAPTURE=1` permits `IDXMTReplayProbe::CaptureNextDraw(budget, &packet)`. The next supported immediate `Draw(count,start)` snapshots its current state and inputs immediately before its normal draw. The application draw continues normally. The completed packet returns an ordinary D3D11 command list and separate conventional output textures. The diagnostic fixture explicitly arms capture; there is no automatic pass recogniser or game profile.

## Architecture reused and added

* Existing `MTLD3D11CommandList`, its owned CPU command allocator, deferred-context resource binding, pipeline compilation, execution and Reset/Recycle are reused. Metal encoder linked lists and transient argument-buffer pointers are not copied.
* Vertex buffers and VS/PS constant buffers are copied into private DEFAULT buffers in immediate GPU command order. Constant-buffer offsets/ranges are preserved with the existing Context1 calls. Copies capture buffer contents, including the allocation version preceding a later `WRITE_DISCARD`.
* Bound supported VS/PS sampled textures are copied into private textures. The original shaders, input layout, sampler/raster/blend/depth-stencil states and the private resources remain owned by a no-op closure in the finished list. Releasing the packet does not release these inputs while the list is still queued or replayable.
* Initial colour and optional combined depth/stencil are copied into immutable seeds. Every execution first copies those seeds into separate working outputs, then executes the captured draw. A previous replay or caller clear cannot become the next replay's initial contents.
* The context retains the pending packet only until the capture attempt; that pending packet initially owns no device/resources. Completed ownership is handed to the caller. There is no persistent ready-packet cache in the immediate context.
* GPU copies use the existing copy encoder/resource lifetime machinery. Capture adds no staging readback, polling, CPU/GPU wait or new synchronisation primitive. Test readbacks occur outside the capture implementation.

The fork's device mutex permits owner-thread recursive acquisition; the snapshot uses public D3D11 getters/copies under that mutex. Tests run with `ID3D11Multithread::SetMultithreadProtected(TRUE)`. Packet access/arming is an owner-thread diagnostic contract, not a concurrent consumer API.

## Deliberately bounded contract

The first packet accepts one non-indexed `Draw`, one colour attachment, VS/PS stages, supported vertex/constant buffers, and simple sampled textures. Colour formats are RGBA8_UNORM and R11G11B10_FLOAT, one mip, one array layer, ordinary 1× or native 2× samples. Optional combined D32Float/S8 is supported; correctness tests use R32G8X24_TYPELESS allocation with explicit depth/stencil views, matching the earlier EXD fixture. Typed D32Float/S8 allocation is accepted for copying but its stencil SRV interpretation is not validated here.

Additional shader stages, shader UAV use, bound OM UAVs, stream output, predication, active Begin/End query scopes, MRTs, incompatible views/resources and a context that has used explicit rate maps cause rejection. Existing automatic foveation also excludes capture. Indexed/instanced/indirect draws reaching PreDraw and DrawAuto reject a pending packet. Zero-count/invalid-primitive calls that return before PreDraw leave it pending. Unsupported resource construction can return its original failed HRESULT; successful ownership capture is reported only after the list has finished.

Resource limits are conservative: sampled inputs must be SRV-only single-sample RGBA8 2D textures with a single-mip view; buffer misc/structured flags, shared resources, texture arrays and nontrivial subresources are rejected. A caller-selected nominal allocation budget is capped at 64 MiB. The fixture supplies 8 MiB. This budget counts payload estimates for seed/working attachments and copied inputs; it excludes driver padding, pipelines/command storage and resident-memory effects. It is **not** a measured memory bound. Duplicated bindings can produce duplicated snapshots in this first implementation.

These restrictions are deliberately inadequate for an EXD frame. At EXD's audited 1792×1008 2× size, two colour plus two 8-byte combined depth/stencil allocations alone exceed the current cap. Raising it requires an explicit allocation/copy-cost experiment. This diagnostic must not be turned into a production selector by merely increasing that limit.

## Correctness validation

The fixture uses mutable dynamic vertex and constant buffers, a mutable sampled texture, a nonzero constant-buffer range, alpha discard, a partial triangle and scissor, preserved clear regions, depth writes and masked stencil replacement. HDR cases contain values above 1.0. The 2× cases have distinct sample planes; a collapse to resolved colour cannot satisfy the comparison.

Four cases cover RGBA8/HDR × 1×/2×. Each checks:

1. Three repeated replays after deliberately corrupting working colour/depth/stencil; every colour/depth/stencil sample matches the captured ordinary draw exactly.
2. Original inputs are mutated, dynamic buffers renamed, context state cleared and caller input/shader/state references released before replay. Original application attachments remain unchanged by replay.
3. A second packet is captured and its inputs mutated **without an intervening readback or flush**. The packet is released before execution and the returned command list is released immediately after queuing it, before GPU drain. Output remains exact.
4. `ExecuteCommandList(TRUE)` restores topology/viewport/attachment getters, and a separately bound ordinary draw after replay produces its expected image.
5. Insufficient budget, MRT, active occlusion query and instanced draw reject capture while the ordinary application draw still renders correctly. Pending results and duplicate-arm rejection are also checked.
6. With capture disabled, arming returns unsupported and the ordinary draw still renders.

Per format, the 1× case checks 65,536 replay sample tuples and the 2× case 131,072, across the three reset replays plus queued-mutation replay. These comparisons include all logical pixels, covered and untouched, rather than just the compressed or shaded region. This is a deterministic synthetic identity test; temporal, stereo, derivatives under compression and real-game correctness remain separate gates.

[Final summary](results/dxmt-draw-replay-20261109/summary.json) records the controls and nominal allocation totals (459,312 bytes at 1× and 852,528 at 2×). No capture GPU/CPU overhead or game FPS is claimed. The earlier 2× foveation gain cannot be carried forward without measuring the added copies and recording work.

## Next bounded experiment

Extend packet capture to an explicitly delimited small multi-draw pass with per-draw state/resource generations, including indexed arguments and the original masked coverage shaders. First require conventional replay identity. Then route that owned packet through the existing fixed rate-map / exact ordinary stencil replay / eager per-sample materialisation path and measure the **whole sequence**, including capture copies and CPU recording. Unsupported intervening clears, copies, updates, UAVs, aliases, queries and exports need explicit semantics or rejection, not silently omitted commands.

Automatic EXD recognition, pass replacement and gaze operation remain gated on that result. The existing native MSAA/rate-map/materialisation code is reused; this change adds no second foveation or gaze subsystem.

## Reproduce

```sh
DXMT_SOURCE_DIR="$PWD/build-in-process/dxmt-foveation-source" \
  scripts/build-dxmt-replay-probe.zsh
MWXR_IN_PROCESS_WINE="$PWD/build-in-process/exd-ngx/wine" \
  scripts/test-dxmt-replay.zsh
```

First build the private DXMT source with patch 14 and overlay its paired PE/Unix runtime components as in the existing experiments. The scripts use the prepared private probe Wine prefix and enable the Metal validation layer. The original EXD application and headset are not involved.

Regression checks on the new private build pass: all 10 automatic colour-foveation cases (including eight fallback/default controls), all 16 existing HDR per-sample materialisation cases, and six existing analysis tests. [Validation record](results/dxmt-draw-replay-20261109/validation.txt), [patch/source verification](results/dxmt-draw-replay-20261109/patch-validation.txt) and [source/build hashes](results/dxmt-draw-replay-20261109/sha256.json) are retained.

# Testing current DXMT native sharing

This is the first validation gate for replacing the old DXMT v0.80/Basalt path.

The test proves that **current DXMT** can expose one shared
`Texture2DArray` and one shared D3D11 fence to a native macOS process without
an IOSurface repack or Monado-specific DXMT patch.

## 1. Build current DXMT

Use `NikNakk/dxmt:macos-xr-native-sharing` and follow its
`docs/DEVELOPMENT.md` cross-build instructions. The important requirement is
that the Wine process loads the DLL/unixlib set built from that branch.

The branch's native-sharing modification is deliberately tiny: it republishes
DXMT's existing Metal bootstrap names through D3D11 private data.

## 2. Build the bridge probes

For the Windows side, configure with a Windows/MinGW toolchain and point
`DXMT_INCLUDE_DIR` at the fork's `include` directory. The resulting target is:

`dxmt_native_sharing_probe.exe`

For the native side on macOS:

```sh
cmake -S . -B build
cmake --build build --target native_metal_sharing_probe
```

## 3. Run the Windows producer under Wine/DXMT

Run `dxmt_native_sharing_probe.exe` in the Wine environment that loads the
current DXMT build. It creates:

- a 64×64 RGBA8, two-layer `Texture2DArray` with
  `D3D11_RESOURCE_MISC_SHARED_NTHANDLE`;
- a shared `ID3D11Fence`.

It prints two opaque names:

```text
texture=<bootstrap-name>
fence=<bootstrap-name>
resources are live; press Enter after the native macOS probe finishes
```

Leave that process running. The DXMT objects and their bootstrap registrations
must remain alive for the consumer test.

## 4. Resolve both objects natively

From a native shell in the **same Mach bootstrap namespace**, run:

```sh
./build/native_metal_sharing_probe '<texture-name>' '<fence-name>'
```

Success prints the reopened texture geometry and the shared event's current
`signaledValue`.

For this initial gate, expect the texture to report an array length of 2.

## Interpretation

If both objects resolve successfully, the old Basalt IOSurface export is not
needed for this class of XR swapchain. The next test is pixel correctness and
GPU ordering, followed by handing the resolved native Metal objects to Monado's
generic XPC/native-resource path.

If bootstrap lookup fails while the producer is still alive, first verify that
both processes really share a Mach bootstrap namespace. The longer-term bridge
architecture resolves the DXMT object in that namespace and then hands the
native Metal object onward, specifically so `monado-service` does not need to
be launched inside the Wine/DXMT namespace.

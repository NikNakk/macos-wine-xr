# Fixed PS VR2 foveation profile — 11 November 2026

Patch `0021-fixed-psvr2-profile.patch` adds an opt-in fixed profile independently
of gaze tracking. It changes only the private native Metal map constructor;
no ABI, shader or resource-read specialization changes are required. The generic
16×8 image-centred profile remains the default.

Select with `DXMT_STAY_WARPED_FIXED_PROFILE=psvr2` alongside the existing
`MWXR_EXD_STAY_WARPED=5600x2856`, peripheral rate and resource-read options.
The launcher inherits this environment variable. It must reach the Wine process
at startup, and requires the matching rebuilt native `winemetal.so`.
**The candidate has been built but is not installed in the experiment Wine copy**,
to avoid changing binaries during the parallel gaze work. The built candidate is
saved in `../build-in-process/exd-warp/fixed-psvr2-candidate/winemetal.so`.
Do not replace a newer gaze module with that snapshot; rebuild from the combined
source instead. No headset run was launched.

## Placement and tradeoff

The old profile centres full rate at eye texture U=0.5. Monado's PS VR2 FOV
(-61.5/+43.5 degrees left, mirrored right, vertical ±53 degrees) instead puts
straight ahead at U≈0.660/0.340. The new 64×32 descriptor samples angular
coordinates through these frusta. Each separable axis requests full rate within
±20 degrees, then smoothstep to the requested outer rate at ±35 degrees.
This is a fixed rectangular/separable protected region, not gaze tracking or a
circular angular map. Both axes must be inside the core for full-rate 2D shading.

At 5600×2856 / outer 0.3, Metal allocates 3280×1728 physical pixels (35.4% of
logical area), versus 2844×1488 (26.5%) for the old narrow misplaced core.
The wider correctly placed core spends more pixels deliberately. These are map
allocation ratios, not measured speedups or fragment counts.

## Validation

Native module compilation passes (four existing SDK managed-storage deprecation
warnings). The standalone `tests/native/fixed_psvr2_rate_map.m` uses the actual
profile header and Metal coordinate queries, with no GPU command buffers or
headset session. Three resolutions × four rates (0.3, 0.5, 0.75, 1.0) pass:

- Both eyes and both axes retain exactly 1.0 local density across ±15 degrees.
- Coordinate mappings are monotonic; all-1.0 mappings are identity.
- Peripheral ±40 degree probes remain reduced at sub-unity outer rates.
  At 5600×2856 / 0.3 their maximum is 0.30774. At 1792×1008 it rounds upward
  to 0.43243; physical tile rounding and edge padding prevent assuming the
  requested float rate is exact everywhere. The test permits up to +0.15.

Reproduce from this repository:

```sh
clang -x objective-c tests/native/fixed_psvr2_rate_map.m \
  -I build-in-process/dxmt-warp-source/src/winemetal/unix \
  -framework Foundation -framework Metal -o /tmp/fixed-psvr2-check
/tmp/fixed-psvr2-check
```

Results and candidate hashes are in `results/dxmt-fixed-psvr2-20261111`.
Worn image quality, ±20 degree boundary behaviour, application reconstruction
and performance remain unvalidated; no headset-quality claim follows from the
coordinate tests. This profile is specifically calibrated to Monado's current
PS VR2 FOV; other headsets should retain the generic profile.

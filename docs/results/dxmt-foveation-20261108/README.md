# DXMT foveation renderer proof

See [implementation and reproduction](../../dxmt-foveation.md).

- Base source: `4c3a2f5ea661bd258c14cf8ccb6924791cc75a77`.
- Runtime: private `build-in-process/wine-foveation`; prefix:
  `build-in-process/prefix-foveation`.
- Metal API validation enabled.
- Positive run: exit 0; 4 cases × 25 pixel checks passed.
- Negative control: exit 1; first sample mismatch detected with maps disabled.
- Installed runtime, Monado configuration and EXD files were not modified.

`positive.log` and `negative-control.log` contain the actual test output.
`manifest.json` records source identity and patch hash. Build logs include
warnings from existing code and Wine font-library warnings. The Metal compiler
component was unavailable: unchanged shader helpers from the base build were
reused; changed PE/native source was rebuilt. No performance result or live
gaze integration is claimed by this proof.

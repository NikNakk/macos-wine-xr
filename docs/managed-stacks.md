# Managed Threshold XR stacks

The shared contract is `threshold-xr/launch-stack-1`. Threshold XR writes it when a
user chooses **Components → Launch Stack**. It records the pinned release identities,
resolved artifact directories, native client paths and expected Monado build tag.
`framework_runner` identifies the adopted, private threshold-runtime installation.
Native hosts, ANGLE and game data remain external profile components. Selecting client
paths does not activate or restart Monado.

`THRESHOLD_LAUNCH_STACK` or `threshold-run --stack FILE` selects this contract.
Threshold Runtime applies its managed dependency paths to both profile listing and
launch planning, and records the stack's provenance in `run.json`. It refuses a
Monado service/client tag mismatch and detects a different running service passively.
The same selected file reaches framework launches from Threshold XR and Threshold Home.

## Public package and local compatibility runtime

The public package builder produces only our launcher, compat gate/scanner, scripts,
compatibility-tool definitions and the corresponding launcher sources. It does not
bundle Steam, Steamworks SDK files or lsteamclient. Public binary uploads are held in
the release workflow while the Steam client/SDK licensing questions are reviewed.
Building/downloading locally does not waive the applicable third-party terms.

```sh
zsh scripts/package-steam-play.zsh /absolute/new/package-directory
```

For local integration, first prepare a separate runtime from an explicitly supplied,
already working WoW64 Wine installation with both lsteamclient PE architectures and the
native bridge, plus a compatible in-process wineopenxr build. This is an adoption of
external components, not a public release or a claim that their redistribution is
permitted. No Steamworks source is downloaded by the preparation tool.

```sh
python3 tools/steam_play/prepare_runtime.py \
  --stack '/path/to/selected-launch-stack.json' \
  --wine-runtime '/path/to/existing/wine-with-lsteamclient' \
  --bridge-build '/path/to/existing/in-process-gate-build' \
  --windows-loader '/path/to/openxr_loader.dll' \
  --stand-in '/path/to/mwxr-steam.exe' \
  --output '/path/to/new/private-compatibility-runtime'
```

The tool checks the redirect markers, copies/clones the supplied runtime, overlays the
selected DXMT, copies the selected Windows xrizer and external loaders, and relocates
Mach-O dependencies in the combined root before signing them ad hoc. It refuses an
existing destination. It neither initializes a prefix nor writes to the supplied runtime.
Its new launch-stack file records external input hashes, the composed file inventory
and `build_attestation_verified: false`. This adoption does not prove ABI compatibility;
use a known-compatible supplied runtime/bridge pair and validate it before real games.

Install the prebuilt launcher beside existing installations:

```sh
python3 scripts/install-prebuilt-steam-play.py \
  --package '/path/to/package-directory' \
  --stack '/path/to/new/private-compatibility-runtime/launch-stack.json' \
  --destination '/path/to/new/Steam Play installation'
```

This creates `Threshold Steam Play.app` under the destination, a complete Steam Play
payload and a runner bound to a snapshot of the configuration. No compilation, Steam
restart, prefix initialization or existing app replacement occurs. Optional `--games`
copies existing mapping preferences without modifying their original. The app uses our
own bundle identity and no Valve icon. Prefixes are persistent data outside the versioned
runtime; their location can be explicitly supplied to the preparation tool.

After deliberately quitting Steam, the user can open this new app. The existing
compatibility mapping code and setup-script handling are retained. The managed runner
validates critical prepared-file hashes and matching installed Monado tags before
starting Wine. Native Alyx's `MWXR_ALYX=native` route uses the selected
`framework_runner` and `threshold-run alyx`, preserving extra arguments and forwarding
Steam's Stop to threshold-run, which stops and waits for the guest process group.
`MWXR_ALYX_COMPONENT_SET` selects a named profile component set. Legacy launch behavior
is preserved when no managed stack is configured.

The source-backed launcher package compiles locally. Stand-in tests cover composition,
changed-file rejection, side-by-side installation, paths containing spaces, argument
filtering and stop propagation. These are not real-game, Steam sign-in or headset tests.

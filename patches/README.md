# Patches

On 15 November 2026 the patch sets that used to live here became commits on forks, so each
build source has one copy with history. Only `lsteamclient/` remains: it patches Valve's
lsteamclient, which `scripts/build-lsteamclient.zsh` fetches from ValveSoftware/Proton at a
pinned commit and which is not redistributed here.

Each removed file is listed with its SHA-256, so results that cite a patch stay verifiable,
and the commit that now holds it. Older docs and result archives still name the files.

## DXMT foveation and diagnostics: `patches/dxmt/` → [NikNakk/dxmt](https://github.com/NikNakk/dxmt)

`macos-xr-foveation` is exactly the source of the foveation runtime (`build-in-process/wine-foveation-unified`): base `4c3a2f5` (on `claude/xr-frame-boundary`), one commit per patch, then the pass-capture replay probe, which existed only in `build-in-process/dxmt-warp-source` and never as a patch. `macos-xr-foveation-next` is the same work on upstream DXMT (on `xr-frame-boundary-next`); patch 0025 is already there as `9d338cb`, and the shader-converter commits were ported to upstream's rewritten converter.

| Patch | SHA-256 | Commit |
| --- | --- | --- |
| `0001-immediate-context-metal-rate-map.patch` | `537088ba2c033e7b3988e984fa84c94d3704dfd8d6253929d42a35bfededac26` | [`51ee88f291`](https://github.com/NikNakk/dxmt/commit/51ee88f2910c6748cb8171bc601e238a363bdd7e) (macos-xr-foveation), [`ef151ee6d8`](https://github.com/NikNakk/dxmt/commit/ef151ee6d8651c8ad337eb3ba752572fdbf2a695) (macos-xr-foveation-next) |
| `0002-target-specific-metal-rate-map.patch` | `2ed697426e42f207832514b56485931ec15baefec54400b1cfab23951069f217` | [`cb2e78ec40`](https://github.com/NikNakk/dxmt/commit/cb2e78ec407f3d43f2b4d02f8439942a77f8d992) (macos-xr-foveation), [`1358f2bb18`](https://github.com/NikNakk/dxmt/commit/1358f2bb18ef4f87252fbc4efe70b8a765f7f17b) (macos-xr-foveation-next) |
| `0003-ngx-feature-routing-and-trace.patch` | `245fe8bff3d243677cf8ddaf33f7d52d55cb94b9c0a2f35bddcab93ce0af5dc5` | [`6f2da76e12`](https://github.com/NikNakk/dxmt/commit/6f2da76e1202e6562c9e30f0a78f6164477b2a91) (macos-xr-foveation), [`c942ef4c85`](https://github.com/NikNakk/dxmt/commit/c942ef4c854094fa1f18ca3582e828160f7ab918) (macos-xr-foveation-next) |
| `0004-scene-resource-provenance-trace.patch` | `19efc86009c806ee0d5be3da901d3ab6ffa617f62d59e82f926e328402ce56d9` | [`81dae7b911`](https://github.com/NikNakk/dxmt/commit/81dae7b911b33657dfcd63eda5e701745bcef334) (macos-xr-foveation), [`77af1e6c96`](https://github.com/NikNakk/dxmt/commit/77af1e6c9647fd7961fcafeb64ca02a50e59870b) (macos-xr-foveation-next) |
| `0005-ngx-subrect-copy-and-validation.patch` | `b3a92c2d3b9dc104b762a5419c42175596b967ee238d8966db9499c55b969c39` | [`7b76a918d0`](https://github.com/NikNakk/dxmt/commit/7b76a918d009445b0cf689dd887f93f4ccf8328c) (macos-xr-foveation), [`51721eecb5`](https://github.com/NikNakk/dxmt/commit/51721eecb5137976ec4315d4827442343b4c6e47) (macos-xr-foveation-next) |
| `0006-rate-map-position-translation.patch` | `95ee640adadb1017d3b31ef93dbda4411dd391882bbdddcd0ba5059472df488f` | [`1559d60315`](https://github.com/NikNakk/dxmt/commit/1559d60315ff775d0b0e44cc6103e2bcb0ec83f5) (macos-xr-foveation), [`bd52052a6d`](https://github.com/NikNakk/dxmt/commit/bd52052a6d41c9443bda52d05b531a9d194cfb1e) (macos-xr-foveation-next) |
| `0007-bounded-automatic-base-foveation.patch` | `61c64d0718a311c2ff094e6e419ccc2cc76f86fefbc306681453763c3bd8beee` | [`77a36163bd`](https://github.com/NikNakk/dxmt/commit/77a36163bd5ca88cc9acb9f43d196ae5ed084abd) (macos-xr-foveation), [`abc3aad370`](https://github.com/NikNakk/dxmt/commit/abc3aad37027e957c244684a04d73771dca1da3e) (macos-xr-foveation-next) |
| `0008-explicit-depth-msaa-diagnostic.patch` | `3ac4e6da6d35572a245511a4c287178ba823b5e6ace60d9c61d9b32b58c089cd` | [`9db630fb99`](https://github.com/NikNakk/dxmt/commit/9db630fb99ab880cc6aeb30e35b92f2c574064a7) (macos-xr-foveation), [`8927c0217f`](https://github.com/NikNakk/dxmt/commit/8927c0217f2e9ef2f623daca6a8d828cb06c37cf) (macos-xr-foveation-next) |
| `0009-passive-tbdr-audit.patch` | `c04e2ea4e6b5c09e06c70e1826b7b504ab4cd4d92703c13fc59ebe819acad7cf` | [`20e26fa960`](https://github.com/NikNakk/dxmt/commit/20e26fa9604a430cc6462eca8b90eeddcc23ddb3) (macos-xr-foveation), [`814f05f9e4`](https://github.com/NikNakk/dxmt/commit/814f05f9e4daaad8e52eb3df3c9a168dc8fe4edc) (macos-xr-foveation-next) |
| `0010-depth-resolve-shader-diagnostics.patch` | `68cb0a23240c65e6d713c5841a7680654a0357042ff03de17519b5c89d5d898b` | [`3f64d23cf8`](https://github.com/NikNakk/dxmt/commit/3f64d23cf84973aaad6523a0a97e297f64d004bf) (macos-xr-foveation), [`e8a7196083`](https://github.com/NikNakk/dxmt/commit/e8a7196083ec5914989e8db82515bc2d818e80b7) (macos-xr-foveation-next) |
| `0011-explicit-depth-only-rate-map-diagnostic.patch` | `0668ea1da3dfbce4df143287bbcfaf332bbfe40bbb396a7c687d5dd2f5d3d1fb` | [`a4f94e5fa6`](https://github.com/NikNakk/dxmt/commit/a4f94e5fa669747c68560f7f0e5e5f25d0b9fc9b) (macos-xr-foveation), [`885d3c79ff`](https://github.com/NikNakk/dxmt/commit/885d3c79ff8c1cfd13f56df405f60e7a85be4d2a) (macos-xr-foveation-next) |
| `0012-per-draw-depth-stencil-audit.patch` | `81a8af2fca188d1b1b6c2217ed59d379ec18a6c96b26778cc1866b22720412fa` | [`5a166dfc40`](https://github.com/NikNakk/dxmt/commit/5a166dfc400d7db98569692da873d36cc7819f08) (macos-xr-foveation), [`07051a9281`](https://github.com/NikNakk/dxmt/commit/07051a9281d014f683df04f894cc735da52044fa) (macos-xr-foveation-next) |
| `0013-explicit-combined-stencil-diagnostic.patch` | `ab01814551e2966025088f99b15131ba998ae9ac1a980ab2c543d3dbc362e5a5` | [`9d9d09675b`](https://github.com/NikNakk/dxmt/commit/9d9d09675b7e8a78c8659296a5749901c0b31673) (macos-xr-foveation), [`efff1d0ad6`](https://github.com/NikNakk/dxmt/commit/efff1d0ad65e159037a00b74120fdd979239048d) (macos-xr-foveation-next) |
| `0014-bounded-draw-replay-ownership.patch` | `ab4b2faf141832d4973b6c6a2e3e7dd2b87204ed4e781340f5a6cae3bcbeed50` | [`b65c4dac0c`](https://github.com/NikNakk/dxmt/commit/b65c4dac0c97d1786773ddc50c0384ee59047a4f) (macos-xr-foveation), [`2bbd155d0b`](https://github.com/NikNakk/dxmt/commit/2bbd155d0b17551795bb53169c8ecc12544a1225) (macos-xr-foveation-next) |
| `0015-rate-map-read-translation.patch` | `7a14d857657ad58cfec1b27fb04e4d0195ec4e7b1c1dbbd4e2b0c62c38b12e6d` | [`1a9135b7b8`](https://github.com/NikNakk/dxmt/commit/1a9135b7b8c673394d636a3a741c9a390a54051b) (macos-xr-foveation), [`f5fd6b8d3f`](https://github.com/NikNakk/dxmt/commit/f5fd6b8d3fba8a6bde989a4e2bb587c64dbce08d) (macos-xr-foveation-next) |
| `0016-passive-layout-audit.patch` | `ae08503ca5e3dce580617cdcc4a490ef3d93a1ac7ed3670643e7685180e85a7c` | [`4949a399dc`](https://github.com/NikNakk/dxmt/commit/4949a399dcc1a8008624ae5452b10202afdbdd9b) (macos-xr-foveation), [`9ee55e87f6`](https://github.com/NikNakk/dxmt/commit/9ee55e87f69a38a87cee7764e12ed778632c04dc) (macos-xr-foveation-next) |
| `0017-automatic-stay-warped-stereo-profile.patch` | `3c217032d9276ef588fd72f5ac886ca7a31cfa695168781959bbd74a6a5bd210` | [`baea812998`](https://github.com/NikNakk/dxmt/commit/baea81299886cb91fa9ac92a4c51e791ef7238d9) (macos-xr-foveation), [`96ed3e5967`](https://github.com/NikNakk/dxmt/commit/96ed3e596716472d91efa631065687e05694cbec) (macos-xr-foveation-next) |
| `0018-no-map-shader-component-diagnostic.patch` | `fc650152181db9f0423ac6cf0f46075061c7076196467803909a1df646b2947d` | [`3e06e4bb22`](https://github.com/NikNakk/dxmt/commit/3e06e4bb22ae0b38454c607c3eab283f173e6e74) (macos-xr-foveation), [`cf01eb779f`](https://github.com/NikNakk/dxmt/commit/cf01eb779ff8e0f9e8f95103ee1b4f4b583f65fe) (macos-xr-foveation-next) |
| `0019-selective-read-shader-variants.patch` | `afea00921de90cfe9d444e64919851e34ed91b70587b54c72625d1d200a9eb54` | [`de214e1ffb`](https://github.com/NikNakk/dxmt/commit/de214e1ffbc8a4188b93afccddf8f9b6d185fb6c) (macos-xr-foveation), [`075cc4b531`](https://github.com/NikNakk/dxmt/commit/075cc4b531615b9c1f61aaba96fa5aaa96a5a582) (macos-xr-foveation-next) |
| `0020-resource-read-shader-variants.patch` | `df45fe8819b914a364ae7710c379eda9cb65f3c4be70410be13cb5d7151e1660` | [`713605973c`](https://github.com/NikNakk/dxmt/commit/713605973c0a4fdb53741ccff43d2cbebacb27d2) (macos-xr-foveation), [`6351da83bc`](https://github.com/NikNakk/dxmt/commit/6351da83bccefa52a29bcca5a6936161f9122da4) (macos-xr-foveation-next) |
| `0021-fixed-psvr2-profile.patch` | `db00f300b9523d0f5eb13b9a526d282ba32b05e8e4f65fef0cfc780f98097114` | [`a014d554c2`](https://github.com/NikNakk/dxmt/commit/a014d554c21f9aa049917dfe5604e6ecbf17bca4) (macos-xr-foveation), [`43ef599dc7`](https://github.com/NikNakk/dxmt/commit/43ef599dc71a0f1d81cd9a5a4eb4e4768cf6c79d) (macos-xr-foveation-next) |
| `0022-unwarp-copy-to-swapchain.patch` | `89270709cb541fe5b6ee5abf5b8fc8d96d21a43e292eef8b28acc5a25c4900b9` | [`24a9f1bf6b`](https://github.com/NikNakk/dxmt/commit/24a9f1bf6bbfb12b8c5daaa6adaa0ff655936fd9) (macos-xr-foveation), [`bbe1d012d4`](https://github.com/NikNakk/dxmt/commit/bbe1d012d4637acd0c046f827d2cc1899353c0c1) (macos-xr-foveation-next) |
| `0023-two-layer-eye-targets.patch` | `82fa207fb4b38412215da67eced6c26e81b4be2564038d3fabbdbd494e39f210` | [`77f09707b8`](https://github.com/NikNakk/dxmt/commit/77f09707b8bee5d2a673eec1282d7663a5be3eed) (macos-xr-foveation), [`306cb76417`](https://github.com/NikNakk/dxmt/commit/306cb76417bdb6cae1434629703d428ab21489ba) (macos-xr-foveation-next) |
| `0024-pass-timing-diagnostic.patch` | `c4cb759466220f535cbed5c33a6347feabc97bce125d4af78ca1494ec4b3bcae` | [`e7d6e1d19e`](https://github.com/NikNakk/dxmt/commit/e7d6e1d19eeacedba091b4416a4ee10f90cdc292) (macos-xr-foveation), [`2926ff0db3`](https://github.com/NikNakk/dxmt/commit/2926ff0db35692d1e1a473460859955b2abd93f8) (macos-xr-foveation-next) |
| `0025-desktop-window-swapchain-stand-in.patch` | `16f034b4cbe2bfc75cf919a59f202de6970c2f1ae523887f516fd1ec23e0d3f8` | [`ed00ceb409`](https://github.com/NikNakk/dxmt/commit/ed00ceb4095e5b00d270f244e7e436af3a4b274a) (macos-xr-foveation) |

## KosmicKrisp implicit foveation (Zink path): `patches/kosmickrisp/` → [NikNakk/mesa](https://github.com/NikNakk/mesa)

`experiments/wine-implicit-foveation` (private repository) is exactly `build-in-process/kk-fov-src`: base `claude/zink-kosmickrisp` (`7dd363c`, the Zink KosmicKrisp in `workspace/kk-zink`), one commit per patch.

| Patch | SHA-256 | Commit |
| --- | --- | --- |
| `0001-implicit-foveation-prototype.patch` | `ea8418acae0243a41ce5d25345204afe15af7c2224d663160aff8f88aef6a8a1` | [`3e2a046d74`](https://github.com/NikNakk/mesa/commit/3e2a046d74cf86bbe6970f2e0cfb7b335ca3b793) (experiments/wine-implicit-foveation) |
| `0002-occlusion-query-overflow.patch` | `2646138e016ff07c1ecbd1015c1d1d6f923d85122351460d5eea2fd5d9239b0b` | [`1cf5eb7a8a`](https://github.com/NikNakk/mesa/commit/1cf5eb7a8a62e11c3e944449488bb3088a0cd414) (experiments/wine-implicit-foveation) |
| `0003-fake-wide-lines-and-cache-uuid.patch` | `87fa35e3fc6146b7864def506a5dec6911903053f4f37eec6bf20dd70470682e` | [`747af57b8f`](https://github.com/NikNakk/mesa/commit/747af57b8f10fe785c1537e76d91ff7a07188acc) (experiments/wine-implicit-foveation) |
| `0004-pass-timing.patch` | `9715758fe54d0a658244d0dc2b4977e40b26fd40348d8062969f2e201ba1885c` | [`1ada034cd6`](https://github.com/NikNakk/mesa/commit/1ada034cd63128ff501d104ef8504e3b137bc4fe) (experiments/wine-implicit-foveation) |
| `0005-compact-compute-dispatch.patch` | `57d60460d179f47f1111e4dfb1d2c330ee4feebd4f78e85664e607803594282e` | [`fa820eb447`](https://github.com/NikNakk/mesa/commit/fa820eb4474a973b0d0f62ef9ef50ad2e999cfce) (experiments/wine-implicit-foveation) |
| `0006-light-barrier-fixes-and-runtime-switches.patch` | `07301f4dc919bedda2c63f8051746f14ebb3be9635b1ed1b85a1ff4feeb40088` | [`bfac6b4f04`](https://github.com/NikNakk/mesa/commit/bfac6b4f0444ef1a37f189d50f9d540950250299) (experiments/wine-implicit-foveation) |
| `0007-fake-vrs-pass-logging.patch` | `55ce310d34eeff3087e2e8716a98489ffe24e4783ca2cf62f02168a885303e7e` | [`70f2b6b150`](https://github.com/NikNakk/mesa/commit/70f2b6b1502247237938c4c287a50e8b48e50fe1) (experiments/wine-implicit-foveation) |
| `0008-relaxed-count-and-size-skip.patch` | `a8d96fb9e4ae6714c53abff5a772f53d55cb4e1596334d4a133a8310d658c382` | [`dcb219aa57`](https://github.com/NikNakk/mesa/commit/dcb219aa579f7f841837a2843bdb3ee331125f81) (experiments/wine-implicit-foveation) |
| `0009-frame-boundary-latch.patch` | `a1d66ba60062d406040914097596508074a9b9618431bb9617f5b07fbc255e02` | [`75b176b12c`](https://github.com/NikNakk/mesa/commit/75b176b12c14f7be28d72f6df26c52a1b78e4214) (experiments/wine-implicit-foveation) |
| `0010-per-image-maps-and-gather.patch` | `893a00d0341a05ffdd7b2e333795c806360f37ff6e9786a71ac4b153515b64a8` | [`3997f10092`](https://github.com/NikNakk/mesa/commit/3997f10092cc1a4c7dbff24b2e66785515aaa3b5) (experiments/wine-implicit-foveation) |
| `0011-gpu-ordered-params-copy-maps-both-eyes.patch` | `eb42df40e3249d544e61c86e41c6c6bf17650a3c3c268cbdf71d9c24cd06e7d1` | [`4a95ba377d`](https://github.com/NikNakk/mesa/commit/4a95ba377d41044ec15bd9d0ea80bf43d40cf2bb) (experiments/wine-implicit-foveation) |

## CrossOver Wine changes: `patches/crossover/` → [NikNakk/wine-crossover](https://github.com/NikNakk/wine-crossover)

`macos-wine-xr` on top of `crossover-26.3.0` (the unmodified CrossOver 26.3.0 Wine source) is exactly `~/Code/wine-crossover/sources/wine`, which the runtimes are built from (now a checkout of that branch). It also holds the Sony Bluetooth XInput fix that was never a patch, and the Wine patch below.

| Patch | SHA-256 | Commit |
| --- | --- | --- |
| `0001-server-keyed-mutex-waits-on-server-syncs.patch` | `7eddc92175d56ab2529cead9ba13bc1937842bf4c39b6ee86a705a9872ef82d3` | [`ff4dd6211c`](https://github.com/NikNakk/wine-crossover/commit/ff4dd6211cb264fcf234d8c661b3a98970003e78) (macos-wine-xr) |
| `0002-msync-remove-abandoned-wait-registrations.patch` | `7d007d9bbddd8b8095d4919244fcded1a085cec11ee0a5a4ed2e2e7e614b01cf` | [`2e1590b129`](https://github.com/NikNakk/wine-crossover/commit/2e1590b129e75e4b49ae9c562eeb4291d0015b7b) (macos-wine-xr) |
| `0003-wtsapi32-session-info-ex-and-remote-session.patch` | `a66fd0415628dfcedb13f19674e81ebf8e858c039950dacdbf36717683aa9578` | [`56d05fcd1d`](https://github.com/NikNakk/wine-crossover/commit/56d05fcd1db3eda7f59bad3b58b7f16b8b34f588) (macos-wine-xr) |
| `0004-winemac-accept-wgl-context-opengl-no-error.patch` | `fe7f1b1683c654a24cc8485319cf0ef1e782996c99a189381776b29118fcc3fe` | [`3dd1985099`](https://github.com/NikNakk/wine-crossover/commit/3dd198509988e46def23d103353ef4eb1d5183ed) (macos-wine-xr) |
| `0005-ntdll-app-names.patch` | `008627a0a779ba204473aa896beffe88c0ac85a3915759b755e1c4486a4b6027` | [`10fb88d05a`](https://github.com/NikNakk/wine-crossover/commit/10fb88d05a0da0d41eadf7fb86b48a37c5db293f) (macos-wine-xr) |
| `0006-server-keyed-mutex-ownership-history.patch` | `92d32a8a1115c79a69b94e7335218c05b157c802bd2a9ded045667bfbe7f9601` | [`4cb9da2fae`](https://github.com/NikNakk/wine-crossover/commit/4cb9da2fae9762a52dade312e2df6f68d33a897b) (macos-wine-xr) |
| `0007-ntdll-lsteamclient-redirect.patch` | `9c554e78571f599bf4de9e7a0c12c750ee141650c4f2164259e9e03716711cef` | [`98ad852b4b`](https://github.com/NikNakk/wine-crossover/commit/98ad852b4b5f3d99e869194012c34133a12a9dcb) (macos-wine-xr) |
| `0008-ntdll-lsteamclient-wow64.patch` | `b27ff222851e8c63623c9d6ae304fd0fa3a9003c2ad9831fc8517198252366b5` | [`4297995530`](https://github.com/NikNakk/wine-crossover/commit/429799553064fa9668f87278913f23b235a473a7) (macos-wine-xr) |
| `0009-d3dx11-load-textures-from-files.patch` | `57cb149078993f6b66ae5491f010ed91ab3c304bcd0875302f9b218ac59cfaea` | [`100782a48c`](https://github.com/NikNakk/wine-crossover/commit/100782a48c16f96b9a0cdac1c80b54d0261edfc9) (macos-wine-xr) |
| `0010-xinput-sony-gamepads-over-iohid.patch` | `3e2382f038b0cb718cd77d868732b9ea1a1a85c500e325971682f564bc763931` | [`dd7fff0496`](https://github.com/NikNakk/wine-crossover/commit/dd7fff04961cc4a08d58802485d5b786d9e03cdb) (macos-wine-xr) |
| `0011-wininet-lock-child-handle-lists.patch` | `37cccae0056fc4d6431470a87abb860797d28289fa31cbeed4a539d289dc1632` | [`682225fd52`](https://github.com/NikNakk/wine-crossover/commit/682225fd52b7cdff6903fd504ef7c1a4bec74a64) (macos-wine-xr) |
| `0012-winevulkan-expose-metal-objects.patch` | `f019681b90afaf30e2bec2044a19e4c063984b2fa28d74198e594e054ce487cf` | [`57c8c3edbb`](https://github.com/NikNakk/wine-crossover/commit/57c8c3edbb5566d47139fa6dfd0b1be447257cfc) (macos-wine-xr) |
| `0013-winemac-hide-vr-headset-displays.patch` | `6d661c82f1799c5ab8cf9ffeb21aad633d21150b37c6cf7d406418c4a3fe5688` | [`4f069519a5`](https://github.com/NikNakk/wine-crossover/commit/4f069519a5343ec8007b913b6f5c6fc9079a48e9) (macos-wine-xr) |

## Wine kernelbase fix: `patches/wine/` → [NikNakk/wine-crossover](https://github.com/NikNakk/wine-crossover)

On the same `NikNakk/wine-crossover` `macos-wine-xr` branch as the CrossOver changes.

| Patch | SHA-256 | Commit |
| --- | --- | --- |
| `0001-kernelbase-fiber-gs-slot-macos.patch` | `715797a22528ca23d33d1f73aa8c74fd9ebf3ebe67d592618b3cdab21b0f3420` | [`9fd4067ba4`](https://github.com/NikNakk/wine-crossover/commit/9fd4067ba4e9cef6727b7a7304d7a6a9b100760c) (macos-wine-xr) |

## Monado test-only simulator display size: `patches/monado/` → [NikNakk/monado](https://github.com/NikNakk/monado)

`test/simulator-display-size`, on play's Monado (`a185d42d5`).

| Patch | SHA-256 | Commit |
| --- | --- | --- |
| `0001-simulator-display-size-test-only.patch` | `3d5fc302353ae7884f2315421b7c976dcb6fdc11433cf98054c8c0b4a3f019e9` | [`b74d5eae2e`](https://github.com/NikNakk/monado/commit/b74d5eae2e04cd8d0d5453cda1d237a3710e48f0) (test/simulator-display-size) |

## xrizer Windows queue-family recovery: `patches/xrizer/` → [NikNakk/xrizer](https://github.com/NikNakk/xrizer)

Committed to `windows-openxr-d3d11` (`c30cd98`, the branch the Windows xrizer is built from) and `windows-openxr-d3d11-next`.

| Patch | SHA-256 | Commit |
| --- | --- | --- |
| `0001-recover-unset-vulkan-queue-family.patch` | `778840eb78054f5433514193aac1522be56a5363d370d7873c3a25a9d33b7360` | [`b065839e11`](https://github.com/NikNakk/xrizer/commit/b065839e11a9f816d1c0e81e7a85c0daec9508af) (windows-openxr-d3d11-next) |

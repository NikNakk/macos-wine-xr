# OpenGL through Zink on KosmicKrisp

Date: 2026-11-08. **Google Earth VR (348250) runs through Mesa's Zink on
KosmicKrisp, and its frames reach Monado with one Metal blit per eye** (no CPU
copy). Checked with frame captures and timings; not yet looked at in the headset.

Windows OpenGL applications normally go through Wine's `winemac.drv`, which uses
Apple's CGL: OpenGL 2.1, or 3.2–4.1 forward-compatible core profiles only. Here
the application instead loads Mesa's Windows `opengl32.dll` (WGL frontend +
Zink). Zink renders with Vulkan through Wine's winevulkan, which goes to an
x86_64 KosmicKrisp build (Wine runs under Rosetta).

```
Earth.exe → opengl32.dll + libgallium_wgl.dll (Mesa, PE) → vulkan-1.dll → winevulkan
          → libvulkan.1.dylib (Khronos loader, x86_64) → libvulkan_kosmickrisp.dylib (x86_64) → Metal
```

## Why Earth VR needs it

Earth VR asks `wglCreateContextAttribsARB` for **OpenGL 3.2, compatibility
profile** (`0x9126 = 2`). `winemac.drv` refuses (`OS X only supports
forward-compatible 3.2+ contexts`), and the game exits with 255
(`~/Library/Logs/macos-wine-xr/348250-20261102-201535`). Its shaders are GLSL
`#version 100`, `110` and `300 es` (Google's ION engine), and it treats transform
feedback as optional.

## Running

```sh
MWXR_OPENGL=zink ~/Windows/run-windows-game.zsh APPID
```

From Steam: Launch Options `MWXR_OPENGL=zink %command%`.
`scripts/run-in-process-openxr.zsh` then:

- puts `$MWXR_ZINK_ROOT/lib` (default `~/Code/alyx-native/build/zink-wine/lib`, the
  x86_64 Vulkan loader) first in `DYLD_FALLBACK_LIBRARY_PATH`, and sets
  `VK_DRIVER_FILES` to `icd/kosmickrisp_x86_64.json` and `GALLIUM_DRIVER=zink`;
- sets `MESA_GL_VERSION_OVERRIDE=3.2COMPAT` and `MESA_KK_EXPERIMENTAL=custom_border`
  unless already set (an empty value turns either off);
- copies `opengl32.dll`, `libgallium_wgl.dll` and `libwinpthread-1.dll` into the
  prefix's `system32` and adds `opengl32=n,b` to the application's overrides. Wine
  finds `system32`'s builtin stub before `PATH`, so `WINEPATH` does not work. Other
  programs in the prefix still get Wine's builtin `opengl32`, since only `n,b` loads
  the copy.

`~/Code/alyx-native/build/zink-wine/wine-zink.zsh program.exe` runs anything in
that folder's test prefix the same way (`MWXR_ZINK=0` for Wine's own OpenGL).

## OpenGL version

KosmicKrisp has tessellation, but neither `geometryShader` nor
`VK_EXT_transform_feedback`. Mesa needs transform feedback for OpenGL 3.0 and
geometry shaders (GLSL 1.50) for 3.2, so Zink offers **2.1** without the
override. With `MESA_GL_VERSION_OVERRIDE=3.2COMPAT`, Zink reports 3.2 compatibility
and GLSL 1.50 but still cannot run geometry shaders or transform feedback. The
proper fix is to add both to KosmicKrisp with Mesa's `src/poly` emulation (already
used for its tessellation, and by Honeykrisp for both).

`tools/gl_probe/wgl_probe.c` asks for Earth VR's context and draws frames:

| OpenGL | Request | Result |
| --- | --- | --- |
| winemac (CGL) | 3.2 compatibility | fails (error 8341); legacy context 2.1 Metal |
| Zink, no override | 3.2 compatibility | fails (error 203); legacy context 2.1 |
| Zink, `3.2COMPAT` | 3.2 compatibility | `3.2 (Compatibility Profile) Mesa 27.0.0-devel`, 60 frames, no GL errors |
| Zink, `4.1COMPAT` | 4.1 compatibility | granted (reported only; same features) |
| Zink, no override | 3.3 core | fails |

Renderer: `zink Vulkan 1.4(Apple M5 (MESA_KOSMICKRISP))`. Without
`MESA_KK_EXPERIMENTAL=custom_border`, Zink warns that `EXT_custom_border_color` is
missing.

## Earth VR results

With `MWXR_OPENGL=zink` (no headset connected; Monado used its service compositor):

- Zink loads, OpenXR connects through wineopenxr, and Earth VR downloads map data.
- It then crashed after 30–80 s in Wine's `wininet` (`invalidate_handle`,
  `internet.c:231`, a null read), three times out of three. Earth VR connects
  from many threads on one `InternetOpen` handle, and Wine adds sessions and
  requests to their parent's child list, and removes them, without a lock;
  closing a session then walked a freed child.
  `patches/crossover/0011-wininet-lock-child-handle-lists.patch` guards those
  lists (and the `valid_handle` check-and-set) with one critical section. It is
  installed in `runtime-lsteamclient` (original
  `lib/wine/x86_64-windows/wininet.dll.pre-children-lock`). The CrossOver build
  tree has tests disabled, so Wine's `wininet` tests were not run.
- With the patch it ran for 3 minutes without crashing, until stopped.
- A 60-second run traced through winevulkan: 3 swapchains created, **3,004
  `vkQueuePresentKHR`** (about 50 fps), 12,022 queue submits, 42 graphics
  pipelines. Whether the image is correct has not been checked by eye.

## Headset: OpenGL submissions in xrizer

Earth VR submits OpenGL textures with `IVRCompositor_021::Submit`; xrizer's Windows
build used to reject them (`gl_stub.rs`). xrizer's `windows-openxr-d3d11` branch now
handles them in `src/graphics_backends/d3d11_gl.rs`: an OpenGL texture creates the
D3D11 backend in a GL-source mode (`D3D11Data::new_for_gl`), so the OpenXR session
stays D3D11 → wineopenxr → Metal → Monado. GL functions are looked up at runtime, so
D3D games never load `opengl32`.

### One Metal blit per eye

```
Earth VR's GL texture ─ Zink VkImage ─ KosmicKrisp MTLTexture (2D view, same storage)
   └─ wglExportMetalTextureMWXR ─ IDXMTNativeDevice2::ImportMetalTexture (no copy)
        └─ CopySubresourceRegion → DXMT Metal blit → Monado's swapchain image
```

- **Mesa** (`kk-zink`): `wglExportMetalTextureMWXR(texture, &info)` returns the
  `MTLTexture`, size, levels, samples and DXGI format of a 2D texture of the current
  context (`st_interop_export_metal_texture`). Zink enables `VK_EXT_metal_objects`,
  answers a new `WINSYS_HANDLE_TYPE_METAL_TEXTURE` with `vkExportMetalObjectsEXT`,
  and installs `resource_get_handle` when that is its only export. It exports a 2D
  view of level 0, layer 0 (kept on the resource object), because KosmicKrisp
  stores images with input-attachment usage as 2D *array* textures, which DXMT
  rejects for `ArraySize = 1`.
- **Wine** (`patches/crossover/0012-winevulkan-expose-metal-objects.patch`):
  winevulkan exposes `VK_EXT_metal_objects`, and the generator now copies the
  exported objects back to the caller (its win64 conversion of the chained structs
  was empty). Regenerated from the pinned Vulkan 1.4.335 registry: the unmodified
  generator reproduces CrossOver's files byte for byte. `struct vulkan_device` grew,
  so `win32u.so`, `winemac.so`, `opengl32.so`, `winevulkan.so` and `winevulkan.dll`
  were rebuilt together and installed in `runtime-lsteamclient` (originals
  `*.pre-metal-objects`). The build tree's `include/config.h` needs the
  `SONAME_LIBVULKAN` define from [graphics-interop.md](graphics-interop.md).
- **xrizer**: imports the texture once (re-imported if the GL texture's storage
  changes), calls `glFinish` so the blit on DXMT's queue sees finished rendering,
  and copies each eye's region. The image keeps GL's bottom-to-top rows; the
  projection layer presents it flipped by swapping the field of view's top and
  bottom (`GraphicsBackend::submits_bottom_up`, as xrizer already did for inverted
  bounds; Monado does not validate the angles). Overlays still use the CPU path,
  which flips their rows.
- **Fallback**: CPU copies (`glReadPixels` → staging texture) when Mesa or DXMT
  lacks the interface or the texture does not fit (format, mip levels, samples), or
  with `XRIZER_GL_CPU=1`. The reason is logged once.

`tools/gl_probe/dxmt_import_probe.c` exports a Zink texture and tries DXMT imports
with several descriptions; it found the array-texture mismatch.

### Results

Earth VR submits one side-by-side texture. Without the PS VR2's display, Monado's
default display gives 896×1007 per eye at 60 Hz. Same session setup, 450-copy
averages, from xrizer's log:

| Path | xrizer per eye | of which waiting for Earth VR's GL work | Frame rate |
| --- | --- | --- | --- |
| Metal blit | 4.4–4.7 ms | 4.4–4.7 ms (`glFinish`) | 56–59 fps |
| CPU copy (`XRIZER_GL_CPU=1`) | 5.1–5.4 ms | inside the readback | 53–55 fps |

With the blit, xrizer's own cost is about 0.01 ms per eye: what remains is Earth
VR's rendering, which the CPU path waits for too. At the headset's 2800×2856 per
eye the CPU copy took **25.3 ms per eye** (about 15 fps), so the blit's gain there
should be far larger. Not yet measured: the PS VR2 display was absent.

`XRIZER_GL_CAPTURE=<Windows folder>` saves eye images as presented (flipped as the
layer flips them) every 600 copies of an eye, up to 5 each, encoded off the
submit thread; on the blit path through a one-off GPU readback. Monado has no file
capture of its own (its mirror goes to the debug GUI). Captures in
`~/Library/Logs/macos-wine-xr/348250-zink-blit-20261108c/capture` show Earth VR's
loading scene upright.

xrizer's log works under Wine now: Wine does not pass `XDG_*` variables to Windows
programs, so `run-windows-game.zsh` also sets `XRIZER_STATE_HOME`, which xrizer
reads first (`<logs>/xrizer/xrizer.txt`). Installed in
`~/Windows/openvr-runtimes/xrizer/bin/vrclient_x64.dll` (original
`vrclient_x64.dll.pre-gl`).

A Monado service crash seen during this work is unrelated: when macOS listed the
PS VR2 display as 1×1, the service created its headset window off the main thread
and AppKit aborted (`~/Library/Logs/Monado/monado-service.err.log`).

## Next steps

1. **Measure at headset resolution**, with the PS VR2 display active.
2. **GPU-side synchronisation.** `glFinish` blocks Earth VR's thread until its
   frame finishes on the GPU. Waiting on a Metal shared event on DXMT's queue
   (KosmicKrisp exports semaphores' events; DXMT imports them as fences) would let
   it continue.
3. **Real OpenGL 3.2+.** Geometry shaders and transform feedback in KosmicKrisp
   with `src/poly`, removing the version override.

## Building

Mesa: `~/Code/alyx-native/workspace/kk-zink`, branch `claude/zink-kosmickrisp`
(the `kk-integration` commit `b386420`, unchanged). Cross files are in
`~/Code/alyx-native/build/cross`. Use the Mesa Python venv
(`workspace/alyx-mesa-py314/bin`) and Homebrew bison first in `PATH`.

x86_64 KosmicKrisp (`build/kk-zink-x86_64`), reusing the arm64 build's host tools
(`mesa_clc`, `vtn_bindgen2`, `kk_clc` from `build/kk-integration` in `PATH`):

```sh
meson setup build/kk-zink-x86_64 workspace/kk-zink --cross-file build/cross/darwin-x86_64.ini \
  -Dbuildtype=release -Dwrap_mode=nodownload -Dplatforms=macos -Dvulkan-drivers=kosmickrisp \
  -Dgallium-drivers= -Dopengl=false -Dzstd=disabled -Dllvm=disabled -Dmesa-clc=system \
  -Dprecomp-compiler=system -Dspirv-tools=disabled -Dexpat=disabled -Dxmlconfig=disabled \
  -Dlibunwind=disabled -Dvalgrind=disabled
```

Zink for Windows (`build/zink-wgl-x86_64`, MinGW GCC 16):

```sh
meson setup build/zink-wgl-x86_64 workspace/kk-zink --cross-file build/cross/mingw-x86_64.ini \
  -Dbuildtype=release -Dwrap_mode=nodownload -Dplatforms=windows -Dgallium-drivers=zink \
  -Dvulkan-drivers= -Dopengl=true -Degl=disabled -Dgles1=disabled -Dgles2=disabled \
  -Dllvm=disabled -Dzstd=disabled -Dzlib=disabled -Dshader-cache=disabled -Dexpat=disabled \
  -Dxmlconfig=disabled -Dspirv-tools=disabled -Dmicrosoft-clc=disabled -Dvalgrind=disabled \
  -Dlibunwind=disabled -Dbuild-tests=false
```

`build/zink-wine` holds the result: `bin/` (the two Mesa DLLs, MinGW's
`libwinpthread-1.dll`, `wgl_probe.exe`), `lib/libvulkan.1.dylib` (the x86_64 slice
of Meta XR Simulator's Khronos loader 1.4; Wine's runtime links MoltenVK directly
as `libvulkan.1.dylib`) and `icd/kosmickrisp_x86_64.json`.

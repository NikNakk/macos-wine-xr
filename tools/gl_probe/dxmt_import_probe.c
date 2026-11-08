/*
 * Copyright 2026, Nick Kennedy
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Exports a Zink GL texture's Metal texture (wglExportMetalTextureMWXR) and tries to
 * import it into DXMT (IDXMTNativeDevice2::ImportMetalTexture) with several D3D11
 * descriptions, to show which ones match.
 *
 * Build: x86_64-w64-mingw32-gcc -O2 -o dxmt_import_probe.exe dxmt_import_probe.c \
 *          -lopengl32 -lgdi32 -ld3d11 -ldxguid
 * Run in a prefix with DXMT, with Zink's opengl32 (MWXR_OPENGL=zink's setup).
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <GL/gl.h>
#include <stdio.h>

struct metal_texture {
   unsigned long long mtl_texture;
   unsigned dxgi_format, width, height, levels, samples;
};

static const GUID IID_native2 = {0x5a6d2e1b, 0xb10a, 0x4ab7,
                                 {0x8a, 0xde, 0xd9, 0xa6, 0x4c, 0x41, 0x72, 0x83}};

typedef struct native2_vtbl {
   void *qi, *addref;
   ULONG (WINAPI *release)(void *);
   void *get_metal_device;
   HRESULT (WINAPI *import_metal_texture)(void *, UINT64, const D3D11_TEXTURE2D_DESC *,
                                          ID3D11Texture2D **);
} native2_vtbl;

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
   return DefWindowProcA(w, m, wp, lp);
}

int main(void)
{
   WNDCLASSA wc = {CS_OWNDC, wndproc, 0, 0, GetModuleHandleA(NULL), 0, 0, 0, 0, "probe"};
   PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1,
      PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
   HWND wnd;
   HDC dc;
   ID3D11Device *device = NULL;
   void **native = NULL;
   struct metal_texture metal = {0};
   int (*export_metal)(unsigned, struct metal_texture *);
   GLuint tex;

   setvbuf(stdout, NULL, _IONBF, 0);
   RegisterClassA(&wc);
   wnd = CreateWindowA("probe", "probe", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, 0, 0, wc.hInstance, 0);
   dc = GetDC(wnd);
   SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
   wglMakeCurrent(dc, wglCreateContext(dc));
   printf("GL: %s\n", glGetString(GL_RENDERER));

   export_metal = (void *)wglGetProcAddress("wglExportMetalTextureMWXR");
   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1792, 1007, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   if (!export_metal || !export_metal(tex, &metal)) {
      printf("FAIL export\n");
      return 1;
   }
   printf("exported %#llx DXGI %u %ux%u levels %u samples %u\n", metal.mtl_texture,
          metal.dxgi_format, metal.width, metal.height, metal.levels, metal.samples);

   if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                D3D11_SDK_VERSION, &device, NULL, NULL))) {
      printf("FAIL D3D11CreateDevice\n");
      return 1;
   }
   if (FAILED(ID3D11Device_QueryInterface(device, &IID_native2, (void **)&native))) {
      printf("FAIL no IDXMTNativeDevice2\n");
      return 1;
   }

   {
      const UINT formats[] = {28, 29, 27, 87, 91};
      const UINT binds[] = {D3D11_BIND_SHADER_RESOURCE, D3D11_BIND_RENDER_TARGET,
                            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET};
      native2_vtbl *vtbl = *(native2_vtbl **)native;
      for (unsigned f = 0; f < ARRAYSIZE(formats); f++) {
         for (unsigned b = 0; b < ARRAYSIZE(binds); b++) {
            D3D11_TEXTURE2D_DESC desc = {metal.width, metal.height, 1, 1, formats[f], {1, 0},
                                         D3D11_USAGE_DEFAULT, binds[b], 0, 0};
            ID3D11Texture2D *out = NULL;
            HRESULT hr = vtbl->import_metal_texture(native, metal.mtl_texture, &desc, &out);
            printf("format %u bind %#x: %#lx%s\n", formats[f], binds[b], (unsigned long)hr,
                   out ? " imported" : "");
            if (out)
               ID3D11Texture2D_Release(out);
         }
      }
   }
   return 0;
}

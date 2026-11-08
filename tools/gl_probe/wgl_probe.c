/*
 * Copyright 2026, Nick Kennedy
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Asks for the OpenGL context Google Earth VR asks for (WGL 3.2, compatibility
 * profile), prints what it got, and draws a few frames to the window.
 *
 * Usage: [WGL_PROBE_FRAMES=n] wgl_probe.exe [major minor [core]]   (default 3 2, 120 frames)
 * Build: x86_64-w64-mingw32-gcc -O2 -o wgl_probe.exe wgl_probe.c -lopengl32 -lgdi32
 */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_FLAGS_ARB 0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x1
#define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x2
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_NUM_EXTENSIONS 0x821D
#define GL_CONTEXT_PROFILE_MASK 0x9126

typedef HGLRC(WINAPI *create_attribs_fn)(HDC, HGLRC, const int *);
typedef const GLubyte *(WINAPI *get_stringi_fn)(GLenum, GLuint);

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
   return DefWindowProcA(w, m, wp, lp);
}

static int has_ext(const char *name)
{
   get_stringi_fn get_stringi = (get_stringi_fn)wglGetProcAddress("glGetStringi");
   GLint n = 0;
   const char *all;
   if (get_stringi) {
      glGetIntegerv(GL_NUM_EXTENSIONS, &n);
      for (GLint i = 0; i < n; i++)
         if (!strcmp((const char *)get_stringi(GL_EXTENSIONS, i), name))
            return 1;
      if (n)
         return 0;
   }
   all = (const char *)glGetString(GL_EXTENSIONS);
   return all && strstr(all, name) != NULL;
}

int main(int argc, char **argv)
{
   int major = argc > 2 ? atoi(argv[1]) : 3, minor = argc > 2 ? atoi(argv[2]) : 2;
   int core = argc > 3 && !strcmp(argv[3], "core");
   int frames = getenv("WGL_PROBE_FRAMES") ? atoi(getenv("WGL_PROBE_FRAMES")) : 0;
   WNDCLASSA wc = {CS_OWNDC, wndproc, 0, 0, GetModuleHandleA(NULL), 0, 0, 0, 0, "wgl_probe"};
   PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1,
      PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
   HWND wnd;
   HDC dc;
   HGLRC legacy, ctx;
   create_attribs_fn create_attribs;
   int fmt, attribs[] = {
      WGL_CONTEXT_MAJOR_VERSION_ARB, major, WGL_CONTEXT_MINOR_VERSION_ARB, minor,
      WGL_CONTEXT_FLAGS_ARB, 0,
      WGL_CONTEXT_PROFILE_MASK_ARB,
      core ? WGL_CONTEXT_CORE_PROFILE_BIT_ARB : WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB, 0};
   GLint profile = 0;

   if (frames <= 0)
      frames = 120;
   pfd.cDepthBits = 24;
   pfd.cStencilBits = 8;
   setvbuf(stdout, NULL, _IONBF, 0);

   RegisterClassA(&wc);
   wnd = CreateWindowA("wgl_probe", "wgl_probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                       100, 100, 640, 480, 0, 0, wc.hInstance, 0);
   dc = GetDC(wnd);
   fmt = ChoosePixelFormat(dc, &pfd);
   if (!fmt || !SetPixelFormat(dc, fmt, &pfd)) {
      printf("FAIL pixel format %d (error %lu)\n", fmt, GetLastError());
      return 1;
   }
   legacy = wglCreateContext(dc);
   if (!legacy || !wglMakeCurrent(dc, legacy)) {
      printf("FAIL wglCreateContext (error %lu)\n", GetLastError());
      return 1;
   }
   printf("legacy context: %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
   if (getenv("WGL_PROBE_EXTENSIONS"))
      printf("GLSL %s\nextensions: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION),
             glGetString(GL_EXTENSIONS));

   create_attribs = (create_attribs_fn)wglGetProcAddress("wglCreateContextAttribsARB");
   if (!create_attribs) {
      printf("FAIL no wglCreateContextAttribsARB\n");
      return 1;
   }
   ctx = create_attribs(dc, NULL, attribs);
   wglMakeCurrent(NULL, NULL);
   wglDeleteContext(legacy);
   if (!ctx || !wglMakeCurrent(dc, ctx)) {
      printf("FAIL %d.%d %s context (error %lu)\n", major, minor,
             core ? "core" : "compatibility", GetLastError());
      return 2;
   }
   if (major * 10 + minor >= 32)
      glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
   printf("requested %d.%d %s: OK\n", major, minor, core ? "core" : "compatibility");
   printf("GL_VENDOR   %s\n", glGetString(GL_VENDOR));
   printf("GL_RENDERER %s\n", glGetString(GL_RENDERER));
   printf("GL_VERSION  %s\n", glGetString(GL_VERSION));
   printf("GLSL        %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
   printf("profile mask 0x%x\n", profile);
   printf("transform feedback %d, geometry shader4 %d, NV_DX_interop %d\n",
          has_ext("GL_EXT_transform_feedback"), has_ext("GL_ARB_geometry_shader4"),
          has_ext("WGL_NV_DX_interop"));

   {
      /* Mesa's in-process Metal export (Zink over KosmicKrisp), used by xrizer. */
      struct {
         unsigned long long mtl_texture;
         unsigned dxgi_format, width, height, levels, samples;
      } metal = {0};
      int (*export_metal)(unsigned, void *) =
         (int (*)(unsigned, void *))wglGetProcAddress("wglExportMetalTextureMWXR");
      GLuint tex;
      glGenTextures(1, &tex);
      glBindTexture(GL_TEXTURE_2D, tex);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 128, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      if (!export_metal)
         printf("wglExportMetalTextureMWXR: not available\n");
      else if (!export_metal(tex, &metal))
         printf("wglExportMetalTextureMWXR: failed\n");
      else
         printf("wglExportMetalTextureMWXR: MTLTexture %#llx, DXGI format %u, %ux%u, %u levels, %u samples\n",
                metal.mtl_texture, metal.dxgi_format, metal.width, metal.height, metal.levels,
                metal.samples);
      glDeleteTextures(1, &tex);
   }

   if (getenv("WGL_PROBE_GEOMETRY")) {
      /* A geometry shader: Zink on KosmicKrisp has none, so this should fail. */
      typedef GLuint(WINAPI *create_shader_fn)(GLenum);
      create_shader_fn create_shader = (create_shader_fn)wglGetProcAddress("glCreateShader");
      GLuint shader = create_shader ? create_shader(0x8DD9 /* GL_GEOMETRY_SHADER */) : 0;
      printf("glCreateShader(GL_GEOMETRY_SHADER) = %u, glGetError 0x%x\n", shader, glGetError());
   }

   for (int i = 0; i < frames; i++) {
      MSG msg;
      while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE))
         DispatchMessageA(&msg);
      glClearColor((i % 60) / 60.0f, 0.3f, 0.6f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      glBegin(GL_TRIANGLES);
      glColor3f(1, 0, 0); glVertex2f(-0.6f, -0.5f);
      glColor3f(0, 1, 0); glVertex2f(0.6f, -0.5f);
      glColor3f(0, 0, 1); glVertex2f(0.0f, 0.6f);
      glEnd();
      SwapBuffers(dc);
   }
   {
      GLenum err = glGetError();
      unsigned char px[4] = {0};
      glReadPixels(320, 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
      printf("drew %d frames, glGetError 0x%x, centre pixel %u,%u,%u\n", frames, err,
             px[0], px[1], px[2]);
   }
   wglMakeCurrent(NULL, NULL);
   wglDeleteContext(ctx);
   DestroyWindow(wnd);
   return 0;
}

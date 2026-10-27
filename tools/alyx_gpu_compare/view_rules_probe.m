// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
//
// Which texture views need MTLTextureUsagePixelFormatView on this GPU? Creates
// textures without that usage and asks for views in the formats D3D11 typeless
// families use. Run with MTL_DEBUG_LAYER=1: Metal's validation layer reports
// (and by default aborts on) views it does not allow, so each case runs in a
// child process.
//
// clang -fobjc-arc -O2 view_rules_probe.m -framework Metal -framework Foundation -o view_rules_probe
// MTL_DEBUG_LAYER=1 ./view_rules_probe
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct { const char *name; MTLPixelFormat storage, view; MTLTextureUsage usage; NSUInteger samples; } Case;

static int run_case(const Case *c) {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  MTLTextureDescriptor *d = [MTLTextureDescriptor new];
  d.textureType = c->samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
  d.pixelFormat = c->storage;
  d.width = 256; d.height = 256; d.sampleCount = c->samples;
  d.storageMode = MTLStorageModePrivate;
  d.usage = c->usage;
  id<MTLTexture> t = [device newTextureWithDescriptor:d];
  if (!t) return 2;
  id<MTLTexture> v = [t newTextureViewWithPixelFormat:c->view];
  return v ? 0 : 3;
}

int main(void) {
  @autoreleasepool {
    const MTLTextureUsage rt = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    const Case cases[] = {
      {"RGBA8Unorm -> RGBA8Unorm_sRGB", MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA8Unorm_sRGB, rt, 1},
      {"RGBA8Unorm_sRGB -> RGBA8Unorm", MTLPixelFormatRGBA8Unorm_sRGB, MTLPixelFormatRGBA8Unorm, rt, 1},
      {"RGBA8Unorm -> RGBA8Uint", MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA8Uint, rt, 1},
      {"RGBA8Unorm -> R32Uint", MTLPixelFormatRGBA8Unorm, MTLPixelFormatR32Uint, rt, 1},
      {"RGBA16Float -> RGBA16Uint", MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA16Uint, rt, 1},
      {"RGBA16Float -> RGBA16Float (same)", MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA16Float, rt, 1},
      {"RGBA16Float MSAA2 -> RGBA16Uint", MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA16Uint, rt, 2},
      {"Depth32Float_Stencil8 -> X32_Stencil8", MTLPixelFormatDepth32Float_Stencil8, MTLPixelFormatX32_Stencil8, rt, 1},
      {"Depth32Float_Stencil8 -> Depth32Float_Stencil8", MTLPixelFormatDepth32Float_Stencil8,
       MTLPixelFormatDepth32Float_Stencil8, rt, 1},
      {"Depth32Float -> R32Float", MTLPixelFormatDepth32Float, MTLPixelFormatR32Float, rt, 1},
      {"R32Float -> R32Uint", MTLPixelFormatR32Float, MTLPixelFormatR32Uint, rt, 1},
      {"BGRA8Unorm -> BGRA8Unorm_sRGB", MTLPixelFormatBGRA8Unorm, MTLPixelFormatBGRA8Unorm_sRGB, rt, 1},
    };
    printf("%-46s %s\n", "view without PixelFormatView usage", "result");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      fflush(stdout);
      pid_t pid = fork();
      if (pid == 0) {
        int fd = open("/dev/null", 1);
        dup2(fd, 2);  // the validation layer's report goes to stderr
        _exit(run_case(&cases[i]));
      }
      int status = 0;
      waitpid(pid, &status, 0);
      const char *result = WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "allowed"
                           : WIFEXITED(status) && WEXITSTATUS(status) == 3 ? "view returned nil"
                           : "rejected by validation";
      printf("%-46s %s\n", cases[i].name, result);
    }
  }
  return 0;
}

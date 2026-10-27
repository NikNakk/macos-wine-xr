// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
//
// Does a texture's Metal usage cost bandwidth on this GPU? Mimics Alyx's
// Windows MSAA path under DXMT: a 2x MSAA render target drawn and stored to
// memory, then a compute pass reading every sample and writing a resolved
// texture (Source 2's D3D11 custom resolve). The target is created with the
// usage DXMT gives it: RenderTarget|ShaderRead, plus PixelFormatView for
// typeless DXGI formats and ShaderWrite for UAV-bound textures. Metal may not
// compress such textures losslessly; if so, the store and the read cost more.
//
// clang -fobjc-arc -O2 msaa_usage_bench.m -framework Metal -framework Foundation -o msaa_usage_bench
// ./msaa_usage_bench [width height samples iterations]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

static NSString *const kSource = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct V { float4 p [[position]]; float2 uv; };\n"
"vertex V vs(uint id [[vertex_id]]) {\n"
"  float2 uv = float2((id << 1) & 2, id & 2);\n"
"  V v; v.p = float4(uv * 2 - 1, 0, 1); v.uv = uv; return v; }\n"
// A scene-like image: smooth gradients with some high-frequency detail, so
// compression helps without being trivial.
"fragment half4 fs(V v [[stage_in]], constant float &t [[buffer(0)]]) {\n"
"  float2 p = v.uv * float2(5600, 2856);\n"
"  float n = fract(sin(dot(floor(p / 4), float2(12.9898, 78.233))) * 43758.5453);\n"
"  float3 c = float3(v.uv, 0.5 + 0.5 * sin(t + v.uv.x * 6)) * (0.8 + 0.2 * n);\n"
"  return half4(half3(c), 1); }\n"
"kernel void resolve(texture2d_ms<half, access::read> src [[texture(0)]],\n"
"                    texture2d<half, access::write> dst [[texture(1)]],\n"
"                    uint2 gid [[thread_position_in_grid]]) {\n"
"  if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;\n"
"  half4 s = 0;\n"
"  for (uint i = 0; i < src.get_num_samples(); ++i) s += src.read(gid, i);\n"
"  dst.write(s / half(src.get_num_samples()), gid); }\n";

static double median(NSMutableArray<NSNumber *> *a) {
  [a sortUsingSelector:@selector(compare:)];
  return a.count ? a[a.count / 2].doubleValue : 0;
}

int main(int argc, const char **argv) {
  @autoreleasepool {
    NSUInteger w = argc > 2 ? atoi(argv[1]) : 5600, h = argc > 2 ? atoi(argv[2]) : 2856;
    NSUInteger samples = argc > 3 ? atoi(argv[3]) : 2, iterations = argc > 4 ? atoi(argv[4]) : 60;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:kSource options:nil error:&error];
    if (!library) { NSLog(@"%@", error); return 1; }
    MTLRenderPipelineDescriptor *rp = [MTLRenderPipelineDescriptor new];
    rp.vertexFunction = [library newFunctionWithName:@"vs"];
    rp.fragmentFunction = [library newFunctionWithName:@"fs"];
    rp.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    rp.rasterSampleCount = samples;
    id<MTLRenderPipelineState> draw = [device newRenderPipelineStateWithDescriptor:rp error:&error];
    id<MTLComputePipelineState> resolve =
        [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"resolve"] error:&error];
    if (!draw || !resolve) { NSLog(@"%@", error); return 1; }
    id<MTLCommandQueue> queue = [device newCommandQueue];

    MTLTextureDescriptor *out = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                    width:w height:h mipmapped:NO];
    out.storageMode = MTLStorageModePrivate;
    out.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    id<MTLTexture> resolved = [device newTextureWithDescriptor:out];

    struct { const char *name; MTLTextureUsage extra; } variants[] = {
      {"RenderTarget|ShaderRead", 0},
      {"+PixelFormatView", MTLTextureUsagePixelFormatView},
      {"+ShaderWrite", MTLTextureUsageShaderWrite},
      {"+PixelFormatView+ShaderWrite", MTLTextureUsagePixelFormatView | MTLTextureUsageShaderWrite},
    };
    printf("%s, %lux%lu, %lux MSAA RGBA16Float, %lu iterations; GPU ms, median\n",
           device.name.UTF8String, (unsigned long)w, (unsigned long)h, (unsigned long)samples,
           (unsigned long)iterations);
    printf("%-30s %10s %12s %10s\n", "MSAA target usage", "draw+store", "resolve read", "total");
    for (int round = 0; round < 2; ++round) {  // two rounds, to show the order does not matter
      for (size_t vi = 0; vi < sizeof(variants) / sizeof(variants[0]); ++vi) {
        const char *name = variants[vi].name;
        MTLTextureUsage extra = variants[vi].extra;
        MTLTextureDescriptor *d = [MTLTextureDescriptor new];
        d.textureType = samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
        d.pixelFormat = MTLPixelFormatRGBA16Float;
        d.width = w; d.height = h; d.sampleCount = samples;
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | extra;
        id<MTLTexture> target = [device newTextureWithDescriptor:d];
        NSMutableArray<NSNumber *> *drawMs = [NSMutableArray new], *readMs = [NSMutableArray new];
        for (NSUInteger i = 0; i < iterations + 5; ++i) {
          float t = i * 0.1f;
          id<MTLCommandBuffer> a = [queue commandBuffer];
          MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
          pass.colorAttachments[0].texture = target;
          pass.colorAttachments[0].loadAction = MTLLoadActionClear;
          pass.colorAttachments[0].storeAction = MTLStoreActionStore;
          id<MTLRenderCommandEncoder> re = [a renderCommandEncoderWithDescriptor:pass];
          [re setRenderPipelineState:draw];
          [re setFragmentBytes:&t length:sizeof(t) atIndex:0];
          [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
          [re endEncoding];
          [a commit];
          id<MTLCommandBuffer> b = [queue commandBuffer];
          id<MTLComputeCommandEncoder> ce = [b computeCommandEncoder];
          [ce setComputePipelineState:resolve];
          [ce setTexture:target atIndex:0];
          [ce setTexture:resolved atIndex:1];
          [ce dispatchThreadgroups:MTLSizeMake((w + 7) / 8, (h + 7) / 8, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
          [ce endEncoding];
          [b commit];
          [b waitUntilCompleted];
          if (i < 5) continue;  // warm-up
          [drawMs addObject:@((a.GPUEndTime - a.GPUStartTime) * 1e3)];
          [readMs addObject:@((b.GPUEndTime - b.GPUStartTime) * 1e3)];
        }
        double dm = median(drawMs), rm = median(readMs);
        printf("%-30s %10.3f %12.3f %10.3f\n", name, dm, rm, dm + rm);
      }
    }
  }
  return 0;
}

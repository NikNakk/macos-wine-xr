// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>
#include "foveation_probe.h"
typedef int32_t NTSTATUS;
typedef NTSTATUS (*unixlib_entry_t)(void *);

static NTSTATUS describe(void *args)
{
    struct foveation_probe_params *p = args;
    id<MTLRasterizationRateMap> map = (id<MTLRasterizationRateMap>)p->map;
    MTLSize physical = map ? [map physicalSizeForLayer:0] : MTLSizeMake(FOVEATION_PROBE_SIZE, FOVEATION_PROBE_SIZE, 1);
    p->physical_width = physical.width; p->physical_height = physical.height;
    const unsigned coords[] = {48, 160, 256, 352, 464};
    for (unsigned y = 0; y < 5; ++y) for (unsigned x = 0; x < 5; ++x) {
        MTLCoordinate2D logical = MTLCoordinate2DMake(coords[x]*FOVEATION_PROBE_SIZE/512, coords[y]*FOVEATION_PROBE_SIZE/512);
        MTLCoordinate2D pos = map ? [map mapScreenToPhysicalCoordinates:logical forLayer:0] : logical;
        struct foveation_probe_sample *sample = &p->samples[y * 5 + x];
        sample->x = floorf(pos.x); sample->y = floorf(pos.y);
        pos = MTLCoordinate2DMake(sample->x + 0.5f, sample->y + 0.5f);
        logical = map ? [map mapPhysicalToScreenCoordinates:pos forLayer:0] : pos;
        uint32_t r = lroundf(logical.x * 255.0f / FOVEATION_PROBE_SIZE);
        uint32_t g = lroundf(logical.y * 255.0f / FOVEATION_PROBE_SIZE);
        sample->expected = r | (g << 8) | 0xff400000u;
    }
    p->status = 0;
    return 0;
}

static NTSTATUS create(void *args)
{
    struct foveation_probe_params *p = args;
    p->status = 1;
    @autoreleasepool {
        id<MTLDevice> device = (id<MTLDevice>)p->device;
        if (!device || ![device supportsRasterizationRateMapWithLayerCount:1]) return 0;
        id<MTLRasterizationRateMap> map = nil;
        if (p->mode) {
            MTLRasterizationRateLayerDescriptor *layer = [[MTLRasterizationRateLayerDescriptor alloc]
                initWithSampleCount:MTLSizeMake(8, 8, 1)];
            for (unsigned i = 0; i < 8; ++i) {
                unsigned center = p->mode == 1 ? 2 : 5;
                layer.horizontal[i] = @((i == center || i == center + 1) ? 1.0f : 0.25f);
                layer.vertical[i] = @((i == 3 || i == 4) ? 1.0f : 0.25f);
            }
            MTLRasterizationRateMapDescriptor *desc = [MTLRasterizationRateMapDescriptor
                rasterizationRateMapDescriptorWithScreenSize:MTLSizeMake(FOVEATION_PROBE_SIZE, FOVEATION_PROBE_SIZE, 1)
                layer:layer];
            map = [device newRasterizationRateMapWithDescriptor:desc];
            [layer release];
            if (!map) return 0;
        }
        MTLSize physical = map ? [map physicalSizeForLayer:0] : MTLSizeMake(FOVEATION_PROBE_SIZE, FOVEATION_PROBE_SIZE, 1);
        p->physical_width = physical.width;
        p->physical_height = physical.height;
        MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:FOVEATION_PROBE_SIZE height:FOVEATION_PROBE_SIZE mipmapped:NO];
        desc.storageMode = MTLStorageModeShared;
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
        id<MTLSharedEvent> event = [device newSharedEvent];
        if (!texture || !event) { [texture release]; [event release]; [map release]; return 0; }
        p->texture = (uint64_t)texture; p->event = (uint64_t)event; p->map = (uint64_t)map;
        describe(p);
    }
    return 0;
}

static NTSTATUS verify(void *args)
{
    struct foveation_probe_params *p = args;
    p->status = 1; p->checked = 0;
    @autoreleasepool {
        id<MTLSharedEvent> event = (id<MTLSharedEvent>)p->event;
        for (unsigned i = 0; event.signaledValue < p->value && i < 5000; ++i) usleep(1000);
        if (event.signaledValue < p->value) { p->status = 2; return 0; }
        id<MTLTexture> texture = (id<MTLTexture>)p->texture;
        id<MTLCommandQueue> queue = [(id<MTLDevice>)p->device newCommandQueue];
        id<MTLBuffer> buffer = [(id<MTLDevice>)p->device newBufferWithLength:256 * FOVEATION_PROBE_SAMPLES
            options:MTLResourceStorageModeShared];
        if (!queue || !buffer) { [queue release]; [buffer release]; return 0; }
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        for (unsigned i = 0; i < FOVEATION_PROBE_SAMPLES; ++i) {
            const struct foveation_probe_sample *sample = &p->samples[i];
            [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0
                sourceOrigin:MTLOriginMake(sample->x, sample->y, 0) sourceSize:MTLSizeMake(1, 1, 1)
                toBuffer:buffer destinationOffset:256*i destinationBytesPerRow:256 destinationBytesPerImage:256];
        }
        [blit endEncoding]; [command commit]; [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) { [buffer release]; [queue release]; return 0; }
        BOOL correct = YES;
        for (unsigned i = 0; i < FOVEATION_PROBE_SAMPLES; ++i) {
            const struct foveation_probe_sample *sample = &p->samples[i];
            uint32_t pixel = *(const uint32_t *)((const char *)buffer.contents + 256*i);
            for (unsigned c = 0; c < 4; ++c) {
                int actual = (pixel >> (8*c)) & 255, expected = (sample->expected >> (8*c)) & 255;
                if (abs(actual - expected) > 3) {
                    fprintf(stderr, "foveation sample %u at %u,%u: got %08x expected %08x\n",
                        i, sample->x, sample->y, pixel, sample->expected);
                    correct = NO;
                }
            }
            ++p->checked;
        }
        [buffer release]; [queue release];
        if (correct) p->status = 0;
    }
    return 0;
}

static NTSTATUS release_map(void *args)
{
    struct foveation_probe_params *p = args;
    [(id)p->map release]; p->map = 0;
    return 0;
}
static NTSTATUS reconstruct(void *args)
{
    struct foveation_probe_params *p = args;
    p->status = 1;
    @autoreleasepool {
        id<MTLDevice> device = (id<MTLDevice>)p->device;
        id<MTLRasterizationRateMap> map = (id<MTLRasterizationRateMap>)p->map;
        if (!map) return 0;
        id<MTLSharedEvent> event = (id<MTLSharedEvent>)p->event;
        for (unsigned i=0; event.signaledValue<p->value && i<5000; ++i) usleep(1000);
        if (event.signaledValue<p->value) { p->status=2; return 0; }
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
          "kernel void unwarp(texture2d<float,access::sample> src [[texture(0)]],"
          "texture2d<float,access::write> dst [[texture(1)]],constant float *xy [[buffer(0)]],"
          "uint2 i [[thread_position_in_grid]]) { if(any(i>=uint2(dst.get_width(),dst.get_height()))) return;"
          "constexpr sampler s(coord::pixel,filter::linear,address::clamp_to_edge);"
          "dst.write(src.sample(s,float2(xy[i.x],xy[dst.get_width()+i.y])),i); }";
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        if (!library) { fprintf(stderr,"unwarp compile: %s\n",error.description.UTF8String); return 0; }
        id<MTLFunction> function = [library newFunctionWithName:@"unwarp"];
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
        [function release]; [library release];
        if (!pipeline) return 0;
        // Fixed separable map: cache its exact per-pixel axis transform once,
        // outside timing. This is a mechanism experiment, not a hot-path API.
        float xy[2*FOVEATION_PROBE_SIZE];
        for (unsigned i=0;i<FOVEATION_PROBE_SIZE;++i) {
            MTLCoordinate2D pos=[map mapScreenToPhysicalCoordinates:MTLCoordinate2DMake(i+.5f,i+.5f) forLayer:0];
            MTLSize physical=[map physicalSizeForLayer:0];
            xy[i]=fminf(fmaxf(pos.x,.5f),physical.width-.5f);
            xy[FOVEATION_PROBE_SIZE+i]=fminf(fmaxf(pos.y,.5f),physical.height-.5f);
        }
        id<MTLBuffer> lut=[device newBufferWithBytes:xy length:sizeof(xy) options:MTLResourceStorageModeShared];
        MTLTextureDescriptor *desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:FOVEATION_PROBE_SIZE height:FOVEATION_PROBE_SIZE mipmapped:NO];
        desc.storageMode=MTLStorageModePrivate; desc.usage=MTLTextureUsageShaderWrite|MTLTextureUsageShaderRead;
        id<MTLTexture> output=[device newTextureWithDescriptor:desc];
        id<MTLCommandQueue> queue=[device newCommandQueue];
        double times[21], cpu[21]; BOOL good=output && queue && lut;
        for (unsigned i=0;good && i<24;++i) {
            double start=[NSDate timeIntervalSinceReferenceDate];
            id<MTLCommandBuffer> command=[queue commandBuffer];
            id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
            [encoder setComputePipelineState:pipeline]; [encoder setBuffer:lut offset:0 atIndex:0];
            [encoder setTexture:(id<MTLTexture>)p->texture atIndex:0]; [encoder setTexture:output atIndex:1];
            [encoder dispatchThreads:MTLSizeMake(FOVEATION_PROBE_SIZE,FOVEATION_PROBE_SIZE,1) threadsPerThreadgroup:MTLSizeMake(16,16,1)];
            [encoder endEncoding]; [command commit];
            double submitted=[NSDate timeIntervalSinceReferenceDate];
            [command waitUntilCompleted];
            good=command.status==MTLCommandBufferStatusCompleted && command.GPUEndTime>command.GPUStartTime;
            if(i>=3) { times[i-3]=(command.GPUEndTime-command.GPUStartTime)*1000.0; cpu[i-3]=(submitted-start)*1000.0; }
        }
        if(good) {
            for(unsigned i=0;i<21;++i) for(unsigned j=i+1;j<21;++j)
                if(times[j]<times[i]) { double t=times[i];times[i]=times[j];times[j]=t; }
            p->reconstruction_gpu_ms=times[10];
            for(unsigned i=0;i<21;++i) for(unsigned j=i+1;j<21;++j)
                if(cpu[j]<cpu[i]) { double t=cpu[i];cpu[i]=cpu[j];cpu[j]=t; }
            p->reconstruction_cpu_ms=cpu[10];
            [(id)p->texture release]; p->texture=(uint64_t)output; output=nil;
            const unsigned coords[]={48,160,256,352,464};
            for(unsigned y=0;y<5;++y) for(unsigned x=0;x<5;++x) {
                struct foveation_probe_sample *s=&p->samples[y*5+x];s->x=coords[x]*FOVEATION_PROBE_SIZE/512;s->y=coords[y]*FOVEATION_PROBE_SIZE/512;
                s->expected=(uint32_t)lroundf((s->x+.5f)*255/FOVEATION_PROBE_SIZE)|((uint32_t)lroundf((s->y+.5f)*255/FOVEATION_PROBE_SIZE)<<8)|0xff400000u;
            }
            p->status=0;
        }
        [output release]; [queue release]; [lut release]; [pipeline release];
    }
    return 0;
}
static NTSTATUS release_objects(void *args)
{
    struct foveation_probe_params *p = args;
    release_map(args);
    [(id)p->texture release]; [(id)p->event release];
    p->texture = p->event = 0;
    return 0;
}
const unixlib_entry_t __wine_unix_call_funcs[] = {create, verify, release_map, release_objects, describe, reconstruct};

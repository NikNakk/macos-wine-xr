// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Test-only synchronous materialization; never used by the DXMT production path.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <unistd.h>
#include <math.h>
#include "depth_foveation_probe.h"
typedef int32_t NTSTATUS;
typedef NTSTATUS (*unixlib_entry_t)(void *);
static id<MTLTexture> texture(id<MTLDevice> device,unsigned size,unsigned samples,MTLPixelFormat format) {
 MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:size height:size mipmapped:NO];
 d.textureType=MTLTextureType2DMultisample;d.sampleCount=samples;d.storageMode=MTLStorageModePrivate;
 d.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
 if(format==MTLPixelFormatDepth32Float)d.usage|=MTLTextureUsagePixelFormatView;
 return [device newTextureWithDescriptor:d];
}
static NTSTATUS create(void *args) {
 struct depth_foveation_params *p=args;p->status=1;
 @autoreleasepool {
  id<MTLDevice> device=(id<MTLDevice>)p->device;
  if(!isfinite(p->peripheral_rate) || p->peripheral_rate<=0 || p->peripheral_rate>1 || !p->size || p->size>4096 || (p->samples!=2 && p->samples!=4) || ![device supportsTextureSampleCount:p->samples] ||
      ![device supportsRasterizationRateMapWithLayerCount:1])return 0;
  id<MTLRasterizationRateMap> map=nil;
  if(p->mapped) {
   MTLRasterizationRateLayerDescriptor *layer=[[MTLRasterizationRateLayerDescriptor alloc] initWithSampleCount:MTLSizeMake(8,8,1)];
   for(unsigned i=0;i<8;++i){layer.horizontal[i]=@((i==3||i==4)?1.f:p->peripheral_rate);layer.vertical[i]=@((i==3||i==4)?1.f:p->peripheral_rate);}
   map=[device newRasterizationRateMapWithDescriptor:[MTLRasterizationRateMapDescriptor
     rasterizationRateMapDescriptorWithScreenSize:MTLSizeMake(p->size,p->size,1) layer:layer]];
   [layer release];if(!map)return 0;
  }
  id<MTLTexture> color=texture(device,p->size,p->samples,MTLPixelFormatRGBA8Unorm);
  id<MTLTexture> depth=texture(device,p->size,p->samples,MTLPixelFormatDepth32Float);
  id<MTLSharedEvent> event=[device newSharedEvent];
  if(!color||!depth||!event){[color release];[depth release];[event release];[map release];return 0;}
  p->color=(uint64_t)color;p->depth=(uint64_t)depth;p->event=(uint64_t)event;p->map=(uint64_t)map;
  MTLSize physical=map?[map physicalSizeForLayer:0]:MTLSizeMake(p->size,p->size,1);
  p->physical_width=physical.width;p->physical_height=physical.height;p->status=0;
 }
 return 0;
}
// Shared per-sample coordinate layout: 2*size values per sample, then sample offsets.
static BOOL fill_sample_axes(struct depth_foveation_params *p,float *xy){
 if(!xy || !p->size || p->size>4096 || (p->samples!=2 && p->samples!=4))return NO;
 id<MTLDevice> device=(id)p->device;id<MTLRasterizationRateMap> map=(id)p->map;
 MTLSamplePosition positions[4];[device getDefaultSamplePositions:positions count:p->samples];
 for(unsigned sample=0;sample<p->samples;++sample){
  for(unsigned i=0;i<p->size;++i){MTLCoordinate2D a=MTLCoordinate2DMake(i+positions[sample].x,i+positions[sample].y);
   if(map)a=[map mapScreenToPhysicalCoordinates:a forLayer:0];
   xy[sample*2*p->size+i]=a.x;xy[sample*2*p->size+p->size+i]=a.y;
  }
  xy[2*p->size*p->samples+2*sample]=positions[sample].x;xy[2*p->size*p->samples+2*sample+1]=positions[sample].y;
 }return YES;
}
static NTSTATUS reconstruct(void *args) {
 struct depth_foveation_params *p=args;p->status=1;
 @autoreleasepool {
  id<MTLDevice> device=(id<MTLDevice>)p->device;id<MTLRasterizationRateMap> map=(id)p->map;
  if(p->filter>5)return 0;BOOL color_only=p->filter>=3;
  id<MTLSharedEvent> event=(id)p->event;
  for(unsigned i=0;event.signaledValue<p->value && i<10000;++i)usleep(1000);
  if(event.signaledValue<p->value)return 0;
  MTLSize physical=map?[map physicalSizeForLayer:0]:MTLSizeMake(p->size,p->size,1);
  size_t count=p->samples*(2*p->size+2);
  id<MTLBuffer> lut=[device newBufferWithLength:count*sizeof(float) options:MTLResourceStorageModeShared];
  if(!lut || !fill_sample_axes(p,lut.contents)){[lut release];return 0;}
  NSString *source=@"#include <metal_stdlib>\nusing namespace metal;\n"
   "struct V{float4 p [[position]];};struct O{float4 c [[color(0)]];\n#if !COLOR_ONLY\nfloat d [[depth(any)]];\n#endif\n};"
   "vertex V vs(uint i [[vertex_id]]){V v;v.p=float4(i==2?3.:-1.,i==1?3.:-1.,0,1);return v;}"
   "fragment O ps(V v [[stage_in]],uint s [[sample_id]],texture2d_ms<float> c [[texture(0)]],"
   "depth2d_ms<float> d [[texture(1)]],constant float *xy [[buffer(0)]],constant uint4 &info [[buffer(1)]]){"
   "uint w=info.x;uint2 i=uint2(v.p.xy);uint k=s*2*w;"
   "float2 pos=float2(xy[k+i.x],xy[k+w+i.y]);"
   "float2 offset=float2(xy[2*w*c.get_num_samples()+2*s],xy[2*w*c.get_num_samples()+2*s+1]);"
   "float2 q=pos-offset;int2 lo=int2(floor(q));float2 f=fract(q);int2 hi=int2(info.yz)-1;O o;"
   "if(info.w==0){uint2 a=uint2(clamp(int2(floor(q+.5)),int2(0),hi));o.c=c.read(a,s);\n#if !COLOR_ONLY\no.d=d.read(a,s);\n#endif\nreturn o;}"
   "uint2 a=uint2(clamp(lo,int2(0),hi)),b=uint2(clamp(lo+int2(1,0),int2(0),hi));"
   "uint2 e=uint2(clamp(lo+int2(0,1),int2(0),hi)),h=uint2(clamp(lo+1,int2(0),hi));"
   "o.c=mix(mix(c.read(a,s),c.read(b,s),f.x),mix(c.read(e,s),c.read(h,s),f.x),f.y);"
   "\n#if !COLOR_ONLY\nfloat da=d.read(a,s),db=d.read(b,s),de=d.read(e,s),dh=d.read(h,s);"
   "float conservative=da;if(f.x>0.000001)conservative=min(conservative,db);"
   "if(f.y>0.000001)conservative=min(conservative,de);if(all(f>0.000001))conservative=min(conservative,dh);"
   "o.d=info.w==2?conservative:mix(mix(da,db,f.x),mix(de,dh,f.x),f.y);\n#endif\nreturn o;}";
  NSError *error=nil;MTLCompileOptions *options=[[MTLCompileOptions alloc]init];
  options.preprocessorMacros=@{@"COLOR_ONLY":@(color_only)};
  id<MTLLibrary> library=[device newLibraryWithSource:source options:options error:&error];[options release];
  if(!library){fprintf(stderr,"depth shader compile: %s\n",error.description.UTF8String);[lut release];return 0;}
  MTLRenderPipelineDescriptor *pd=[[MTLRenderPipelineDescriptor alloc]init];
  pd.vertexFunction=[library newFunctionWithName:@"vs"];pd.fragmentFunction=[library newFunctionWithName:@"ps"];
  pd.rasterSampleCount=p->samples;pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;pd.depthAttachmentPixelFormat=color_only?MTLPixelFormatInvalid:MTLPixelFormatDepth32Float;
  id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];
  [pd.vertexFunction release];[pd.fragmentFunction release];[pd release];[library release];
  MTLDepthStencilDescriptor *dd=[[MTLDepthStencilDescriptor alloc]init];dd.depthCompareFunction=MTLCompareFunctionAlways;dd.depthWriteEnabled=YES;
  id<MTLDepthStencilState> ds=[device newDepthStencilStateWithDescriptor:dd];[dd release];
  id<MTLTexture> color=texture(device,p->size,p->samples,MTLPixelFormatRGBA8Unorm);
  id<MTLTexture> depth=color_only?nil:texture(device,p->size,p->samples,MTLPixelFormatDepth32Float);
  id<MTLCommandQueue> queue=[device newCommandQueue];
  BOOL good=pipeline && color && (color_only || depth) && ds && lut && queue;
  uint32_t info[4]={p->size,(uint32_t)physical.width,(uint32_t)physical.height,p->filter%3};
  double times[21]={};
  for(unsigned n=0;good && n<24;++n){
   MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
   pass.colorAttachments[0].texture=color;pass.colorAttachments[0].loadAction=MTLLoadActionDontCare;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
   if(!color_only){pass.depthAttachment.texture=depth;pass.depthAttachment.loadAction=MTLLoadActionDontCare;pass.depthAttachment.storeAction=MTLStoreActionStore;}
   id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLRenderCommandEncoder> enc=[command renderCommandEncoderWithDescriptor:pass];
   [enc setRenderPipelineState:pipeline];if(!color_only)[enc setDepthStencilState:ds];[enc setViewport:(MTLViewport){0,0,p->size,p->size,0,1}];
   [enc setFragmentTexture:(id)p->color atIndex:0];[enc setFragmentTexture:(id)p->depth atIndex:1];
   [enc setFragmentBuffer:lut offset:0 atIndex:0];[enc setFragmentBytes:info length:sizeof(info) atIndex:1];
   [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[enc endEncoding];[command commit];[command waitUntilCompleted];
   good=command.status==MTLCommandBufferStatusCompleted && command.GPUEndTime>command.GPUStartTime;
   if(n>=3)times[n-3]=(command.GPUEndTime-command.GPUStartTime)*1000;
  }
  if(good){for(unsigned i=0;i<21;++i)for(unsigned j=i+1;j<21;++j)if(times[j]<times[i]){double a=times[i];times[i]=times[j];times[j]=a;}
   [(id)p->output_color release];[(id)p->output_depth release];p->output_color=(uint64_t)color;p->output_depth=(uint64_t)depth;
   color=depth=nil;p->reconstruction_gpu_ms=times[10];p->status=0;
  }else if(error)fprintf(stderr,"depth pipeline: %s\n",error.description.UTF8String);
  [color release];[depth release];[ds release];[queue release];[pipeline release];[lut release];
 }
 return 0;
}
// Resolve at physical density, then materialize only conventional single-sample colour.
// All explicit waits and repeated timing passes belong solely to this fixture.
static NTSTATUS resolve_first(void *args) {
 struct depth_foveation_params *p=args;p->status=1;
 @autoreleasepool {
  id<MTLDevice> device=(id)p->device;id<MTLRasterizationRateMap> map=(id)p->map;id<MTLSharedEvent> event=(id)p->event;
  for(unsigned n=0;event.signaledValue<p->value && n<10000;++n)usleep(1000);
  if(event.signaledValue<p->value || !p->size)return 0;
  MTLSize physical=map?[map physicalSizeForLayer:0]:MTLSizeMake(p->size,p->size,1);
  id<MTLBuffer> lut=[device newBufferWithLength:2*p->size*sizeof(float) options:MTLResourceStorageModeShared];
  if(!lut)return 0;
  float *xy=lut.contents;
  for(unsigned i=0;i<p->size;++i){
   MTLCoordinate2D a=MTLCoordinate2DMake(i+.5f,i+.5f);if(map)a=[map mapScreenToPhysicalCoordinates:a forLayer:0];
   xy[i]=fminf(fmaxf(a.x,.5f),physical.width-.5f);xy[p->size+i]=fminf(fmaxf(a.y,.5f),physical.height-.5f);
  }
  MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:p->size height:p->size mipmapped:NO];
  d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
  id<MTLTexture> resolved=[device newTextureWithDescriptor:d];
  d.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;id<MTLTexture> output=[device newTextureWithDescriptor:d];
  NSString *source=@"#include <metal_stdlib>\nusing namespace metal;\n"
   "kernel void unwarp(texture2d<float,access::sample> src [[texture(0)]],texture2d<float,access::write> dst [[texture(1)]],"
   "constant float *xy [[buffer(0)]],uint2 i [[thread_position_in_grid]]){"
   "if(any(i>=uint2(dst.get_width(),dst.get_height())))return;constexpr sampler s(coord::pixel,filter::linear,address::clamp_to_edge);"
   "dst.write(src.sample(s,float2(xy[i.x],xy[dst.get_width()+i.y])),i);}";
  NSError *error=nil;id<MTLLibrary> lib=[device newLibraryWithSource:source options:nil error:&error];
  id<MTLFunction> function=[lib newFunctionWithName:@"unwarp"];
  id<MTLComputePipelineState> pipeline=function?[device newComputePipelineStateWithFunction:function error:&error]:nil;
  [function release];[lib release];id<MTLCommandQueue> queue=[device newCommandQueue];
  BOOL good=resolved && output && pipeline && queue;double medians[3]={};
  // Interleave/rotate phase order to avoid measuring one kernel only cold and
  // another only warm. First three rounds are discarded for every phase.
  double times[3][21]={};
  for(unsigned n=0;good && n<24;++n){
   for(unsigned step=0;good && step<3;++step){unsigned phase=(n+step)%3;id<MTLCommandBuffer> command=[queue commandBuffer];
    if(phase!=1){
     MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
     pass.colorAttachments[0].texture=(id)p->color;pass.colorAttachments[0].resolveTexture=resolved;
     pass.colorAttachments[0].loadAction=MTLLoadActionLoad;
     // Retain the multisample source across trials and subsequent consumers.
     pass.colorAttachments[0].storeAction=MTLStoreActionStoreAndMultisampleResolve;
     id<MTLRenderCommandEncoder> enc=[command renderCommandEncoderWithDescriptor:pass];[enc endEncoding];
    }
    if(phase!=0){id<MTLComputeCommandEncoder> enc=[command computeCommandEncoder];
     [enc setComputePipelineState:pipeline];[enc setTexture:resolved atIndex:0];[enc setTexture:output atIndex:1];[enc setBuffer:lut offset:0 atIndex:0];
     [enc dispatchThreads:MTLSizeMake(p->size,p->size,1) threadsPerThreadgroup:MTLSizeMake(16,16,1)];[enc endEncoding];
    }
    [command commit];[command waitUntilCompleted];
    good=command.status==MTLCommandBufferStatusCompleted && command.GPUEndTime>command.GPUStartTime;
    if(n>=3)times[phase][n-3]=(command.GPUEndTime-command.GPUStartTime)*1000;
   }
  }
  if(good)for(unsigned phase=0;phase<3;++phase){
   for(unsigned i=0;i<21;++i)for(unsigned j=i+1;j<21;++j)if(times[phase][j]<times[phase][i]){double a=times[phase][i];times[phase][i]=times[phase][j];times[phase][j]=a;}
   medians[phase]=times[phase][10];
  }
  if(good){[(id)p->output_color release];[(id)p->output_depth release];p->output_color=(uint64_t)output;p->output_depth=0;output=nil;
   p->resolve_gpu_ms=medians[0];p->unwarp_gpu_ms=medians[1];p->reconstruction_gpu_ms=medians[2];p->status=0;
  }else if(error)fprintf(stderr,"resolve-first pipeline: %s\n",error.description.UTF8String);
  [queue release];[pipeline release];[resolved release];[output release];[lut release];
 }
 return 0;
}

// Test-only physical-depth seeding from an existing ordinary prepass.
// The caller completes prior D3D work through readback; this is not production sync.
static NTSTATUS compress_prepass(void *args){
 struct depth_foveation_params *p=args;p->status=1;
 @autoreleasepool {
  if(!p->source_depth || !p->map || p->filter>2)return 0;
  id<MTLDevice> device=(id)p->device;id<MTLRasterizationRateMap> map=(id)p->map;
  MTLSize physical=[map physicalSizeForLayer:0];MTLSamplePosition samples[4];
  [device getDefaultSamplePositions:samples count:p->samples];
  NSUInteger stride=physical.width+physical.height;
  id<MTLBuffer> lut=[device newBufferWithLength:p->samples*stride*sizeof(float) options:MTLResourceStorageModeShared];
  float *xy=lut.contents;
  for(unsigned s=0;s<p->samples;++s){
   for(unsigned x=0;x<physical.width;++x){MTLCoordinate2D q=[map mapPhysicalToScreenCoordinates:MTLCoordinate2DMake(x+samples[s].x,samples[s].y) forLayer:0];xy[s*stride+x]=q.x-samples[s].x;}
   for(unsigned y=0;y<physical.height;++y){MTLCoordinate2D q=[map mapPhysicalToScreenCoordinates:MTLCoordinate2DMake(samples[s].x,y+samples[s].y) forLayer:0];xy[s*stride+physical.width+y]=q.y-samples[s].y;}
  }
  NSString *source=@"#include <metal_stdlib>\nusing namespace metal;\n"
   "struct V{float4 p [[position]];};struct O{float d [[depth(any)]];};"
   "vertex V vs(uint i [[vertex_id]]){return {float4(i==2?3.:-1.,i==1?3.:-1.,0,1)};}"
   "fragment O ps(V v [[stage_in]],uint s [[sample_id]],depth2d_ms<float> d [[texture(0)]],constant float *xy [[buffer(0)]],constant uint4 &info [[buffer(1)]]){"
   "uint2 i=uint2(v.p.xy);uint k=s*(info.x+info.y);float2 q=float2(xy[k+i.x],xy[k+info.x+i.y]);"
   "int2 hi=int2(d.get_width()-1,d.get_height()-1);float z=0;"
   "if(info.z==0)z=d.read(uint2(clamp(int2(round(q)),int2(0),hi)),s);"
   "else {int2 b=int2(floor(q+1e-5));float2 f=q-float2(b);z=info.z==1?1.:0.;"
   "for(int y=0;y<2;++y)for(int x=0;x<2;++x){if((x&&f.x<1e-5)||(y&&f.y<1e-5))continue;"
   "float a=d.read(uint2(clamp(b+int2(x,y),int2(0),hi)),s);z=info.z==1?min(z,a):max(z,a);}}return {z};}";
  NSError *error=nil;id<MTLLibrary> lib=[device newLibraryWithSource:source options:nil error:&error];
  MTLRenderPipelineDescriptor *desc=[[MTLRenderPipelineDescriptor alloc] init];
  desc.vertexFunction=[lib newFunctionWithName:@"vs"];desc.fragmentFunction=[lib newFunctionWithName:@"ps"];
  desc.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float;desc.rasterSampleCount=p->samples;
  id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:desc error:&error];
  MTLDepthStencilDescriptor *ds=[[MTLDepthStencilDescriptor alloc] init];ds.depthCompareFunction=MTLCompareFunctionAlways;ds.depthWriteEnabled=YES;
  id<MTLDepthStencilState> state=[device newDepthStencilStateWithDescriptor:ds];
  id<MTLCommandQueue> queue=[device newCommandQueue];BOOL good=lut && pipeline && state && queue;
  if(good){id<MTLCommandBuffer> cb=[queue commandBuffer];MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
   pass.depthAttachment.texture=(id)p->depth;pass.depthAttachment.loadAction=MTLLoadActionClear;pass.depthAttachment.clearDepth=0;pass.depthAttachment.storeAction=MTLStoreActionStore;
   id<MTLRenderCommandEncoder> e=[cb renderCommandEncoderWithDescriptor:pass];
   [e setViewport:(MTLViewport){0,0,physical.width,physical.height,0,1}];[e setRenderPipelineState:pipeline];[e setDepthStencilState:state];
   [e setFragmentTexture:(id)p->source_depth atIndex:0];[e setFragmentBuffer:lut offset:0 atIndex:0];
   uint32_t info[4]={(uint32_t)physical.width,(uint32_t)physical.height,p->filter,0};[e setFragmentBytes:info length:sizeof(info) atIndex:1];
   [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[e endEncoding];[cb commit];[cb waitUntilCompleted];
   good=cb.status==MTLCommandBufferStatusCompleted;
  }
  if(good)p->status=0;else if(error)fprintf(stderr,"prepass seed: %s\n",error.description.UTF8String);
  [queue release];[state release];[ds release];[pipeline release];[desc.vertexFunction release];[desc.fragmentFunction release];[desc release];[lib release];[lut release];
 }return 0;
}

// CPU-only coordinate export for the joined D3D11 timing fixture.
static NTSTATUS axis_lut(void *args){
 struct depth_foveation_params *p=args;p->status=1;
 @autoreleasepool {
  if(!p->axis_lut || !p->size || p->size>4096)return 0;
  id<MTLRasterizationRateMap> map=(id)p->map;
  MTLSize physical=map?[map physicalSizeForLayer:0]:MTLSizeMake(p->size,p->size,1);
  float *xy=(float *)(uintptr_t)p->axis_lut;
  for(unsigned i=0;i<p->size;++i){
   MTLCoordinate2D a=MTLCoordinate2DMake(i+.5f,i+.5f);if(map)a=[map mapScreenToPhysicalCoordinates:a forLayer:0];
   xy[i]=fminf(fmaxf(a.x,.5f),physical.width-.5f);xy[p->size+i]=fminf(fmaxf(a.y,.5f),physical.height-.5f);
  }
  p->status=0;
 }return 0;
}

// CPU-only export reused by the ordinary D3D11 MSAA materialisation fixture.
static NTSTATUS sample_axis_lut(void *args){
 struct depth_foveation_params *p=args;p->status=1;
 @autoreleasepool {if(fill_sample_axes(p,(float *)(uintptr_t)p->axis_lut))p->status=0;}
 return 0;
}

static NTSTATUS release_objects(void *args){struct depth_foveation_params *p=args;
 [(id)p->color release];[(id)p->depth release];[(id)p->event release];[(id)p->map release];
 [(id)p->output_color release];[(id)p->output_depth release];p->color=p->depth=p->event=p->map=p->output_color=p->output_depth=0;return 0;}
const unixlib_entry_t __wine_unix_call_funcs[]={create,reconstruct,release_objects,resolve_first,compress_prepass,axis_lut,sample_axis_lut};

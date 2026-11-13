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
  if(!p->size || p->size>4096 || (p->samples!=2 && p->samples!=4) || ![device supportsTextureSampleCount:p->samples] ||
      ![device supportsRasterizationRateMapWithLayerCount:1])return 0;
  id<MTLRasterizationRateMap> map=nil;
  if(p->mapped) {
   MTLRasterizationRateLayerDescriptor *layer=[[MTLRasterizationRateLayerDescriptor alloc] initWithSampleCount:MTLSizeMake(8,8,1)];
   for(unsigned i=0;i<8;++i){layer.horizontal[i]=@((i==3||i==4)?1.f:.25f);layer.vertical[i]=@((i==3||i==4)?1.f:.25f);}
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
static NTSTATUS reconstruct(void *args) {
 struct depth_foveation_params *p=args;p->status=1;
 @autoreleasepool {
  id<MTLDevice> device=(id<MTLDevice>)p->device;id<MTLRasterizationRateMap> map=(id)p->map;
  if(p->filter>5)return 0;BOOL color_only=p->filter>=3;
  id<MTLSharedEvent> event=(id)p->event;
  for(unsigned i=0;event.signaledValue<p->value && i<10000;++i)usleep(1000);
  if(event.signaledValue<p->value)return 0;
  MTLSize physical=map?[map physicalSizeForLayer:0]:MTLSizeMake(p->size,p->size,1);
  MTLSamplePosition positions[4];[device getDefaultSamplePositions:positions count:p->samples];
  size_t count=p->samples*(2*p->size+2);
  id<MTLBuffer> lut=[device newBufferWithLength:count*sizeof(float) options:MTLResourceStorageModeShared];
  float *xy=lut.contents;
  for(unsigned sample=0;sample<p->samples;++sample){
   for(unsigned i=0;i<p->size;++i){
    MTLCoordinate2D a=MTLCoordinate2DMake(i+positions[sample].x,i+positions[sample].y);
    if(map)a=[map mapScreenToPhysicalCoordinates:a forLayer:0];
    xy[sample*2*p->size+i]=a.x;xy[sample*2*p->size+p->size+i]=a.y;
   }
   xy[2*p->size*p->samples+2*sample]=positions[sample].x;
   xy[2*p->size*p->samples+2*sample+1]=positions[sample].y;
  }
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
static NTSTATUS release_objects(void *args){struct depth_foveation_params *p=args;
 [(id)p->color release];[(id)p->depth release];[(id)p->event release];[(id)p->map release];
 [(id)p->output_color release];[(id)p->output_depth release];p->color=p->depth=p->event=p->map=p->output_color=p->output_depth=0;return 0;}
const unixlib_entry_t __wine_unix_call_funcs[]={create,reconstruct,release_objects};

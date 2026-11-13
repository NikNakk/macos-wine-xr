// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Standalone experiment, not a DXMT optimisation. Synchronous readback is test-only.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static id<MTLTexture> texture(id<MTLDevice> device,NSUInteger width,NSUInteger height,NSUInteger samples){
 MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8 width:width height:height mipmapped:NO];
 d.textureType=samples>1?MTLTextureType2DMultisample:MTLTextureType2D;d.sampleCount=samples;
 d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget|MTLTextureUsagePixelFormatView;
 return [device newTextureWithDescriptor:d];
}
static MTLRenderPassDescriptor *pass(id<MTLTexture> t,BOOL clear){
 MTLRenderPassDescriptor *p=[MTLRenderPassDescriptor renderPassDescriptor];
 p.depthAttachment.texture=t;p.depthAttachment.loadAction=clear?MTLLoadActionClear:MTLLoadActionLoad;
 p.depthAttachment.storeAction=MTLStoreActionStore;p.depthAttachment.clearDepth=.875;
 p.stencilAttachment.texture=t;p.stencilAttachment.loadAction=clear?MTLLoadActionClear:MTLLoadActionLoad;
 p.stencilAttachment.storeAction=MTLStoreActionStore;p.stencilAttachment.clearStencil=clear?0xa7:0;
 return p;
}
static void seed(id<MTLCommandBuffer> cb,id<MTLTexture> src,id<MTLTexture> dst,id<MTLRenderPipelineState> pso,id<MTLDepthStencilState> state,BOOL fused,MTLMultisampleDepthResolveFilter filter){
 MTLRenderPassDescriptor *p=pass(src,YES);p.stencilAttachment.clearStencil=0x35;
 if(fused){p.depthAttachment.resolveTexture=dst;p.depthAttachment.depthResolveFilter=filter;p.depthAttachment.storeAction=MTLStoreActionStoreAndMultisampleResolve;}
 id<MTLRenderCommandEncoder> e=[cb renderCommandEncoderWithDescriptor:p];[e setRenderPipelineState:pso];[e setDepthStencilState:state];uint32_t n=(uint32_t)src.sampleCount;[e setFragmentBytes:&n length:sizeof(n) atIndex:0];[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[e endEncoding];
}
static void resolve(id<MTLCommandBuffer> cb,id<MTLTexture> src,id<MTLTexture> dst,id<MTLRenderPipelineState> pso,id<MTLDepthStencilState> state,int mode,MTLMultisampleDepthResolveFilter filter){
 if(mode==0){
  id<MTLRenderCommandEncoder> e=[cb renderCommandEncoderWithDescriptor:pass(dst,NO)];[e setRenderPipelineState:pso];[e setDepthStencilState:state];[e setFragmentTexture:src atIndex:0];[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[e endEncoding];
 }else if(mode==2){
  MTLRenderPassDescriptor *p=pass(src,NO);p.depthAttachment.resolveTexture=dst;p.depthAttachment.depthResolveFilter=filter;p.depthAttachment.storeAction=MTLStoreActionStoreAndMultisampleResolve;
  id<MTLRenderCommandEncoder> e=[cb renderCommandEncoderWithDescriptor:p];[e endEncoding];
 }
}
static BOOL complete(id<MTLCommandBuffer> cb){[cb commit];[cb waitUntilCompleted];if(cb.status!=MTLCommandBufferStatusCompleted){fprintf(stderr,"GPU error: %s\n",cb.error.description.UTF8String);return NO;}return YES;}
static double median(double *v,unsigned n){for(unsigned i=0;i<n;++i)for(unsigned j=i+1;j<n;++j)if(v[j]<v[i]){double a=v[i];v[i]=v[j];v[j]=a;}return v[n/2];}
int main(int argc,char **argv){@autoreleasepool {
 const unsigned w=1792,h=1008,repeats=8,batches=15,warm=10;
 unsigned samples=argc>1?(unsigned)atoi(argv[1]):2;REQUIRE(samples==2||samples==4);
 id<MTLDevice> device=MTLCreateSystemDefaultDevice();REQUIRE(device && [device supportsTextureSampleCount:samples]);
 id<MTLCommandQueue> queue=[device newCommandQueue];
 NSString *source=@"#include <metal_stdlib>\nusing namespace metal;\n"
 "struct D{float z [[depth(any)]];};struct V{float4 p [[position]];};vertex V vs(uint i [[vertex_id]]){return {float4(i==2?3.f:-1.f,i==1?3.f:-1.f,0,1)};}"
 "fragment D seed(V v [[stage_in]],uint s [[sample_id]],constant uint &n [[buffer(0)]]) {uint2 p=uint2(v.p.xy);uint k=((p.x+p.y)&1)?n-1-s:s;return {.125f+float((p.x*3+p.y)%16)*.03125f+float(k)*.0625f};}"
 "fragment D reduction(V v [[stage_in]],depth2d_ms<float> d [[texture(0)]]){uint2 p=uint2(v.p.xy);float z=d.read(p,0);for(uint s=1;s<d.get_num_samples();++s)z=max(z,d.read(p,s));return {z};}"
 "kernel void inspect(depth2d_ms<float> src [[texture(0)]],depth2d<float> dst [[texture(1)]],texture2d<uint> ss [[texture(2)]],texture2d_ms<uint> ms [[texture(3)]],device uint *out [[buffer(0)]],uint2 p [[thread_position_in_grid]]){uint w=dst.get_width(),i=p.y*w+p.x,n=src.get_num_samples();out[i*(n*2+2)]=as_type<uint>(dst.read(p));out[i*(n*2+2)+1]=ss.read(p).x;for(uint s=0;s<n;++s){out[i*(n*2+2)+2+s]=as_type<uint>(src.read(p,s));out[i*(n*2+2)+2+n+s]=ms.read(p,s).x;}}";
 NSError *error=nil;id<MTLLibrary> lib=[device newLibraryWithSource:source options:nil error:&error];if(!lib)fprintf(stderr,"%s\n",error.description.UTF8String);REQUIRE(lib);
 id<MTLFunction> vs=[lib newFunctionWithName:@"vs"],sf=[lib newFunctionWithName:@"seed"],rf=[lib newFunctionWithName:@"reduction"],inspect=[lib newFunctionWithName:@"inspect"];
 MTLRenderPipelineDescriptor *pd=[[MTLRenderPipelineDescriptor alloc] init];pd.vertexFunction=vs;pd.fragmentFunction=sf;pd.depthAttachmentPixelFormat=pd.stencilAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8;pd.rasterSampleCount=samples;
 id<MTLRenderPipelineState> seedPSO=[device newRenderPipelineStateWithDescriptor:pd error:&error];REQUIRE(seedPSO);
 pd.fragmentFunction=rf;pd.rasterSampleCount=1;id<MTLRenderPipelineState> resolvePSO=[device newRenderPipelineStateWithDescriptor:pd error:&error];REQUIRE(resolvePSO);
 id<MTLComputePipelineState> checkPSO=[device newComputePipelineStateWithFunction:inspect error:&error];if(!checkPSO)fprintf(stderr,"%s\n",error.description.UTF8String);REQUIRE(checkPSO);
 MTLDepthStencilDescriptor *ds=[[MTLDepthStencilDescriptor alloc] init];ds.depthCompareFunction=MTLCompareFunctionAlways;ds.depthWriteEnabled=YES;id<MTLDepthStencilState> state=[device newDepthStencilStateWithDescriptor:ds];
 id<MTLTexture> src=texture(device,w,h,samples),dst=texture(device,w,h,1);REQUIRE(src&&dst);
 id<MTLTexture> ss=[dst newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8],ms=[src newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];REQUIRE(ss&&ms);
 id<MTLBuffer> readback=[device newBufferWithLength:(NSUInteger)w*h*(samples*2+2)*4 options:MTLResourceStorageModeShared];REQUIRE(readback);
 double times[4][15];
 // Rotate case order each batch to reduce warm-up/clock/order bias.
 id<MTLCommandBuffer> init=[queue commandBuffer];id<MTLRenderCommandEncoder> ie=[init renderCommandEncoderWithDescriptor:pass(dst,YES)];[ie endEncoding];REQUIRE(complete(init));
 for(unsigned b=0;b<batches+warm;++b)for(unsigned j=0;j<4;++j){@autoreleasepool{
  unsigned test=(j+b)%4;int mode=test==3?1:test;
  MTLMultisampleDepthResolveFilter filter=test==3?MTLMultisampleDepthResolveFilterSample0:MTLMultisampleDepthResolveFilterMax;
  id<MTLCommandBuffer> cb=[queue commandBuffer];
  for(unsigned i=0;i<repeats;++i){seed(cb,src,dst,seedPSO,state,mode==1,filter);resolve(cb,src,dst,resolvePSO,state,mode,filter);}
  REQUIRE(complete(cb));if(b>=warm)times[test][b-warm]=(cb.GPUEndTime-cb.GPUStartTime)*1000./repeats;
 }}
 unsigned failures=0;
 for(int test=0;test<4;++test){
  int mode=test==3?1:test;MTLMultisampleDepthResolveFilter filter=test==3?MTLMultisampleDepthResolveFilterSample0:MTLMultisampleDepthResolveFilterMax;
  id<MTLCommandBuffer> draw=[queue commandBuffer];seed(draw,src,dst,seedPSO,state,mode==1,filter);resolve(draw,src,dst,resolvePSO,state,mode,filter);REQUIRE(complete(draw));
  id<MTLCommandBuffer> cb=[queue commandBuffer];id<MTLComputeCommandEncoder> ce=[cb computeCommandEncoder];[ce setComputePipelineState:checkPSO];[ce setTexture:src atIndex:0];[ce setTexture:dst atIndex:1];[ce setTexture:ss atIndex:2];[ce setTexture:ms atIndex:3];[ce setBuffer:readback offset:0 atIndex:0];[ce dispatchThreads:MTLSizeMake(w,h,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)];[ce endEncoding];REQUIRE(complete(cb));
  uint32_t *data=readback.contents;unsigned wrongDepth=0,wrongStencil=0,wrongSource=0;
  for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
   uint32_t *row=data+((NSUInteger)y*w+x)*(samples*2+2);float z;memcpy(&z,row,4);
   float base=.125f+((x*3+y)%16)*.03125f;wrongDepth+=!isfinite(z)||fabsf(z-(base+(samples-1)*.0625f))>1e-7f;
   wrongStencil+=row[1]!=0xa7;
   for(unsigned sample=0;sample<samples;++sample){float a;memcpy(&a,row+2+sample,4);unsigned k=((x+y)&1)?samples-1-sample:sample;wrongSource+=!isfinite(a)||fabsf(a-(base+k*.0625f))>1e-7f;wrongStencil+=row[2+samples+sample]!=0x35;}
  }
  BOOL ok=(test==3?wrongDepth>0:wrongDepth==0)&&wrongStencil==0&&wrongSource==0;failures+=!ok;
  printf("DEPTH_RESOLVE mode=%s samples=%u width=%u height=%u batches=%u repeats=%u gpu_chain_ms=%.6f depth_wrong=%u stencil_wrong=%u source_wrong=%u control=%u pass=%u\n",test==0?"shader":test==1?"fused-max":test==2?"separate-max":"fused-sample0",samples,w,h,batches,repeats,median(times[test],batches),wrongDepth,wrongStencil,wrongSource,test==3,ok);
 }
 [readback release];[ss release];[ms release];[src release];[dst release];[state release];[ds release];[pd release];[seedPSO release];[resolvePSO release];[checkPSO release];[vs release];[sf release];[rf release];[inspect release];[lib release];[queue release];[device release];return failures?1:0;
}}

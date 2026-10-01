// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
/* Validate on the GPU; only a four-byte mismatch count is read on the CPU.
 * There is no image readback or CPU image copy in this proof. */
static bool verify(id<MTLDevice> device, id<MTLTexture> texture, unsigned image)
{
	NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
	    "kernel void verify(texture2d_array<float, access::read> t [[texture(0)]], "
	    "device atomic_uint *errors [[buffer(0)]], constant uint &image [[buffer(1)]], "
	    "uint3 p [[thread_position_in_grid]]) { float4 expected=float4(0,0,0,1); "
	    "expected[(image+p.z)%3]=1; if (any(abs(t.read(p.xy,p.z)-expected)>0.01)) "
	    "atomic_fetch_add_explicit(errors,1,memory_order_relaxed); }";
	NSError *error = nil;
	id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
	id<MTLFunction> function = [library newFunctionWithName:@"verify"];
	id<MTLComputePipelineState> pipeline = function ? [device newComputePipelineStateWithFunction:function error:&error] : nil;
	id<MTLBuffer> errors = [device newBufferWithLength:4 options:MTLResourceStorageModeShared];
	id<MTLCommandQueue> queue = [device newCommandQueue];
	id<MTLTexture> array = [texture newTextureViewWithPixelFormat:texture.pixelFormat
	    textureType:MTLTextureType2DArray levels:NSMakeRange(0,1) slices:NSMakeRange(0,texture.arrayLength)];
	bool ok = false;
	if (pipeline && errors && queue && array) {
		*(uint32_t *)errors.contents = 0;
		id<MTLCommandBuffer> command = [queue commandBuffer];
		id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
		[compute setComputePipelineState:pipeline]; [compute setTexture:array atIndex:0];
		[compute setBuffer:errors offset:0 atIndex:0]; [compute setBytes:&image length:sizeof(image) atIndex:1];
		[compute dispatchThreads:MTLSizeMake(texture.width,texture.height,texture.arrayLength)
		    threadsPerThreadgroup:MTLSizeMake(8,8,1)];
		[compute endEncoding]; [command commit]; [command waitUntilCompleted];
		ok = command.status == MTLCommandBufferStatusCompleted && *(uint32_t *)errors.contents == 0;
		fprintf(stderr,"verify image=%u array=%lu mismatches=%u\n",image,(unsigned long)texture.arrayLength,*(uint32_t *)errors.contents);
	} else fprintf(stderr,"GPU verification setup failed: %s\n",error.description.UTF8String);
	[array release]; [queue release]; [errors release]; [pipeline release]; [function release]; [library release];
	return ok;
}

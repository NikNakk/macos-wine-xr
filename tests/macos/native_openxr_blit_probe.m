// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "macos_wine_xr/native_openxr_backend.h"
#include "metal_pattern_verify.h"

int main(int argc, char **argv)
{
	if (argc > 2) return 2;
	@autoreleasepool {
		struct mwxr_native_backend *b = NULL;
		struct mwxr_native_swapchain *s = NULL;
		id<MTLTexture> source = nil;
		id<MTLSharedEvent> event = nil;
		id<MTLCommandQueue> queue = nil;
		int ret = 1;
		if (XR_FAILED(mwxr_native_backend_open(argc == 2 ? argv[1] : NULL, &b))) return 1;
		id<MTLDevice> device = (id<MTLDevice>)mwxr_native_backend_metal_device(b);
		XrSwapchainCreateInfo ci = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
		    .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
		    .format = MTLPixelFormatRGBA8Unorm_sRGB, .sampleCount = 1, .width = 64, .height = 32,
		    .faceCount = 1, .arraySize = 2, .mipCount = 1};
		if (XR_FAILED(mwxr_native_swapchain_create(b, &ci, &s))) goto done;
		MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:ci.format
		    width:ci.width height:ci.height mipmapped:NO];
		desc.textureType = MTLTextureType2DArray; desc.arrayLength = ci.arraySize;
		desc.storageMode = MTLStorageModePrivate;
		desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		source = [device newSharedTextureWithDescriptor:desc];
		event = [device newSharedEvent]; queue = [device newCommandQueue];
		if (!source || !event || !queue) goto done;
		for (uint32_t frame = 0; frame < mwxr_native_swapchain_image_count(s); ++frame) {
			uint32_t index;
			if (mwxr_native_swapchain_acquire(s,&index) != XR_SUCCESS ||
			    mwxr_native_swapchain_wait(s,1000000000ll) != XR_SUCCESS) goto done;
			id<MTLCommandBuffer> command = [queue commandBuffer];
			for (uint32_t slice = 0; slice < ci.arraySize; ++slice) {
				MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
				pass.colorAttachments[0].texture = source; pass.colorAttachments[0].slice = slice;
				pass.colorAttachments[0].loadAction = MTLLoadActionClear;
				pass.colorAttachments[0].storeAction = MTLStoreActionStore;
				double color[4] = {0,0,0,1}; color[(frame+slice)%3] = 1;
				pass.colorAttachments[0].clearColor = MTLClearColorMake(color[0],color[1],color[2],color[3]);
				id<MTLRenderCommandEncoder> render = [command renderCommandEncoderWithDescriptor:pass];
				[render endEncoding];
			}
			// Check an unsignaled producer fence returns timeout without copying/releasing.
			struct mwxr_blit_timing timing;
			if (mwxr_native_swapchain_blit(s,index,source,event,frame+1,0,&timing) != XR_TIMEOUT_EXPIRED) goto done;
			[command encodeSignalEvent:event value:frame+1]; [command commit];
			if (mwxr_native_swapchain_blit(s,index,source,event,frame+1,1000000000ll,&timing) != XR_SUCCESS) goto done;
			id<MTLTexture> target = (id<MTLTexture>)mwxr_native_swapchain_metal_texture(s,index);
			if (!verify(device,target,frame)) goto done;
			if (mwxr_native_swapchain_release(s) != XR_SUCCESS) goto done;
			printf("frame=%u image=%u fence_wait_ns=%llu copy_wall_ns=%llu copy_gpu_ns=%.0f\n",frame,index,
			    (unsigned long long)timing.fence_wait_ns,
			    (unsigned long long)(timing.copy_complete_ns-timing.copy_submit_ns),
			    (timing.gpu_end_seconds-timing.gpu_start_seconds)*1e9);
		}
		printf("runtime=%s\none-GPU-blit array-pattern verification passed\n",mwxr_native_backend_info(b)->runtime.runtimeName);
		ret = 0;
done:
		[queue release]; [event release]; [source release];
		mwxr_native_swapchain_destroy(s); mwxr_native_backend_close(b);
		return ret;
	}
}

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "macos_wine_xr/native_metal_sharing.h"
#include <mach/mach.h>
#include "macos_wine_xr/native_capability.h"
#include <stdio.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#ifdef MWXR_HAVE_NATIVE_OPENXR
#include "macos_wine_xr/native_openxr_backend.h"
#endif

@interface MTLSharedTextureHandle (MWXRReverseProbe)
- (mach_port_t)createMachPort;
@end
@interface MTLSharedEventHandle (MWXRReverseEvent)
- (mach_port_t)eventPort;
@end

#include "metal_pattern_verify.h"


int main(int argc, char **argv)
{
	if (argc != 3 && argc != 4) { fprintf(stderr,"usage: %s wine reverse-import-probe.exe [loader.dylib]\n",argv[0]); return 2; }
	signal(SIGPIPE,SIG_IGN);
	@autoreleasepool {
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
#ifdef MWXR_HAVE_NATIVE_OPENXR
		struct mwxr_native_backend *backend = NULL;
		if (argc == 4) {
			if (XR_FAILED(mwxr_native_backend_open(argv[3],&backend))) { [device release]; return 1; }
			[device release]; device = [(id<MTLDevice>)mwxr_native_backend_metal_device(backend) retain];
		}
#else
		if (argc == 4) { [device release]; return 2; }
#endif
		if (!device) return 1;
		int ret = 0;
		for (unsigned array_size = 1; array_size <= 2 && !ret; ++array_size) {
			unsigned image_count = 3;
			MTLPixelFormat format = argc == 4 ? MTLPixelFormatRGBA8Unorm_sRGB : MTLPixelFormatRGBA8Unorm;
#ifdef MWXR_HAVE_NATIVE_OPENXR
			struct mwxr_native_swapchain *swapchain = NULL;
			bool direct = false;
			if (backend) {
				XrSwapchainCreateInfo ci = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
				    .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
				    .format = format, .sampleCount = 1, .width = 64, .height = 32,
				    .faceCount = 1, .arraySize = array_size, .mipCount = 1};
				if (XR_FAILED(mwxr_native_swapchain_create(backend,&ci,&swapchain))) { ret = 1; break; }
				image_count = mwxr_native_swapchain_image_count(swapchain);
				if (image_count == 0 || image_count > 16) {
					fprintf(stderr,"unsupported runtime image count\n");
					mwxr_native_swapchain_destroy(swapchain); ret = 1; break;
				}
				direct = mwxr_native_swapchain_image(swapchain,0)->strategy == MWXR_IMAGE_SHARED_METAL;
				printf("runtime=%s array=%u strategy=%s\n",mwxr_native_backend_info(backend)->runtime.runtimeName,
				    array_size,direct ? "shared-metal-zero-copy" : "gpu-blit");

			}
#endif
			id<MTLSharedEvent> event = [device newSharedEvent];
			MTLSharedEventHandle *event_handle = [event newSharedEventHandle];
			mach_port_t event_port = [event_handle eventPort];
			struct mwxr_native_capability *event_cap = NULL;
			bool event_registered = event_port && !mwxr_native_capability_create(event_port,2,&event_cap);
			NSString *event_name = event_cap ? [NSString stringWithUTF8String:mwxr_native_capability_name(event_cap)] : @"";
			[event_handle release]; // eventPort is borrowed from this handle.
			if (!event_registered) ret = 1;
			id<MTLTexture> textures[16] = {nil};
			NSString *names[16] = {nil};
			struct mwxr_native_capability *caps[16] = {NULL};
			MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
			    width:64 height:32 mipmapped:NO];
			desc.textureType = array_size == 1 ? MTLTextureType2D : MTLTextureType2DArray;
			desc.arrayLength = array_size; desc.storageMode = MTLStorageModePrivate;
			desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
			for (unsigned i = 0; i < image_count; ++i) {
				mach_port_t port = MACH_PORT_NULL;
#ifdef MWXR_HAVE_NATIVE_OPENXR
				if (direct) {
					textures[i] = [(id<MTLTexture>)mwxr_native_swapchain_metal_texture(swapchain,i) retain];
					if (XR_FAILED(mwxr_native_swapchain_export(swapchain,i,&port))) ret = 1;
				} else
#endif
				{
					textures[i] = [device newSharedTextureWithDescriptor:desc];
					MTLSharedTextureHandle *handle = [textures[i] newSharedTextureHandle];
					port = [handle createMachPort]; [handle release];
				}
				bool published = port && !mwxr_native_capability_create(port,1,&caps[i]);
				names[i] = caps[i] ? [NSString stringWithUTF8String:mwxr_native_capability_name(caps[i])] : @"";
				if (port) mach_port_deallocate(mach_task_self(),port);
				if (!published) ret = 1;
			}
			NSTask *task = [[NSTask alloc] init];
			NSPipe *input = [NSPipe pipe], *output = [NSPipe pipe];
			task.executableURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]];
			NSMutableArray *arguments = [NSMutableArray arrayWithObjects:[NSString stringWithUTF8String:argv[2]],
			    [NSString stringWithFormat:@"%u",array_size], format == MTLPixelFormatRGBA8Unorm ? @"28" : @"29", nil];
			for (unsigned i = 0; i < image_count; ++i) { [arguments addObject:names[i]]; }
			[arguments addObject:event_name];
			task.arguments = arguments;
			task.standardInput = input; task.standardOutput = output;
			NSError *error = nil;
			bool launched = !ret && [task launchAndReturnError:&error];
			if (launched) {
				FILE *stream = fdopen(dup(output.fileHandleForReading.fileDescriptor),"r");
				if (stream) setvbuf(stream,NULL,_IONBF,0);
				char line[256];
				bool found = false;
				struct pollfd fd = {.fd = stream ? fileno(stream) : -1, .events = POLLIN};
				unsigned rendered = 0, expected = 0;
				while (stream) {
					int ready = poll(&fd,1,30000);
					if (ready < 0 && errno == EINTR) continue;
					if (ready <= 0 || !fgets(line,sizeof(line),stream)) break;
					if (!strncmp(line,"imported",8)) {
						mwxr_native_capability_close(event_cap); event_cap = NULL;
						for (unsigned i = 0; i < image_count; ++i) {
							mwxr_native_capability_close(caps[i]); caps[i] = NULL;
						}
					} else {
						unsigned index = image_count;
						if (sscanf(line,"rendered=%u",&index) != 1 || index != expected) {
							fprintf(stderr,"unexpected renderer response: %s\n",line); break;
						}
						if (![event waitUntilSignaledValue:rendered+1 timeoutMS:10000]) break;
						id<MTLTexture> target = textures[index];
#ifdef MWXR_HAVE_NATIVE_OPENXR
						if (swapchain && !direct) {
							struct mwxr_blit_timing timing;
							if (mwxr_native_swapchain_blit(swapchain,index,textures[index],event,rendered+1,
							    1000000000ll,&timing) != XR_SUCCESS) break;
							target = (id<MTLTexture>)mwxr_native_swapchain_metal_texture(swapchain,index);
							printf("copy_gpu_ns=%.0f\n",(timing.gpu_end_seconds-timing.gpu_start_seconds)*1e9);
						}
#endif
						if (!verify(device,target,index)) break;
#ifdef MWXR_HAVE_NATIVE_OPENXR
						if (swapchain && mwxr_native_swapchain_release(swapchain) != XR_SUCCESS) break;
#endif
						if (++rendered == image_count) { found = true; break; }
					}
					expected = rendered;
#ifdef MWXR_HAVE_NATIVE_OPENXR
					if (swapchain && (mwxr_native_swapchain_acquire(swapchain,&expected) != XR_SUCCESS ||
					    mwxr_native_swapchain_wait(swapchain,1000000000ll) != XR_SUCCESS)) break;
#endif
					char command[16]; int length = snprintf(command,sizeof(command),"%u\n",expected);
					if (write(input.fileHandleForWriting.fileDescriptor,command,(size_t)length) != length) break;
				}
				ret = !found;
				if (!found) fprintf(stderr,"Wine renderer did not complete GPU verification\n");
				ssize_t written = write(input.fileHandleForWriting.fileDescriptor,"\n",1);
				if (written != 1) ret = 1;
				[input.fileHandleForWriting closeFile];
				if (stream) fclose(stream);
				if (!found && task.running) [task terminate];
				[task waitUntilExit];
				if (task.terminationStatus) fprintf(stderr,"Wine renderer exit=%d\n",task.terminationStatus);
				ret |= task.terminationStatus != 0;
			} else { fprintf(stderr,"Wine launch failed: %s\n",error.description.UTF8String); ret = 1; }
			[task release];
			mwxr_native_capability_close(event_cap);
			[event release];
			for (unsigned i = 0; i < image_count; ++i) {
				mwxr_native_capability_close(caps[i]);
				[textures[i] release];
			}
#ifdef MWXR_HAVE_NATIVE_OPENXR
			mwxr_native_swapchain_destroy(swapchain);
#endif
		}
		[device release];
#ifdef MWXR_HAVE_NATIVE_OPENXR
		mwxr_native_backend_close(backend);
#endif
		if (!ret) puts("native-to-D3D11: 2D/array/all-runtime-images verified on GPU");
		return ret;
	}
}

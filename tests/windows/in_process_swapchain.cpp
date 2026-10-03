// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_NO_PROTOTYPES
#include <windows.h>
#include <d3d11_4.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cstdio>
#include <vector>
#include "../in_process/metal_probe.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAILED line %d: %s\n",__LINE__,#x); return 1; } } while (0)
#define XR(x) do { XrResult check_result=(x); if (XR_FAILED(check_result)) { std::fprintf(stderr,"FAILED line %d: %s = %d\n",__LINE__,#x,check_result); return 1; } } while (0)
int main()
{
    HMODULE runtime=LoadLibraryA("wineopenxr.dll"), helper=LoadLibraryA("wine_metal_probe.dll");
    CHECK(runtime && helper);
    auto gipa=(PFN_xrGetInstanceProcAddr)GetProcAddress(runtime,"xrGetInstanceProcAddr");
    auto image_object=(XrResult (WINAPI *)(XrSwapchain,uint32_t,UINT64 *,UINT64 *))GetProcAddress(runtime,"MWXRProbeSwapchainImage");
    auto probe=(LONG (WINAPI *)(UINT,void *))GetProcAddress(helper,"MWXRMetalProbe");
    CHECK(gipa && image_object && probe);
    XrInstance instance=XR_NULL_HANDLE;
    PFN_xrCreateInstance xrCreateInstance=nullptr;
    XR(gipa(XR_NULL_HANDLE,"xrCreateInstance",(PFN_xrVoidFunction *)&xrCreateInstance));
    const char *extension=XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
    XrInstanceCreateInfo ci={XR_TYPE_INSTANCE_CREATE_INFO};
    ci.applicationInfo.apiVersion=XR_API_VERSION_1_0; ci.enabledExtensionCount=1; ci.enabledExtensionNames=&extension;
    strcpy(ci.applicationInfo.applicationName,"runtime-image-probe");
    XR(xrCreateInstance(&ci,&instance));
#define LOAD(name) PFN_##name name=nullptr; XR(gipa(instance,#name,(PFN_xrVoidFunction *)&name))
    LOAD(xrGetSystem); LOAD(xrGetD3D11GraphicsRequirementsKHR); LOAD(xrCreateSession); LOAD(xrPollEvent);
    LOAD(xrBeginSession); LOAD(xrCreateSwapchain); LOAD(xrEnumerateSwapchainImages); LOAD(xrAcquireSwapchainImage);
    LOAD(xrWaitSwapchainImage); LOAD(xrReleaseSwapchainImage); LOAD(xrDestroySwapchain); LOAD(xrDestroySession);
    LOAD(xrDestroyInstance); LOAD(xrWaitFrame); LOAD(xrBeginFrame); LOAD(xrEndFrame);
    XrSystemGetInfo si={XR_TYPE_SYSTEM_GET_INFO}; si.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system; XR(xrGetSystem(instance,&si,&system));
    XrGraphicsRequirementsD3D11KHR requirements={XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XR(xrGetD3D11GraphicsRequirementsKHR(instance,system,&requirements));
    ID3D11Device *device=nullptr; ID3D11DeviceContext *context=nullptr;
    CHECK(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)));
    // Backend-neutral: readback ordering uses a plain D3D11 fence completed on the CPU.
    ID3D11Device5 *device5=nullptr; ID3D11DeviceContext4 *context4=nullptr;
    CHECK(SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Device5),(void **)&device5)));
    CHECK(SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext4),(void **)&context4)));
    XrGraphicsBindingD3D11KHR binding={XR_TYPE_GRAPHICS_BINDING_D3D11_KHR}; binding.device=device;
    XrSessionCreateInfo sci={XR_TYPE_SESSION_CREATE_INFO}; sci.next=&binding; sci.systemId=system;
    XrSession session; XR(xrCreateSession(instance,&sci,&session));
    bool ready=false;
    for (unsigned attempt=0;attempt<500 && !ready;++attempt) {
        XrEventDataBuffer event={XR_TYPE_EVENT_DATA_BUFFER};
        XrResult result=xrPollEvent(instance,&event); CHECK(XR_SUCCEEDED(result));
        if (result==XR_SUCCESS && event.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            ready=((XrEventDataSessionStateChanged *)&event)->state==XR_SESSION_STATE_READY;
        if (!ready) Sleep(10);
    }
    CHECK(ready);
    XrSessionBeginInfo begin={XR_TYPE_SESSION_BEGIN_INFO}; begin.primaryViewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    XR(xrBeginSession(session,&begin));
    for (unsigned arrays=1;arrays<=2;++arrays) {
        XrSwapchainCreateInfo swapinfo={XR_TYPE_SWAPCHAIN_CREATE_INFO};
        // Include OpenComposite's requested usage; verify the same runtime
        // image identity and GPU pixels for both 2D and array swapchains.
        swapinfo.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT|
                            XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        swapinfo.format=DXGI_FORMAT_R8G8B8A8_UNORM; swapinfo.sampleCount=1; swapinfo.width=swapinfo.height=8;
        swapinfo.faceCount=swapinfo.mipCount=1; swapinfo.arraySize=arrays;
        XrSwapchain swapchain; XR(xrCreateSwapchain(session,&swapinfo,&swapchain));
        uint32_t count=0; XR(xrEnumerateSwapchainImages(swapchain,0,&count,nullptr)); CHECK(count>0);
        std::vector<XrSwapchainImageD3D11KHR> images(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        XR(xrEnumerateSwapchainImages(swapchain,count,&count,(XrSwapchainImageBaseHeader *)images.data()));
        std::vector<bool> seen(count,false); uint32_t remaining=count;
        metal_probe_params p={};
        UINT64 texture=0; XR(image_object(swapchain,0,&p.device,&texture));
        CHECK(probe(3,&p)==0 && p.status==0);
        ID3D11Fence *fence=nullptr; CHECK(SUCCEEDED(device5->CreateFence(0,D3D11_FENCE_FLAG_NONE,__uuidof(ID3D11Fence),(void **)&fence)));
        UINT64 fence_value=0;
        for (unsigned iteration=0;iteration<count*8 && remaining;++iteration) {
            XrFrameWaitInfo wi={XR_TYPE_FRAME_WAIT_INFO}; XrFrameState fs={XR_TYPE_FRAME_STATE}; XR(xrWaitFrame(session,&wi,&fs));
            XrFrameBeginInfo bi={XR_TYPE_FRAME_BEGIN_INFO}; XR(xrBeginFrame(session,&bi));
            XrSwapchainImageAcquireInfo ai={XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO}; uint32_t index;
            XR(xrAcquireSwapchainImage(swapchain,&ai,&index)); CHECK(index<count);
            XrSwapchainImageWaitInfo wait={XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wait.timeout=5000000000LL;
            XR(xrWaitSwapchainImage(swapchain,&wait));
            XR(image_object(swapchain,index,&p.device,&p.texture));
            for (unsigned slice=0;slice<arrays;++slice) {
                D3D11_RENDER_TARGET_VIEW_DESC view={}; view.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
                if (arrays==1) view.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
                else { view.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2DARRAY; view.Texture2DArray.FirstArraySlice=slice; view.Texture2DArray.ArraySize=1; }
                ID3D11RenderTargetView *rtv=nullptr; CHECK(SUCCEEDED(device->CreateRenderTargetView(images[index].texture,&view,&rtv)));
                float color[4]={slice ? 0.f:1.f,slice ? 1.f:0.f,index&1 ? 1.f:0.f,1.f};
                context->ClearRenderTargetView(rtv,color); rtv->Release();
            }
            CHECK(SUCCEEDED(context4->Signal(fence,++fence_value))); context->Flush();
            for (unsigned wait=0;fence->GetCompletedValue()<fence_value && wait<5000;++wait) Sleep(1);
            CHECK(fence->GetCompletedValue()>=fence_value); p.value=0;
            for (unsigned slice=0;slice<arrays;++slice) {
                p.slice=slice; p.timeout_ms=5000; p.expected_pixel=0xff000000u|((index&1)?0xff0000u:0)| (slice?0xff00u:0xffu);
                CHECK(probe(1,&p)==0 && p.status==0);
                std::printf("PASS runtime-image=%u/%u arrays=%u slice=%u Metal=%llx pixel=%08x\n",index,count,arrays,slice,(unsigned long long)p.texture,p.pixel);
            }
            if (!seen[index]) { seen[index]=true; --remaining; }
            XrSwapchainImageReleaseInfo release={XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; XR(xrReleaseSwapchainImage(swapchain,&release));
            XrFrameEndInfo end={XR_TYPE_FRAME_END_INFO}; end.displayTime=fs.predictedDisplayTime; end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            XR(xrEndFrame(session,&end));
        }
        CHECK(remaining==0); fence->Release(); CHECK(probe(4,&p)==0); XR(xrDestroySwapchain(swapchain));
    }
    XR(xrDestroySession(session)); XR(xrDestroyInstance(instance));
    context4->Release(); device5->Release(); context->Release(); device->Release();
    std::puts("PASS all runtime-owned 2D and array images/slices");
    return 0;
}

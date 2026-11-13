// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_NO_PROTOTYPES
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include "dxmt_native_interop.h"
#include "../../src/in_process/foveation_d3d11.h"
#include "../in_process/foveation_probe.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAILED line %d: %s\n",__LINE__,#x); return 1; } } while (0)
#define XR(x) do { XrResult r=(x); if (XR_FAILED(r)) { std::fprintf(stderr,"FAILED line %d: %s = %d\n",__LINE__,#x,r); return 1; } } while (0)

static int run_session()
{
    HMODULE runtime = LoadLibraryA("wineopenxr.dll"), helper = LoadLibraryA("wine_foveation_probe.dll");
    CHECK(runtime && helper);
    auto gipa = (PFN_xrGetInstanceProcAddr)GetProcAddress(runtime,"xrGetInstanceProcAddr");
    auto image_object = (XrResult (WINAPI *)(XrSwapchain,uint32_t,UINT64 *,UINT64 *))GetProcAddress(runtime,"MWXRProbeSwapchainImage");
    auto image_map = (UINT64 (WINAPI *)(XrSwapchain))GetProcAddress(runtime,"MWXRProbeFoveationMap");
    auto probe = (LONG (WINAPI *)(UINT,void *))GetProcAddress(helper,"MWXRMetalProbe");
    CHECK(gipa && image_object && image_map && probe);
    PFN_xrCreateInstance xrCreateInstance = nullptr;
    XR(gipa(XR_NULL_HANDLE,"xrCreateInstance",(PFN_xrVoidFunction *)&xrCreateInstance));
    const char *extensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_FB_FOVEATION_EXTENSION_NAME,
        XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME, XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME,
        XR_MWXR_FOVEATION_D3D11_EXTENSION_NAME, XR_META_FOVEATION_EYE_TRACKED_EXTENSION_NAME};
    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    std::strcpy(ci.applicationInfo.applicationName,"D3D11-foveation-proof");
    ci.enabledExtensionCount = 1; ci.enabledExtensionNames = extensions;
    XrInstance no_foveation; XR(xrCreateInstance(&ci,&no_foveation));
    PFN_xrVoidFunction unavailable = nullptr;
    CHECK(gipa(no_foveation,"xrApplyFoveationD3D11MWXR",&unavailable)==XR_ERROR_FUNCTION_UNSUPPORTED && !unavailable);
    PFN_xrDestroyInstance destroy = nullptr;
    XR(gipa(no_foveation,"xrDestroyInstance",(PFN_xrVoidFunction *)&destroy)); XR(destroy(no_foveation));
    ci.enabledExtensionCount = 6;
    XrInstance instance; XR(xrCreateInstance(&ci,&instance));
#define LOAD(name) PFN_##name name=nullptr; XR(gipa(instance,#name,(PFN_xrVoidFunction *)&name))
    LOAD(xrGetSystem); LOAD(xrGetD3D11GraphicsRequirementsKHR); LOAD(xrCreateSession); LOAD(xrPollEvent);
    LOAD(xrBeginSession); LOAD(xrCreateSwapchain); LOAD(xrEnumerateSwapchainImages); LOAD(xrAcquireSwapchainImage);
    LOAD(xrWaitSwapchainImage); LOAD(xrReleaseSwapchainImage); LOAD(xrDestroySwapchain); LOAD(xrDestroySession);
    LOAD(xrDestroyInstance); LOAD(xrWaitFrame); LOAD(xrBeginFrame); LOAD(xrEndFrame);
    LOAD(xrCreateFoveationProfileFB); LOAD(xrDestroyFoveationProfileFB); LOAD(xrUpdateSwapchainFB);
    LOAD(xrGetSwapchainStateFB);
    LOAD(xrApplyFoveationD3D11MWXR); LOAD(xrCreateReferenceSpace); LOAD(xrDestroySpace); LOAD(xrLocateViews);
    XrSystemGetInfo si = {XR_TYPE_SYSTEM_GET_INFO}; si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system; XR(xrGetSystem(instance,&si,&system));
    XrGraphicsRequirementsD3D11KHR requirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XR(xrGetD3D11GraphicsRequirementsKHR(instance,system,&requirements));
    ID3D11Device *device = nullptr; ID3D11DeviceContext *context = nullptr;
    CHECK(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)));
    ID3D11Device5 *device5 = nullptr; ID3D11DeviceContext4 *context4 = nullptr; IDXMTNativeDevice2 *native = nullptr;
    CHECK(SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Device5),(void **)&device5)));
    CHECK(SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext4),(void **)&context4)));
    CHECK(SUCCEEDED(device->QueryInterface(DXMT_IID_NATIVE_DEVICE2,(void **)&native)));
    XrGraphicsBindingD3D11KHR binding = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR}; binding.device = device;
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO}; sci.next = &binding; sci.systemId = system;
    XrSession session; XR(xrCreateSession(instance,&sci,&session));
    bool ready = false;
    for (unsigned i=0;i<500 && !ready;++i) {
        XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
        XrResult result = xrPollEvent(instance,&event); CHECK(XR_SUCCEEDED(result));
        if (result==XR_SUCCESS && event.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            ready = ((XrEventDataSessionStateChanged *)&event)->state==XR_SESSION_STATE_READY;
        if (!ready) Sleep(10);
    }
    CHECK(ready);
    XrSessionBeginInfo begin = {XR_TYPE_SESSION_BEGIN_INFO}; begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    XR(xrBeginSession(session,&begin));
    XrReferenceSpaceCreateInfo rs = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; rs.poseInReferenceSpace.orientation.w = 1;
    XrSpace space; XR(xrCreateReferenceSpace(session,&rs,&space));
    XrFoveationProfileFB profiles[3] = {};
    for (unsigned i=0;i<3;++i) {
        XrFoveationLevelProfileCreateInfoFB level = {XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB};
        level.level = i==2 ? XR_FOVEATION_LEVEL_NONE_FB : XR_FOVEATION_LEVEL_HIGH_FB;
        level.verticalOffset = i==0 ? -15.0f : i==1 ? 15.0f : 0.0f;
        level.dynamic = XR_FOVEATION_DYNAMIC_DISABLED_FB;
        XrFoveationProfileCreateInfoFB profile = {XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB}; profile.next = &level;
        XR(xrCreateFoveationProfileFB(session,&profile,&profiles[i]));
    }
    XrSwapchain swaps[2]; std::vector<XrSwapchainImageD3D11KHR> images[2];
    for (unsigned eye=0;eye<2;++eye) {
        XrSwapchainCreateInfoFoveationFB f = {XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB};
        f.flags = XR_SWAPCHAIN_CREATE_FOVEATION_SCALED_BIN_BIT_FB;
        XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO}; info.next = &f;
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT|XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
        info.format = DXGI_FORMAT_R8G8B8A8_UNORM; info.width = info.height = FOVEATION_PROBE_SIZE;
        info.sampleCount = info.faceCount = info.mipCount = info.arraySize = 1;
        XR(xrCreateSwapchain(session,&info,&swaps[eye]));
        uint32_t count = 0; XR(xrEnumerateSwapchainImages(swaps[eye],0,&count,nullptr));
        images[eye].resize(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        XR(xrEnumerateSwapchainImages(swaps[eye],count,&count,(XrSwapchainImageBaseHeader *)images[eye].data()));
        XrFoveationD3D11StateMWXR state = {XR_TYPE_FOVEATION_D3D11_STATE_MWXR};
        CHECK(xrApplyFoveationD3D11MWXR(swaps[eye],eye,&state)==XR_ERROR_CALL_ORDER_INVALID);
        XrSwapchainStateFoveationFB update={XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB}; update.profile=profiles[eye];
        XR(xrUpdateSwapchainFB(swaps[eye],(XrSwapchainStateBaseHeaderFB *)&update));
    }
    // This service deliberately has no eye device. The native runtime must
    // reject the tracked profile explicitly and preserve the fixed profile.
    XrFoveationEyeTrackedProfileCreateInfoMETA tracked={XR_TYPE_FOVEATION_EYE_TRACKED_PROFILE_CREATE_INFO_META};
    XrFoveationLevelProfileCreateInfoFB tracked_level={XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB};
    tracked_level.level=XR_FOVEATION_LEVEL_HIGH_FB; tracked_level.next=&tracked;
    XrFoveationProfileCreateInfoFB tracked_info={XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB}; tracked_info.next=&tracked_level;
    XrFoveationProfileFB tracked_profile; XR(xrCreateFoveationProfileFB(session,&tracked_info,&tracked_profile));
    XrSwapchainStateFoveationFB attempted={XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB}; attempted.profile=tracked_profile;
    CHECK(xrUpdateSwapchainFB(swaps[0],(XrSwapchainStateBaseHeaderFB *)&attempted)==XR_ERROR_FEATURE_UNSUPPORTED);
    XrSwapchainStateFoveationFB preserved={XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB};
    XR(xrGetSwapchainStateFB(swaps[0],(XrSwapchainStateBaseHeaderFB *)&preserved)); CHECK(preserved.profile==profiles[0]);
    XR(xrDestroyFoveationProfileFB(tracked_profile));
    std::puts("PASS disabled-extension gating and missing-eye-device rejection without changing the fixed profile");
    const char *hlsl = "struct V {float4 p:SV_Position;float2 uv:TEXCOORD;};"
        "V vs(uint i:SV_VertexID){V v;float2 p=float2(i==2?3:-1,i==1?3:-1);"
        "v.p=float4(p,0,1);v.uv=float2((p.x+1)*.5,(1-p.y)*.5);return v;}"
        "float4 ps(V v):SV_Target{return float4(v.uv,.25,1);}";
    ID3DBlob *vs=nullptr,*ps=nullptr;
    CHECK(SUCCEEDED(D3DCompile(hlsl,std::strlen(hlsl),nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&vs,nullptr)));
    CHECK(SUCCEEDED(D3DCompile(hlsl,std::strlen(hlsl),nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&ps,nullptr)));
    ID3D11VertexShader *vertex=nullptr; ID3D11PixelShader *pixel=nullptr;
    CHECK(SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex)));
    CHECK(SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel)));
    vs->Release(); ps->Release(); context->VSSetShader(vertex,nullptr,0); context->PSSetShader(pixel,nullptr,0);
    D3D11_RASTERIZER_DESC raster = {}; raster.FillMode=D3D11_FILL_SOLID; raster.CullMode=D3D11_CULL_NONE; raster.DepthClipEnable=TRUE;
    ID3D11RasterizerState *rasterizer=nullptr; CHECK(SUCCEEDED(device->CreateRasterizerState(&raster,&rasterizer)));
    context->RSSetState(rasterizer); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_VIEWPORT viewport={0,0,512,512,0,1}; context->RSSetViewports(1,&viewport);
    ID3D11Fence *fence=nullptr; CHECK(SUCCEEDED(device5->CreateFence(0,D3D11_FENCE_FLAG_NONE,__uuidof(ID3D11Fence),(void **)&fence)));
    foveation_probe_params control={}; CHECK(SUCCEEDED(native->GetMetalDevice(&control.device)));
    CHECK(probe(FOVEATION_CREATE,&control)==0 && control.status==0);
    ID3D11Texture2D *control_texture=nullptr;
    D3D11_TEXTURE2D_DESC td={}; td.Width=td.Height=512; td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    CHECK(SUCCEEDED(native->ImportMetalTexture(control.texture,&td,&control_texture)));
    ID3D11RenderTargetView *control_rtv=nullptr; CHECK(SUCCEEDED(device->CreateRenderTargetView(control_texture,nullptr,&control_rtv)));
    for (unsigned frame=0;frame<6;++frame) {
        XrFrameWaitInfo wi={XR_TYPE_FRAME_WAIT_INFO}; XrFrameState fs={XR_TYPE_FRAME_STATE}; XR(xrWaitFrame(session,&wi,&fs));
        XrFrameBeginInfo bi={XR_TYPE_FRAME_BEGIN_INFO}; XR(xrBeginFrame(session,&bi));
        uint32_t indices[2]; foveation_probe_params expected[2]={}; XrFoveationD3D11StateMWXR states[2];
        for (unsigned eye=0;eye<2;++eye) {
            XrSwapchainStateFoveationFB update={XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB};
            update.profile=profiles[frame==2 ? 2 : (frame+eye)%2];
            if (frame) XR(xrUpdateSwapchainFB(swaps[eye],(XrSwapchainStateBaseHeaderFB *)&update));
            XrSwapchainImageAcquireInfo ai={XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO}; XR(xrAcquireSwapchainImage(swaps[eye],&ai,&indices[eye]));
            XrSwapchainImageWaitInfo wait={XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wait.timeout=5000000000LL;
            XR(xrWaitSwapchainImage(swaps[eye],&wait));
            XrSwapchainImageReleaseInfo early={XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            CHECK(xrReleaseSwapchainImage(swaps[eye],&early)==XR_ERROR_CALL_ORDER_INVALID);
        }
        // Both eyes are acquired first, then maps selected before either draw.
        for (unsigned eye=0;eye<2;++eye) {
            states[eye]={XR_TYPE_FOVEATION_D3D11_STATE_MWXR};
            XR(xrApplyFoveationD3D11MWXR(swaps[eye],eye,&states[eye]));
            XrFoveationD3D11StateMWXR duplicate={XR_TYPE_FOVEATION_D3D11_STATE_MWXR};
            CHECK(xrApplyFoveationD3D11MWXR(swaps[eye],eye,&duplicate)==XR_ERROR_CALL_ORDER_INVALID);
            CHECK(states[eye].foveationEnabled==(frame!=2));
            XR(image_object(swaps[eye],indices[eye],&expected[eye].device,&expected[eye].texture));
            expected[eye].map=image_map(swaps[eye]);
            CHECK(probe(FOVEATION_DESCRIBE,&expected[eye])==0 && expected[eye].status==0);
        }
        for (unsigned eye=0;eye<2;++eye) {
            ID3D11RenderTargetView *rtv=nullptr;
            CHECK(SUCCEEDED(device->CreateRenderTargetView(images[eye][indices[eye]].texture,nullptr,&rtv)));
            context->OMSetRenderTargets(1,&rtv,nullptr); context->Draw(3,0); rtv->Release();
            context->OMSetRenderTargets(1,&control_rtv,nullptr); context->Draw(3,0);
        }
        CHECK(SUCCEEDED(context4->Signal(fence,frame+1))); context->Flush();
        for (unsigned i=0;i<5000 && fence->GetCompletedValue()<frame+1;++i) Sleep(1);
        CHECK(fence->GetCompletedValue()>=frame+1);
        for (unsigned eye=0;eye<2;++eye) {
            CHECK(probe(FOVEATION_VERIFY,&expected[eye])==0 && expected[eye].status==0);
            std::printf("PASS XR frame=%u eye=%u image=%u enabled=%u revision=%u physical=%ux%u samples=%u\n",
                frame,eye,indices[eye],states[eye].foveationEnabled,states[eye].revision,
                expected[eye].physical_width,expected[eye].physical_height,expected[eye].checked);
            XrSwapchainImageReleaseInfo release={XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; XR(xrReleaseSwapchainImage(swaps[eye],&release));
        }
        CHECK(probe(FOVEATION_VERIFY,&control)==0 && control.status==0);
        XrViewLocateInfo locate={XR_TYPE_VIEW_LOCATE_INFO}; locate.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime=fs.predictedDisplayTime; locate.space=space;
        XrViewState viewstate={XR_TYPE_VIEW_STATE}; XrView views[2]={{XR_TYPE_VIEW},{XR_TYPE_VIEW}}; uint32_t count=0;
        XR(xrLocateViews(session,&locate,&viewstate,2,&count,views)); CHECK(count==2);
        XrCompositionLayerProjectionView pv[2]={{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        for (unsigned eye=0;eye<2;++eye) {
            pv[eye].pose=views[eye].pose; pv[eye].fov=views[eye].fov; pv[eye].subImage.swapchain=swaps[eye];
            pv[eye].subImage.imageRect.extent={512,512};
        }
        XrCompositionLayerProjection projection={XR_TYPE_COMPOSITION_LAYER_PROJECTION}; projection.space=space;
        projection.viewCount=2; projection.views=pv;
        const XrCompositionLayerBaseHeader *layers[]={(XrCompositionLayerBaseHeader *)&projection};
        XrFrameEndInfo end={XR_TYPE_FRAME_END_INFO}; end.displayTime=fs.predictedDisplayTime;
        end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE; end.layerCount=1; end.layers=layers;
        XR(xrEndFrame(session,&end));
    }
    for (auto swap:swaps) XR(xrDestroySwapchain(swap));
    for (auto profile:profiles) XR(xrDestroyFoveationProfileFB(profile));
    XR(xrDestroySpace(space)); XR(xrDestroySession(session)); XR(xrDestroyInstance(instance));
    context->ClearState(); fence->Release(); control_rtv->Release(); control_texture->Release();
    CHECK(probe(FOVEATION_RELEASE,&control)==0);
    rasterizer->Release(); vertex->Release(); pixel->Release(); native->Release(); context4->Release(); device5->Release();
    context->Release(); device->Release(); FreeLibrary(helper); FreeLibrary(runtime);
    std::puts("PASS OpenXR stereo map changes, disable/re-enable, unrelated RT, and projection submission");
    return 0;
}

int main()
{
    for (unsigned cycle=0;cycle<2;++cycle) {
        std::printf("OpenXR lifecycle cycle %u\n",cycle);
        int result=run_session();
        if (result) return result;
    }
    std::puts("PASS OpenXR instance/session destruction and recreation");
    return 0;
}

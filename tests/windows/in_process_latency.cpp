// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <windows.h>
#include <d3d11_4.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cstdio>
#include <vector>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAILED line %d: %s\n",__LINE__,#x); return 1; } } while (0)
#define XR(x) do { XrResult check_result=(x); if (XR_FAILED(check_result)) { std::fprintf(stderr,"FAILED line %d: %s = %d\n",__LINE__,#x,check_result); return 1; } } while (0)
int main(int argc, char **argv)
{
    CHECK(argc==2);
    auto gipa=xrGetInstanceProcAddr;
    XrInstance instance=XR_NULL_HANDLE;
    PFN_xrCreateInstance xrCreateInstance=nullptr;
    XR(gipa(XR_NULL_HANDLE,"xrCreateInstance",(PFN_xrVoidFunction *)&xrCreateInstance));
    const char *extension=XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
    XrInstanceCreateInfo ci={XR_TYPE_INSTANCE_CREATE_INFO};
    ci.applicationInfo.apiVersion=XR_API_VERSION_1_0; ci.enabledExtensionCount=1; ci.enabledExtensionNames=&extension;
    strcpy(ci.applicationInfo.applicationName,"bridge-latency-probe");
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
    LOAD(xrCreateReferenceSpace); LOAD(xrDestroySpace); LOAD(xrLocateSpace);
    LOAD(xrCreateActionSet); LOAD(xrCreateAction); LOAD(xrAttachSessionActionSets); LOAD(xrSyncActions);
    LOAD(xrGetActionStateBoolean); LOAD(xrGetActionStateFloat); LOAD(xrGetActionStateVector2f); LOAD(xrGetActionStatePose);
    XrReferenceSpaceCreateInfo rsi={XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsi.poseInReferenceSpace.orientation.w=1; rsi.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL;
    XrSpace local,view; XR(xrCreateReferenceSpace(session,&rsi,&local));
    rsi.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_VIEW; XR(xrCreateReferenceSpace(session,&rsi,&view));
    XrActionSetCreateInfo asi={XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(asi.actionSetName,"measure"); strcpy(asi.localizedActionSetName,"Measure");
    XrActionSet set; XR(xrCreateActionSet(instance,&asi,&set));
    XrAction actions[4]; XrActionType types[]={XR_ACTION_TYPE_BOOLEAN_INPUT,XR_ACTION_TYPE_FLOAT_INPUT,
        XR_ACTION_TYPE_VECTOR2F_INPUT,XR_ACTION_TYPE_POSE_INPUT};
    for (unsigned i=0;i<4;++i) {
        XrActionCreateInfo ai={XR_TYPE_ACTION_CREATE_INFO}; ai.actionType=types[i];
        sprintf(ai.actionName,"action%u",i); sprintf(ai.localizedActionName,"Action %u",i);
        XR(xrCreateAction(set,&ai,&actions[i]));
    }
    XrSessionActionSetsAttachInfo attach={XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO}; attach.countActionSets=1; attach.actionSets=&set;
    XR(xrAttachSessionActionSets(session,&attach));
    LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
    struct Sample { unsigned frame; const char *operation; long long start,duration; XrResult result; XrTime display; };
    std::vector<Sample> samples; samples.reserve(200*82);
    auto ticks=[](){ LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; };
    XrTime display=0;
#define TIME(operation, expression) do { auto start=ticks(); XrResult r=(expression); auto stop=ticks(); \
        samples.push_back({frame,operation,start,(long long)((stop-start)*1e9/frequency.QuadPart),r,display}); XR(r); } while(0)
    for (unsigned frame=0;frame<200;++frame) {
        XrFrameWaitInfo wi={XR_TYPE_FRAME_WAIT_INFO}; XrFrameState fs={XR_TYPE_FRAME_STATE};
        TIME("xrWaitFrame",xrWaitFrame(session,&wi,&fs)); display=fs.predictedDisplayTime; samples.back().display=display;
        XrFrameBeginInfo bi={XR_TYPE_FRAME_BEGIN_INFO}; XR(xrBeginFrame(session,&bi));
        XrActiveActionSet active={set,XR_NULL_PATH}; XrActionsSyncInfo sync={XR_TYPE_ACTIONS_SYNC_INFO};
        sync.countActiveActionSets=1; sync.activeActionSets=&active; XR(xrSyncActions(session,&sync));
        for (unsigned repetition=0;repetition<20;++repetition) {
            XrSpaceLocation location={XR_TYPE_SPACE_LOCATION}; TIME("xrLocateSpace",xrLocateSpace(view,local,display,&location));
            XrActionStateGetInfo get={XR_TYPE_ACTION_STATE_GET_INFO}; get.action=actions[0];
            XrActionStateBoolean boolean={XR_TYPE_ACTION_STATE_BOOLEAN}; TIME("xrGetActionStateBoolean",xrGetActionStateBoolean(session,&get,&boolean));
            get.action=actions[1]; XrActionStateFloat scalar={XR_TYPE_ACTION_STATE_FLOAT}; TIME("xrGetActionStateFloat",xrGetActionStateFloat(session,&get,&scalar));
            get.action=actions[2]; XrActionStateVector2f vector={XR_TYPE_ACTION_STATE_VECTOR2F}; TIME("xrGetActionStateVector2f",xrGetActionStateVector2f(session,&get,&vector));
            get.action=actions[3]; XrActionStatePose pose={XR_TYPE_ACTION_STATE_POSE}; TIME("xrGetActionStatePose",xrGetActionStatePose(session,&get,&pose));
        }
        XrFrameEndInfo end={XR_TYPE_FRAME_END_INFO}; end.displayTime=display; end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        TIME("xrEndFrame",xrEndFrame(session,&end));
    }
    FILE *csv=fopen(argv[1],"w"); CHECK(csv);
    fprintf(csv,"frame,operation,start_ticks,duration_ns,result,predicted_display_ns,qpc_frequency\n");
    for (const auto &s:samples) fprintf(csv,"%u,%s,%lld,%lld,%d,%lld,%lld\n",s.frame,s.operation,s.start,s.duration,s.result,(long long)s.display,frequency.QuadPart);
    fclose(csv);
    XR(xrDestroySpace(view)); XR(xrDestroySpace(local)); XR(xrDestroySession(session)); XR(xrDestroyInstance(instance));
    context->Release(); device->Release();
    puts("PASS latency probe: 200 empty frames; inactive unbound action state reads");
    return 0;
}

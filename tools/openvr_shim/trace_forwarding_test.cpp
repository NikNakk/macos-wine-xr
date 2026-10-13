// Copyright 2026 Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "openvr_shim.cpp"
static int posesCalls=0, submitCalls=0, deviceCalls=0, failures=0;
static vr::Texture_t *expectedTexture;
static vr::VRTextureBounds_t *expectedBounds;
static vr::EVRCompositorError poses(void *,vr::TrackedDevicePose_t *render,uint32_t rc,vr::TrackedDevicePose_t *,uint32_t gc){
 if(rc!=1||gc!=0)++failures; ++posesCalls;Sleep(25);render[0].bPoseIsValid=true;return (vr::EVRCompositorError)7;
}
static vr::EVRCompositorError devicePose(void *,vr::TrackedDeviceIndex_t device,vr::TrackedDevicePose_t *render,vr::TrackedDevicePose_t *){
 if(device!=5)++failures;++deviceCalls;render->bPoseIsValid=true;return (vr::EVRCompositorError)7;
}
static vr::EVRCompositorError submit(void *,vr::EVREye eye,const vr::Texture_t *texture,const vr::VRTextureBounds_t *bounds,vr::EVRSubmitFlags flags){
 if(eye!=vr::Eye_Right||texture!=expectedTexture||bounds!=expectedBounds||flags!=vr::Submit_Default)++failures;
 ++submitCalls;Sleep(5);return (vr::EVRCompositorError)7;
}
int main(){
 _putenv("MWXR_OPENVR_SHIM_TRACE=1");_putenv("MWXR_OPENVR_SHIM_LOG=Z:\\tmp\\openvr-trace-forwarding.log");
 _putenv("MWXR_OPENVR_SHIM_FIX_POSE=0");
 void *table[8]={};struct{void **vtable;} self{table};
 g_patched[0]={table,poses,poses,devicePose,submit};g_patchedCount=1;
 vr::TrackedDevicePose_t render={};vr::Texture_t texture={(void*)0x1234,vr::TextureType_DirectX,vr::ColorSpace_Auto};vr::VRTextureBounds_t bounds={0,0,1,1};expectedTexture=&texture;expectedBounds=&bounds;
 if(WaitGetPosesHook(&self,&render,1,nullptr,0)!=(vr::EVRCompositorError)7||!render.bPoseIsValid)++failures;
 if(GetLastPosesHook(&self,&render,1,nullptr,0)!=(vr::EVRCompositorError)7)++failures;
 if(GetLastPoseForDeviceHook(&self,5,&render,nullptr)!=(vr::EVRCompositorError)7)++failures;
 if(SubmitHook(&self,vr::Eye_Right,&texture,&bounds,vr::Submit_Default)!=(vr::EVRCompositorError)7)++failures;
 Sleep(1100);WaitGetPosesHook(&self,&render,1,nullptr,0);
 if(posesCalls!=3||submitCalls!=1||deviceCalls!=1)++failures;
 printf("%s OpenVR trace forwarding and pose/error preservation: failures=%d\n",failures?"FAIL":"PASS",failures);return failures?1:0;
}

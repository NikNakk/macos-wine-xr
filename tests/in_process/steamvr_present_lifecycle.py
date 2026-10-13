# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
from pathlib import Path
import argparse, subprocess, tempfile
parser = argparse.ArgumentParser(description="Compile the production Present body against lifecycle mocks")
parser.add_argument("--openxr-includes", type=Path, required=True)
args = parser.parse_args()
p = Path(__file__).resolve().parents[2] / "src/steamvr_driver/xr_backend.cpp"
s=p.read_text(); body=s[s.index('void\nXrBackend::Present('):s.index('\nvoid\nXrBackend::PresentDisplayImage')]
header=r'''
#include <openxr/openxr.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <functional>
#include <mutex>
#include <set>
#include <vector>
#include <cstdio>
using LONGLONG = long long;
struct LARGE_INTEGER { long long QuadPart; };
void QueryPerformanceCounter(LARGE_INTEGER *v) { v->QuadPart=0; }
void QueryPerformanceFrequency(LARGE_INTEGER *v) { v->QuadPart=1; }
template<class... T> void Log(const char*, T...) {}
struct D3D11_TEXTURE2D_DESC { unsigned Width=32,Height=32; int Format=1; };
struct D3D11_BOX { unsigned left=0,top=0,front=0,right=32,bottom=32,back=1; };
struct ID3D11Texture2D { void GetDesc(D3D11_TEXTURE2D_DESC *d) { *d={}; } };
static bool owned=false;
static int copies=0, waits=0, released=0, zeroReleased=0, preparedCount=0;
static bool failSecond=false;
static XrSwapchain handle(int i) { return reinterpret_cast<XrSwapchain>(uintptr_t(i)); }
XrResult xrAcquireSwapchainImage(XrSwapchain s, const XrSwapchainImageAcquireInfo*, uint32_t *i) {
 assert(!owned); *i=0; if(failSecond && s==handle(2))return XR_ERROR_RUNTIME_FAILURE; return XR_SUCCESS;
}
XrResult xrWaitSwapchainImage(XrSwapchain,const XrSwapchainImageWaitInfo*) { assert(!owned); ++waits; return XR_SUCCESS; }
XrResult xrReleaseSwapchainImage(XrSwapchain s,const XrSwapchainImageReleaseInfo*) {
 ++released; if(s==handle(99)) { assert(owned); ++zeroReleased; } return XR_SUCCESS;
}
struct Context {
 void CopySubresourceRegion(ID3D11Texture2D*,int,int,int,int,ID3D11Texture2D*,int,const D3D11_BOX*) {
  assert(owned); ++copies;
 }
};
struct EyeSubmit { ID3D11Texture2D *texture; XrSwapchain swapchain; D3D11_BOX box; XrPosef pose{}; XrFovf fov{}; };
struct LayerSubmit { EyeSubmit eye[2]; };
class XrBackend {
public:
 struct Image { ID3D11Texture2D *texture; };
 struct EyeSwapchain { XrSwapchain handle; std::vector<Image> images; };
 Context context; Context *context_=&context;
 ID3D11Texture2D dest;
 EyeSwapchain slots[4];
 bool running_=true,frameBegun_=true;
 XrFrameState frameState_{XR_TYPE_FRAME_STATE};
 std::mutex mutex_;
 XrSpace baseSpace_=XR_NULL_HANDLE;
 LONGLONG lastLayerLog_=0;
 int projected=-1;
 XrBackend() { frameState_.shouldRender=true; for(int i=0;i<4;++i)slots[i]={handle(i+1),{{&dest}}}; }
 bool WaitAndBeginFrame(double&,double&) { assert(!owned); frameBegun_=true; return true; }
 EyeSwapchain *SwapchainFor(size_t layer,int eye,unsigned,unsigned,int) { assert(!owned); ++preparedCount; return &slots[layer*2+eye]; }
 void EndFrameLocked(const std::vector<const XrCompositionLayerBaseHeader*> &headers) { assert(!owned); projected=headers.size(); }
 void Present(const std::vector<LayerSubmit>&,const std::function<void()>&,const std::function<bool()>&);
};
'''
tests=r'''
void reset(){owned=false;copies=waits=released=zeroReleased=preparedCount=0;failSecond=false;}
int main(){
 ID3D11Texture2D source;
 LayerSubmit layer{{{&source,XR_NULL_HANDLE,{}},{&source,XR_NULL_HANDLE,{}}}};
 auto release=[] { owned=false; };
 reset(); { XrBackend b; b.Present({layer},release,[]{ assert(waits==2); owned=true; return true; });
 assert(copies==2 && released==2 && b.projected==1); }
 reset(); { XrBackend b; b.Present({layer},release,[]{assert(waits==2);return false;});
 assert(copies==0 && released==2 && b.projected==0); }
 reset(); { failSecond=true; XrBackend b; b.Present({layer},release,[]{owned=true;return true;});
 assert(released==1 && copies==1 && b.projected==0); }
 layer.eye[0].swapchain=layer.eye[1].swapchain=handle(99);
 reset(); { XrBackend b; b.Present({layer},release,[]{owned=true;return true;});
 assert(copies==0 && zeroReleased==1 && b.projected==1); }
 reset(); { XrBackend b; b.Present({layer},release,[]{return false;});
 assert(copies==0 && zeroReleased==0 && b.projected==0); }
 reset(); { XrBackend b; b.running_=false; b.Present({layer},release,[]{assert(false);return false;});
 assert(copies==0 && released==0); }
 puts("PASS: destination waits before source ownership; balanced image releases; failed acquire reads nothing; zero-copy release follows ownership; stopped session skips acquisition (6 cases)");
}
'''
with tempfile.TemporaryDirectory(prefix="mwxr-present-lifecycle-") as directory:
    source = Path(directory) / "test.cpp"
    binary = Path(directory) / "test"
    source.write_text(header + body + tests)
    subprocess.run(["clang++", "-std=c++17", "-I", str(args.openxr_includes), str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <vector>
#include <cstring>
#include <cmath>
#include <string>
#include "nvngx.hpp"
#include "d3d11_interfaces.hpp"
#define CHECK(x) do { if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;} }while(0)
using Result=unsigned;
using Cap=Result(*)(NVNGXParameter**);
using Create=Result(*)(ID3D11DeviceContext*,unsigned,NVNGXParameter*,unsigned**);
using Evaluate=Result(*)(ID3D11DeviceContext*,const unsigned*,NVNGXParameter*,void*);
using Release=Result(*)(unsigned*);
using Destroy=Result(*)(NVNGXParameter*);
struct ParameterProxy final : NVNGXParameter {
 NVNGXParameter *real;
 explicit ParameterProxy(NVNGXParameter *p):real(p){}
#define SET(T) void Set(const char*n,T v) override {real->Set(n,v);}
 SET(void*) SET(ID3D12Resource*) SET(ID3D11Resource*) SET(int) SET(unsigned) SET(double) SET(float) SET(unsigned long long)
#undef SET
#define GET(T) NVNGX_RESULT Get(const char*n,T *v) const override {return real->Get(n,v);}
 GET(void*) GET(ID3D12Resource*) GET(ID3D11Resource*) GET(int) GET(unsigned) GET(double) GET(float) GET(unsigned long long)
#undef GET
 void Reset() override {real->Reset();}
};
struct HalfPixel { unsigned short r,g,b,a; };
static_assert(sizeof(HalfPixel)==8);

static int check_depth_copy(ID3D11Device *device,ID3D11DeviceContext *ctx) {
 IMTLD3D11ContextExt2 *roi=nullptr;CHECK(SUCCEEDED(ctx->QueryInterface(__uuidof(IMTLD3D11ContextExt2),(void**)&roi)));
 D3D11_TEXTURE2D_DESC td={};td.Width=td.Height=64;td.ArraySize=td.MipLevels=1;td.SampleDesc.Count=1;
 td.Format=DXGI_FORMAT_R32G8X24_TYPELESS;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
 ID3D11Texture2D *src=nullptr,*dst=nullptr;CHECK(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&src)));CHECK(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&dst)));
 D3D11_DEPTH_STENCIL_VIEW_DESC dd={};dd.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;dd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
 ID3D11DepthStencilView *sv=nullptr,*dv=nullptr;CHECK(SUCCEEDED(device->CreateDepthStencilView(src,&dd,&sv)));CHECK(SUCCEEDED(device->CreateDepthStencilView(dst,&dd,&dv)));
 ctx->ClearDepthStencilView(sv,D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.375f,17);
 ctx->ClearDepthStencilView(dv,D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.875f,91);
 CHECK(roi->CopyTemporalRegion(src,16,16,dst,8,8,32,32)==S_OK);
 CHECK(roi->CopyTemporalRegion(src,0xffffffffu,0,dst,0,0,32,32)==E_INVALIDARG);
 for(unsigned plane=0;plane<2;++plane) {
  const char *type=plane?"uint":"float";
  std::string text=std::string("Texture2D<")+(plane?"uint4":"float")+"> dep:register(t0);RWTexture2D<"+type+"> outp:register(u0);"
    "[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){outp[p.xy]=dep.Load(int3(p.xy,0))"+(plane?".g":"")+";}";
  ID3DBlob *code=nullptr,*error=nullptr;auto hr=D3DCompile(text.data(),text.size(),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&code,&error);
  if(error){std::fprintf(stderr,"%s",(const char*)error->GetBufferPointer());error->Release();}CHECK(SUCCEEDED(hr));
  ID3D11ComputeShader *cs=nullptr;CHECK(SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&cs)));code->Release();
  D3D11_SHADER_RESOURCE_VIEW_DESC sd={};sd.Format=plane?DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
  sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
  ID3D11ShaderResourceView *srv=nullptr;CHECK(SUCCEEDED(device->CreateShaderResourceView(dst,&sd,&srv)));
  auto od=td;od.Format=plane?DXGI_FORMAT_R32_UINT:DXGI_FORMAT_R32_FLOAT;od.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
  ID3D11Texture2D *out=nullptr,*read=nullptr;CHECK(SUCCEEDED(device->CreateTexture2D(&od,nullptr,&out)));
  od.Usage=D3D11_USAGE_STAGING;od.BindFlags=0;od.CPUAccessFlags=D3D11_CPU_ACCESS_READ;CHECK(SUCCEEDED(device->CreateTexture2D(&od,nullptr,&read)));
  ID3D11UnorderedAccessView *uav=nullptr;CHECK(SUCCEEDED(device->CreateUnorderedAccessView(out,nullptr,&uav)));
  ctx->CSSetShader(cs,nullptr,0);ctx->CSSetShaderResources(0,1,&srv);ctx->CSSetUnorderedAccessViews(0,1,&uav,nullptr);ctx->Dispatch(8,8,1);
  ID3D11ShaderResourceView *no_srv=nullptr;ID3D11UnorderedAccessView *no_uav=nullptr;
  ctx->CSSetShaderResources(0,1,&no_srv);ctx->CSSetUnorderedAccessViews(0,1,&no_uav,nullptr);
  ctx->CopyResource(read,out);D3D11_MAPPED_SUBRESOURCE map={};CHECK(SUCCEEDED(ctx->Map(read,0,D3D11_MAP_READ,0,&map)));
  unsigned wrong=0;
  for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){bool inner=x>=8&&x<40&&y>=8&&y<40;
   auto p=(char*)map.pData+y*map.RowPitch+x*4;
   if(plane){if(*(unsigned*)p!=(inner?17u:91u))++wrong;}
   else if(std::fabs(*(float*)p-(inner?.375f:.875f))>1e-6f)++wrong;
  }
  ctx->Unmap(read,0);if(wrong)std::fprintf(stderr,"depth/stencil plane=%u wrong=%u\n",plane,wrong);CHECK(!wrong);
  read->Release();out->Release();uav->Release();srv->Release();cs->Release();
 }
 ctx->ClearState();sv->Release();dv->Release();src->Release();dst->Release();roi->Release();
 std::puts("PASS packed depth/stencil partial GPU copy and ordinary shader reads");return 0;
}
static float half_float(unsigned h) {
 int sign=(h&0x8000)?-1:1;unsigned e=(h>>10)&31,m=h&1023;
 return sign*std::ldexp(e?1.0f+m/1024.0f:m/1024.0f,e?int(e)-15:-14);
}

int main() {
 ID3D11Device *device=nullptr; ID3D11DeviceContext *ctx=nullptr;
 CHECK(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx)));
 CHECK(check_depth_copy(device,ctx)==0);
 HMODULE ngx=LoadLibraryA("nvngx.dll"); CHECK(ngx);
 auto cap=(Cap)GetProcAddress(ngx,"NVSDK_NGX_D3D11_GetCapabilityParameters");
 auto create=(Create)GetProcAddress(ngx,"NVSDK_NGX_D3D11_CreateFeature");
 auto evaluate=(Evaluate)GetProcAddress(ngx,"NVSDK_NGX_D3D11_EvaluateFeature");
 auto release=(Release)GetProcAddress(ngx,"NVSDK_NGX_D3D11_ReleaseFeature");
 auto destroy=(Destroy)GetProcAddress(ngx,"NVSDK_NGX_D3D11_DestroyParameters"); CHECK(cap&&create&&evaluate&&release&&destroy);
 NVNGXParameter *p=nullptr; CHECK(cap(&p)==1 && p);
 p->Set("Width",64u); p->Set("Height",64u); p->Set("OutWidth",128u); p->Set("OutHeight",128u);
 p->Set("PerfQualityValue",2); p->Set("DLSS.Feature.Create.Flags",2); p->Set("DLSS.Enable.Output.Subrects",1);
 unsigned *handle=nullptr; CHECK(create(ctx,1,p,&handle)==1 && handle);
 auto texture=[&](UINT w,UINT h,DXGI_FORMAT format,UINT bind,const void *data,UINT pitch) {
  D3D11_TEXTURE2D_DESC d={}; d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;d.Format=format;
  d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=bind;
  ID3D11Texture2D *t=nullptr;D3D11_SUBRESOURCE_DATA init={data,pitch,0};
  return SUCCEEDED(device->CreateTexture2D(&d,data?&init:nullptr,&t))?t:nullptr;
 };
 std::vector<HalfPixel> color(160*96,{0,0,0x3c00,0x3c00});
 for(unsigned y=16;y<80;++y)for(unsigned x=8;x<72;++x)color[y*160+x]={0x3c00,0,0,0x3c00};
 for(unsigned y=16;y<80;++y)for(unsigned x=88;x<152;++x)color[y*160+x]={0,0x3c00,0,0x3c00};
 std::vector<float> depth(160*96,.5f), motion(160*96*2,0);
 ID3D11Texture2D *c=texture(160,96,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET,color.data(),160*8);
 ID3D11Texture2D *d=texture(160,96,DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE,depth.data(),160*4);
 ID3D11Texture2D *mv=texture(160,96,DXGI_FORMAT_R32G32_FLOAT,D3D11_BIND_SHADER_RESOURCE,motion.data(),160*8);
 float one=1; ID3D11Texture2D *exposure=texture(1,1,DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE,&one,4);
 ID3D11Texture2D *out=texture(160,144,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET|D3D11_BIND_UNORDERED_ACCESS,nullptr,0);
 CHECK(c&&d&&mv&&exposure&&out);
 ID3D11RenderTargetView *out_rtv=nullptr;CHECK(SUCCEEDED(device->CreateRenderTargetView(out,nullptr,&out_rtv)));
 D3D11_TEXTURE2D_DESC read_desc={};out->GetDesc(&read_desc);read_desc.Usage=D3D11_USAGE_STAGING;read_desc.BindFlags=0;read_desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
 ID3D11Texture2D *read=nullptr; CHECK(SUCCEEDED(device->CreateTexture2D(&read_desc,nullptr,&read)));
 p->Set("Color",(ID3D11Resource*)c);p->Set("Depth",(ID3D11Resource*)d);p->Set("MotionVectors",(ID3D11Resource*)mv);
 p->Set("Output",(ID3D11Resource*)out);p->Set("ExposureTexture",(ID3D11Resource*)exposure);
 p->Set("DLSS.Render.Subrect.Dimensions.Width",64u);p->Set("DLSS.Render.Subrect.Dimensions.Height",64u);
 p->Set("Reset",1);p->Set("DLSS.Pre.Exposure",1.0f);p->Set("MV.Scale.X",1.0f);p->Set("MV.Scale.Y",1.0f);
 p->Set("DLSS.Output.Subrect.Base.X",16u);p->Set("DLSS.Output.Subrect.Base.Y",8u);
 const float blue[]={0,0,1,1};
 auto verify=[&](bool green,bool untouched) {
  ctx->CopyResource(read,out);D3D11_MAPPED_SUBRESOURCE map={};if(FAILED(ctx->Map(read,0,D3D11_MAP_READ,0,&map)))return false;
  unsigned wrong=0;
  for(unsigned y=0;y<144;++y)for(unsigned x=0;x<160;++x){
   auto a=((HalfPixel*)((char*)map.pData+y*map.RowPitch))[x];
   bool inside=x>=16&&x<144&&y>=8&&y<136;
   if(!inside||untouched){if(a.r!=0||a.g!=0||a.b!=0x3c00||a.a!=0x3c00)++wrong;}
   else if(x>=24&&x<136&&y>=16&&y<128) {
    // Uniform primary colours tolerate temporal reconstruction rounding;
    // padding outside the declared output ROI must remain bit exact.
    float high=half_float(green?a.g:a.r), low=half_float(green?a.r:a.g);
    if(high<.8f||high>1.2f||std::fabs(low)>.1f||std::fabs(half_float(a.b))>.1f)++wrong;
   }
  }
  ctx->Unmap(read,0);if(wrong)std::fprintf(stderr,"wrong pixels=%u green=%u untouched=%u\n",wrong,green,untouched);return !wrong;
 };
 ParameterProxy proxy(p);
 for(unsigned pass=0;pass<4;++pass){
  unsigned x=(pass&1)?88:8;
  p->Set("DLSS.Input.Color.Subrect.Base.X",x);p->Set("DLSS.Input.Color.Subrect.Base.Y",16u);
  p->Set("DLSS.Input.Depth.Subrect.Base.X",x);p->Set("DLSS.Input.Depth.Subrect.Base.Y",16u);
  p->Set("DLSS.Input.MV.Subrect.Base.X",x);p->Set("DLSS.Input.MV.Subrect.Base.Y",16u);
  ctx->ClearRenderTargetView(out_rtv,blue); CHECK(evaluate(ctx,handle,pass==3?&proxy:p,nullptr)==1);CHECK(verify(pass&1,false));
  std::printf("PASS ROI evaluation=%u eye=%s padded-output preserved\n",pass,(pass&1)?"right":"left");
 }
 p->Set("DLSS.Input.Color.Subrect.Base.X",0xffffffffu);ctx->ClearRenderTargetView(out_rtv,blue);
 CHECK(evaluate(ctx,handle,p,nullptr)==0x0bad0005u);CHECK(verify(false,true));
 p->Set("DLSS.Input.Color.Subrect.Base.X",8u);p->Set("DLSS.Output.Subrect.Base.X",64u);
 CHECK(evaluate(ctx,handle,p,nullptr)==0x0bad0005u);CHECK(verify(false,true));
 std::puts("PASS invalid origins rejected without output writes");
 CHECK(release(handle)==1);
 p->Set("DLSS.Output.Subrect.Base.X",16u);p->Set("DLSS.Input.Color.Subrect.Base.X",8u);
 p->Set("DLSS.Feature.Create.Flags",0);
 CHECK(create(ctx,1,p,&handle)==1);
 std::vector<float> high_motion(160*144*2,0);
 ID3D11Texture2D *high_mv=texture(160,144,DXGI_FORMAT_R32G32_FLOAT,D3D11_BIND_SHADER_RESOURCE,high_motion.data(),160*8);CHECK(high_mv);
 p->Set("MotionVectors",(ID3D11Resource*)high_mv);
 p->Set("DLSS.Input.MV.Subrect.Base.X",16u);p->Set("DLSS.Input.MV.Subrect.Base.Y",8u);
 ctx->ClearRenderTargetView(out_rtv,blue);CHECK(evaluate(ctx,handle,&proxy,nullptr)==1);CHECK(verify(false,false));
 CHECK(release(handle)==1);high_mv->Release();
 std::puts("PASS display-resolution motion ROI and foreign parameter proxy");
 p->Set("MotionVectors",(ID3D11Resource*)mv);p->Set("DLSS.Feature.Create.Flags",2);
 p->Set("DLSS.Input.MV.Subrect.Base.X",8u);p->Set("DLSS.Input.MV.Subrect.Base.Y",16u);
 p->Set("DLSS.Enable.Output.Subrects",0);CHECK(create(ctx,1,p,&handle)==1);
 ctx->ClearRenderTargetView(out_rtv,blue);CHECK(evaluate(ctx,handle,p,nullptr)==0x0bad0005u);CHECK(verify(false,true));
 CHECK(release(handle)==1);CHECK(destroy(p)==1);
 read->Release();out_rtv->Release();out->Release();exposure->Release();mv->Release();d->Release();c->Release();
 ctx->ClearState();ctx->Flush();ctx->Release();device->Release();FreeLibrary(ngx);return 0;
}

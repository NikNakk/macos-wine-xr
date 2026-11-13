// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Diagnostic explicit-map experiment, not an automatic application profile.
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>
#include "dxmt_native_interop.h"
#include "../in_process/depth_foveation_probe.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
template<class T>struct Ptr{T *p=nullptr;~Ptr(){if(p)p->Release();}Ptr()=default;Ptr(const Ptr&)=delete;T**out(){return &p;}T*operator->()const{return p;}operator T*()const{return p;}};
struct Pixel{float r,g,b,d;};
struct Target{Ptr<ID3D11Texture2D> color,depth;Ptr<ID3D11RenderTargetView> rtv;Ptr<ID3D11DepthStencilView>dsv;Ptr<ID3D11ShaderResourceView> csrv,dsrv;};
static ID3DBlob* compile(const char *source,const char *entry,const char *profile,const D3D_SHADER_MACRO *macros=nullptr){ID3DBlob *code=nullptr,*error=nullptr;
 HRESULT hr=D3DCompile(source,strlen(source),nullptr,macros,nullptr,entry,profile,0,0,&code,&error);
 if(error){fprintf(stderr,"%s",(char*)error->GetBufferPointer());error->Release();}CHECK(SUCCEEDED(hr));return code;}
static void wait(ID3D11DeviceContext *ctx,ID3D11Query *q,void *data,unsigned bytes){HRESULT hr=S_FALSE;ULONGLONG deadline=GetTickCount64()+30000;
 while((hr=ctx->GetData(q,data,bytes,0))==S_FALSE && GetTickCount64()<deadline)Sleep(1);CHECK(hr==S_OK);}
static void import(ID3D11Device *dev,IDXMTNativeDevice2 *native,Target &t,uint64_t color,uint64_t depth,unsigned size,unsigned samples){
 D3D11_TEXTURE2D_DESC desc={};desc.Width=desc.Height=size;desc.MipLevels=desc.ArraySize=1;desc.SampleDesc.Count=samples;
 desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
 CHECK(SUCCEEDED(native->ImportMetalTexture(color,&desc,t.color.out())));CHECK(SUCCEEDED(dev->CreateRenderTargetView(t.color,nullptr,t.rtv.out())));
 CHECK(SUCCEEDED(dev->CreateShaderResourceView(t.color,nullptr,t.csrv.out())));
 desc.Format=DXGI_FORMAT_R32_TYPELESS;desc.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
 CHECK(SUCCEEDED(native->ImportMetalTexture(depth,&desc,t.depth.out())));
 D3D11_DEPTH_STENCIL_VIEW_DESC dd={};dd.Format=DXGI_FORMAT_D32_FLOAT;dd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2DMS;
 CHECK(SUCCEEDED(dev->CreateDepthStencilView(t.depth,&dd,t.dsv.out())));
 D3D11_SHADER_RESOURCE_VIEW_DESC sd={};sd.Format=DXGI_FORMAT_R32_FLOAT;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DMS;
 CHECK(SUCCEEDED(dev->CreateShaderResourceView(t.depth,&sd,t.dsrv.out())));
}
static std::vector<Pixel> read_samples(ID3D11Device *dev,ID3D11DeviceContext *ctx,Target &t,unsigned size,unsigned samples){
 char count[8];snprintf(count,sizeof(count),"%u",samples);D3D_SHADER_MACRO macros[]={{"N",count},{nullptr,nullptr}};
 Ptr<ID3DBlob> code;code.p=compile("Texture2DMS<float4,N> c:register(t0);Texture2DMS<float,N> d:register(t1);RWTexture2D<float4> outp:register(u0);"
  "[numthreads(8,8,1)]void main(uint3 i:SV_DispatchThreadID){uint w,h,n;c.GetDimensions(w,h,n);if(i.x>=w||i.y>=h)return;"
  "[unroll]for(uint s=0;s<N;++s)outp[uint2(i.x*N+s,i.y)]=float4(c.Load(i.xy,s).rgb,d.Load(i.xy,s));}","main","cs_5_0",macros);
 Ptr<ID3D11ComputeShader> shader;CHECK(SUCCEEDED(dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,shader.out())));
 D3D11_TEXTURE2D_DESC td={};td.Width=size*samples;td.Height=size;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
 td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
 Ptr<ID3D11Texture2D> output,read;CHECK(SUCCEEDED(dev->CreateTexture2D(&td,nullptr,output.out())));
 Ptr<ID3D11UnorderedAccessView> uav;CHECK(SUCCEEDED(dev->CreateUnorderedAccessView(output,nullptr,uav.out())));
 td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
 CHECK(SUCCEEDED(dev->CreateTexture2D(&td,nullptr,read.out())));
 ID3D11ShaderResourceView *srvs[]={t.csrv,t.dsrv};ID3D11UnorderedAccessView *views[]={uav};
 ctx->OMSetRenderTargets(0,nullptr,nullptr);ctx->CSSetShader(shader,nullptr,0);ctx->CSSetShaderResources(0,2,srvs);ctx->CSSetUnorderedAccessViews(0,1,views,nullptr);
 ctx->Dispatch((size+7)/8,(size+7)/8,1);ID3D11ShaderResourceView *nulls[2]={};ID3D11UnorderedAccessView *none=nullptr;
 ctx->CSSetShaderResources(0,2,nulls);ctx->CSSetUnorderedAccessViews(0,1,&none,nullptr);ctx->CopyResource(read,output);
 D3D11_MAPPED_SUBRESOURCE mapped={};CHECK(SUCCEEDED(ctx->Map(read,0,D3D11_MAP_READ,0,&mapped)));
 std::vector<Pixel> result(size*size*samples);
 for(unsigned y=0;y<size;++y)memcpy(result.data()+y*size*samples,(char*)mapped.pData+y*mapped.RowPitch,size*samples*sizeof(Pixel));
 ctx->Unmap(read,0);return result;
}
struct Vertex{float x,y,z,r,g,b,u,v;};
static std::vector<Vertex> scene(unsigned size){std::vector<Vertex> v;
 auto point=[&](float x,float y,float z,float r,float g,float b){return Vertex{2*x/size-1,1-2*y/size,z,r,g,b,x/size,y/size};};
 auto tri=[&](Vertex a,Vertex b,Vertex c){v.push_back(a);v.push_back(b);v.push_back(c);};
 auto rect=[&](float x,float y,float w,float h,float z0,float z1,float r,float g,float b){auto a=point(x,y,z0,r,g,b),bb=point(x+w,y,z1,r,g,b),c=point(x,y+h,z0,r,g,b),d=point(x+w,y+h,z1,r,g,b);tri(a,bb,c);tri(c,bb,d);};
 rect(0,0,size,size,.9,.9,.1,.15,.5);
 rect(.16*size,.12*size,.68*size,.76*size,.32,.68,.7,.3,.1);
 tri(point(.08*size,.18*size,.22,.15,.65,.2),point(.34*size,.83*size,.42,.15,.65,.2),point(.73*size,.22*size,.28,.15,.65,.2));
 for(unsigned i=0;i<7;++i){float width=.5f+.25f*i;rect((.045f+i*.017f)*size,.25f*size,width,.5f*size,.12,.12,.8,.15,.7);}
 rect(.5*size+.2f,.2*size,1.2f,.6*size,.1,.1,.8,.15,.7);
 return v;
}
static double draw_scene(ID3D11Device *dev,ID3D11DeviceContext *ctx,Target &target,unsigned size,bool depth_only=false){
 auto vertices=scene(size);D3D11_BUFFER_DESC bd={};bd.ByteWidth=vertices.size()*sizeof(Vertex);bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;D3D11_SUBRESOURCE_DATA init={vertices.data(),0,0};
 Ptr<ID3D11Buffer> buffer;CHECK(SUCCEEDED(dev->CreateBuffer(&bd,&init,buffer.out())));
 const char *hlsl="struct I{float3 p:POSITION;float3 c:COLOR;float2 uv:TEXCOORD;};struct V{float4 p:SV_Position;float3 c:COLOR;float2 uv:TEXCOORD;};"
  "V vs(I i){V o;o.p=float4(i.p,1);o.c=i.c;o.uv=i.uv;return o;}float4 ps(V i):SV_Target{float a=i.uv.x+i.uv.y;"
  "[loop]for(uint j=0;j<128;++j)a=sin(a+j*.01);return float4(i.c*(.65+.35*i.uv.x)+a*.000001,1);}";
 Ptr<ID3DBlob> vs,ps;vs.p=compile(hlsl,"vs","vs_5_0");ps.p=compile(hlsl,"ps","ps_5_0");
 Ptr<ID3D11VertexShader> vertex;Ptr<ID3D11PixelShader> pixel;CHECK(SUCCEEDED(dev->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,vertex.out())));
 CHECK(SUCCEEDED(dev->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,pixel.out())));
 D3D11_INPUT_ELEMENT_DESC elements[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"COLOR",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},{"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,24,D3D11_INPUT_PER_VERTEX_DATA,0}};
 Ptr<ID3D11InputLayout> layout;CHECK(SUCCEEDED(dev->CreateInputLayout(elements,3,vs->GetBufferPointer(),vs->GetBufferSize(),layout.out())));
 UINT stride=sizeof(Vertex),offset=0;ID3D11Buffer *buffers[]={buffer};ctx->IASetVertexBuffers(0,1,buffers,&stride,&offset);ctx->IASetInputLayout(layout);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 ctx->VSSetShader(vertex,nullptr,0);ctx->PSSetShader(depth_only?nullptr:pixel.p,nullptr,0);ctx->OMSetDepthStencilState(nullptr,0);
 ID3D11RenderTargetView *rtv=target.rtv;ctx->OMSetRenderTargets(depth_only?0:1,depth_only?nullptr:&rtv,target.dsv);
 Ptr<ID3D11Query> disjoint,begin[24],end[24];D3D11_QUERY_DESC qd={D3D11_QUERY_TIMESTAMP_DISJOINT,0};CHECK(SUCCEEDED(dev->CreateQuery(&qd,disjoint.out())));qd.Query=D3D11_QUERY_TIMESTAMP;
 for(unsigned i=0;i<24;++i){CHECK(SUCCEEDED(dev->CreateQuery(&qd,begin[i].out())));CHECK(SUCCEEDED(dev->CreateQuery(&qd,end[i].out())));}
 ctx->Begin(disjoint);for(unsigned i=0;i<24;++i){ctx->End(begin[i]);float clear[4]={0,0,0,1};if(!depth_only)ctx->ClearRenderTargetView(target.rtv,clear);ctx->ClearDepthStencilView(target.dsv,D3D11_CLEAR_DEPTH,1,0);ctx->Draw(vertices.size(),0);ctx->End(end[i]);}
 ctx->End(disjoint);ctx->OMSetRenderTargets(0,nullptr,nullptr);
 D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj={};wait(ctx,disjoint,&dj,sizeof(dj));CHECK(!dj.Disjoint && dj.Frequency);double times[21];
 for(unsigned i=0;i<24;++i){UINT64 a,b;wait(ctx,begin[i],&a,sizeof(a));wait(ctx,end[i],&b,sizeof(b));CHECK(b>a);if(i>=3)times[i-3]=(b-a)*1000./dj.Frequency;}
 std::sort(times,times+21);return times[10];
}
static std::vector<unsigned> overlay_resolve(ID3D11Device *dev,ID3D11DeviceContext *ctx,Target &target,unsigned size,double &resolve_ms){
 const char *hlsl="float4 vs(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,.5,1);}float4 ps():SV_Target{return float4(0,1,1,1);}";
 Ptr<ID3DBlob> vs,ps;vs.p=compile(hlsl,"vs","vs_5_0");ps.p=compile(hlsl,"ps","ps_5_0");Ptr<ID3D11VertexShader> vertex;Ptr<ID3D11PixelShader> pixel;
 CHECK(SUCCEEDED(dev->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,vertex.out())));CHECK(SUCCEEDED(dev->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,pixel.out())));
 ctx->IASetInputLayout(nullptr);ctx->VSSetShader(vertex,nullptr,0);ctx->PSSetShader(pixel,nullptr,0);
 D3D11_DEPTH_STENCIL_DESC dd={};dd.DepthEnable=TRUE;dd.DepthFunc=D3D11_COMPARISON_LESS;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
 Ptr<ID3D11DepthStencilState> ds;CHECK(SUCCEEDED(dev->CreateDepthStencilState(&dd,ds.out())));ctx->OMSetDepthStencilState(ds,0);
 ID3D11RenderTargetView *rtv=target.rtv;ctx->OMSetRenderTargets(1,&rtv,target.dsv);ctx->Draw(3,0);ctx->OMSetRenderTargets(0,nullptr,nullptr);
 D3D11_TEXTURE2D_DESC desc={};desc.Width=desc.Height=size;desc.ArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
 Ptr<ID3D11Texture2D> resolved,staging;CHECK(SUCCEEDED(dev->CreateTexture2D(&desc,nullptr,resolved.out())));
 Ptr<ID3D11Query> dj,begins[24],ends[24];D3D11_QUERY_DESC qd={D3D11_QUERY_TIMESTAMP_DISJOINT,0};CHECK(SUCCEEDED(dev->CreateQuery(&qd,dj.out())));qd.Query=D3D11_QUERY_TIMESTAMP;
 for(unsigned i=0;i<24;++i){CHECK(SUCCEEDED(dev->CreateQuery(&qd,begins[i].out())));CHECK(SUCCEEDED(dev->CreateQuery(&qd,ends[i].out())));}
 ctx->Begin(dj);for(unsigned i=0;i<24;++i){ctx->End(begins[i]);ctx->ResolveSubresource(resolved,0,target.color,0,desc.Format);ctx->End(ends[i]);}ctx->End(dj);
 desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;CHECK(SUCCEEDED(dev->CreateTexture2D(&desc,nullptr,staging.out())));ctx->CopyResource(staging,resolved);
 D3D11_MAPPED_SUBRESOURCE mapped={};CHECK(SUCCEEDED(ctx->Map(staging,0,D3D11_MAP_READ,0,&mapped)));std::vector<unsigned> result(size*size);
 for(unsigned y=0;y<size;++y)memcpy(result.data()+y*size,(char*)mapped.pData+y*mapped.RowPitch,size*4);ctx->Unmap(staging,0);
 D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data={};wait(ctx,dj,&data,sizeof(data));CHECK(!data.Disjoint && data.Frequency);double times[21];
 for(unsigned i=0;i<24;++i){UINT64 a,b;wait(ctx,begins[i],&a,sizeof(a));wait(ctx,ends[i],&b,sizeof(b));CHECK(b>a);if(i>=3)times[i-3]=(b-a)*1000./data.Frequency;}std::sort(times,times+21);resolve_ms=times[10];return result;
}
static void dump(const char *name,const std::vector<unsigned>&pixels,unsigned size,unsigned samples){
 char filename[128];snprintf(filename,sizeof(filename),"depth-%u-%u-%s.ppm",size,samples,name);
 FILE *file=fopen(filename,"wb");CHECK(file);fprintf(file,"P6\n%u %u\n255\n",size,size);
 for(auto p:pixels){unsigned char rgb[3]={(unsigned char)p,(unsigned char)(p>>8),(unsigned char)(p>>16)};CHECK(fwrite(rgb,1,3,file)==3);}CHECK(fclose(file)==0);
}
static void compare(const char *name,const std::vector<Pixel>&a,const std::vector<Pixel>&b,const std::vector<unsigned>&ar,const std::vector<unsigned>&br,unsigned size,unsigned samples,double scene_ms,double unwarp_ms,double resolve_ms,bool identity,double prepass_ms=0){
 unsigned depth_bad=0,color_bad=0,coverage=0,compare_bad=0,interior_bad=0,resolved_bad=0,plane_bad=0,center_bad=0,thin_missing=0,thin_added=0;double maximum=0;
 for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x)for(unsigned s=0;s<samples;++s){size_t i=(y*size+x)*samples+s;
  double delta=fabs(a[i].d-b[i].d);CHECK(std::isfinite(b[i].d));maximum=std::max(maximum,delta);bool bad=delta>1e-5;depth_bad+=bad;
  color_bad+=fabs(a[i].r-b[i].r)>3./255 || fabs(a[i].g-b[i].g)>3./255 || fabs(a[i].b-b[i].b)>3./255;
  coverage+=(a[i].d<.8)!=(b[i].d<.8);compare_bad+=(a[i].d>.5)!=(b[i].d>.5);
  if(x>.76*size && x<.80*size && y>.30*size && y<.70*size)plane_bad+=bad;
  if(x>.44*size && x<.56*size && y>.44*size && y<.56*size)center_bad+=bad;
  if(x<.18*size){thin_missing+=a[i].d<.15 && b[i].d>=.15;thin_added+=a[i].d>=.15 && b[i].d<.15;}
  if(bad && x>4 && y>4 && x+4<size && y+4<size){bool interior=true;
   for(int dy=-4;dy<=4;dy+=4)for(int dx=-4;dx<=4;dx+=4)if(fabs(a[((y+dy)*size+x+dx)*samples+s].d-a[i].d)>.02)interior=false;
   interior_bad+=interior;
  }
 }
 for(size_t i=0;i<ar.size();++i){bool bad=false;for(unsigned c=0;c<3;++c)bad|=abs(int((ar[i]>>(c*8))&255)-int((br[i]>>(c*8))&255))>3;resolved_bad+=bad;}
 printf("DEPTH_RESULT case=%s size=%u samples=%u checked_samples=%zu depth_abs_1e5=%u max_depth_error=%.9f interior_depth_bad=%u plane_bad=%u center_bad=%u thin_missing=%u thin_added=%u color_bad=%u coverage_bad=%u threshold_bad=%u resolved_pixels_bad=%u scene_gpu_ms=%.6f reconstruct_gpu_ms=%.6f resolve_gpu_ms=%.6f depth_prepass_gpu_ms=%.6f total_gpu_ms=%.6f\n",name,size,samples,a.size(),depth_bad,maximum,interior_bad,plane_bad,center_bad,thin_missing,thin_added,color_bad,coverage,compare_bad,resolved_bad,scene_ms,unwarp_ms,resolve_ms,prepass_ms,scene_ms+unwarp_ms+resolve_ms+prepass_ms);fflush(stdout);
 CHECK(!center_bad);
 if(strstr(name,"-full-depth"))CHECK(maximum==0 && !coverage && !compare_bad);
 if(identity)CHECK(!depth_bad && !color_bad && !coverage && !compare_bad && !resolved_bad);
}
int main(int argc,char **argv){unsigned samples=argc>1?atoi(argv[1]):2,size=argc>2?atoi(argv[2]):512;CHECK((samples==2||samples==4)&&size>=64&&size<=2048);
 HMODULE helper=LoadLibraryA("wine_depth_foveation_probe.dll");CHECK(helper);auto probe=(LONG(WINAPI*)(UINT,void*))GetProcAddress(helper,"MWXRMetalProbe");CHECK(probe);
 Ptr<ID3D11Device> dev;Ptr<ID3D11DeviceContext> ctx;CHECK(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,dev.out(),nullptr,ctx.out())));
 Ptr<IDXMTNativeDevice2> native;Ptr<IDXMTNativeContext1> rate;Ptr<ID3D11DeviceContext4> ctx4;
 CHECK(SUCCEEDED(dev->QueryInterface(DXMT_IID_NATIVE_DEVICE2,(void**)native.out())));CHECK(SUCCEEDED(ctx->QueryInterface(DXMT_IID_NATIVE_CONTEXT1,(void**)rate.out())));CHECK(SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext4),(void**)ctx4.out())));
 D3D11_VIEWPORT viewport={0,0,float(size),float(size),0,1};ctx->RSSetViewports(1,&viewport);D3D11_RASTERIZER_DESC rd={};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;rd.MultisampleEnable=TRUE;
 Ptr<ID3D11RasterizerState> raster;CHECK(SUCCEEDED(dev->CreateRasterizerState(&rd,raster.out())));ctx->RSSetState(raster);
 depth_foveation_params p[2]={};Target targets[2];double scene_ms[2]={};
 for(unsigned i=0;i<2;++i){CHECK(SUCCEEDED(native->GetMetalDevice(&p[i].device)));p[i].size=size;p[i].samples=samples;p[i].mapped=i;p[i].value=1;
  CHECK(probe(DEPTH_CREATE,&p[i])==0 && !p[i].status);import(dev,native,targets[i],p[i].color,p[i].depth,size,samples);
  CHECK(SUCCEEDED(rate->SetRasterizationRateMap(p[i].map)));scene_ms[i]=draw_scene(dev,ctx,targets[i],size);CHECK(SUCCEEDED(rate->SetRasterizationRateMap(0)));
  Ptr<ID3D11Fence> fence;CHECK(SUCCEEDED(native->ImportMetalSharedEvent(p[i].event,fence.out())));CHECK(SUCCEEDED(ctx4->Signal(fence,p[i].value)));ctx->Flush();
  printf("DEPTH_SCENE mapped=%u size=%u samples=%u physical=%ux%u scene_gpu_ms=%.6f\n",i,size,samples,p[i].physical_width,p[i].physical_height,scene_ms[i]);fflush(stdout);
 }
 auto reference=read_samples(dev,ctx,targets[0],size,samples);
 double prepass_ms=draw_scene(dev,ctx,targets[0],size,true);
 auto depth_replay=read_samples(dev,ctx,targets[0],size,samples);unsigned replay_wrong=0;
 for(size_t i=0;i<reference.size();++i)replay_wrong+=reference[i].d!=depth_replay[i].d;
 printf("DEPTH_REPLAY size=%u samples=%u checked_samples=%zu wrong=%u gpu_ms=%.6f\n",size,samples,reference.size(),replay_wrong,prepass_ms);CHECK(!replay_wrong);
 Target identity[3];std::vector<Pixel> identity_samples[3];double identity_unwarp[3]={};
 for(unsigned f=0;f<3;++f){p[0].filter=f;CHECK(probe(DEPTH_RECONSTRUCT,&p[0])==0 && !p[0].status);
  import(dev,native,identity[f],p[0].output_color,p[0].output_depth,size,samples);
  identity_samples[f]=read_samples(dev,ctx,identity[f],size,samples);identity_unwarp[f]=p[0].reconstruction_gpu_ms;
 }
 double baseline_resolve=0;
 auto baseline_overlay=overlay_resolve(dev,ctx,targets[0],size,baseline_resolve);dump("baseline",baseline_overlay,size,samples);
 const char *identity_names[]={"identity-nearest","identity-bilinear","identity-min"};
 for(unsigned f=0;f<3;++f){double resolve=0;auto image=overlay_resolve(dev,ctx,identity[f],size,resolve);
  compare(identity_names[f],reference,identity_samples[f],baseline_overlay,image,size,samples,scene_ms[0],identity_unwarp[f],resolve,true);
 }
 printf("DEPTH_BASELINE size=%u samples=%u scene_gpu_ms=%.6f resolve_gpu_ms=%.6f total_gpu_ms=%.6f\n",size,samples,scene_ms[0],baseline_resolve,scene_ms[0]+baseline_resolve);
 const char *filters[]={"nearest","bilinear","min"};
 for(unsigned f=0;f<3;++f){p[1].filter=f;CHECK(probe(DEPTH_RECONSTRUCT,&p[1])==0 && !p[1].status);Target output;
  import(dev,native,output,p[1].output_color,p[1].output_depth,size,samples);auto data=read_samples(dev,ctx,output,size,samples);double resolve=0;auto overlay=overlay_resolve(dev,ctx,output,size,resolve);
  dump(filters[f],overlay,size,samples);
  compare(filters[f],reference,data,baseline_overlay,overlay,size,samples,scene_ms[1],p[1].reconstruction_gpu_ms,resolve,false);
  // Keep exact full-resolution depth and materialize colour only.
  p[1].filter=f+3;CHECK(probe(DEPTH_RECONSTRUCT,&p[1])==0 && !p[1].status && !p[1].output_depth);Target hybrid;
  import(dev,native,hybrid,p[1].output_color,p[0].depth,size,samples);
  auto hybrid_data=read_samples(dev,ctx,hybrid,size,samples);double hybrid_resolve=0;
  auto hybrid_overlay=overlay_resolve(dev,ctx,hybrid,size,hybrid_resolve);
  char name[64];snprintf(name,sizeof(name),"%s-full-depth",filters[f]);dump(name,hybrid_overlay,size,samples);
  compare(name,reference,hybrid_data,baseline_overlay,hybrid_overlay,size,samples,scene_ms[1],p[1].reconstruction_gpu_ms,hybrid_resolve,false,prepass_ms);
 }
 ctx->ClearState();ctx->Flush();for(auto &item:p)CHECK(probe(DEPTH_RELEASE,&item)==0);printf("PASS experiment completed; mapped differences are measured, not accepted as compatibility.\n");return 0;
}

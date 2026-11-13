// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Ordinary D3D11 only: no DXMT/native interfaces or application foveation calls.
#include <windows.h>
#include <wincrypt.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;} }while(0)
constexpr unsigned size=2048, samples=24;
static void hash(const char *name,ID3DBlob *blob) {
 HCRYPTPROV provider=0;HCRYPTHASH digest=0;BYTE bytes[20];DWORD len=20;
 if(!CryptAcquireContextA(&provider,nullptr,nullptr,PROV_RSA_FULL,CRYPT_VERIFYCONTEXT))return;
 if(CryptCreateHash(provider,CALG_SHA1,0,0,&digest) &&
    CryptHashData(digest,(BYTE*)blob->GetBufferPointer(),blob->GetBufferSize(),0) &&
    CryptGetHashParam(digest,HP_HASHVAL,bytes,&len,0)) {
  printf("%s=",name);for(auto b:bytes)printf("%02x",b);printf("\n");
 }
 if(digest)CryptDestroyHash(digest);CryptReleaseContext(provider,0);
}
static ID3DBlob *compile(const char *source,const char *entry,const char *profile) {
 ID3DBlob *blob=nullptr,*error=nullptr;
 HRESULT hr=D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,entry,profile,0,0,&blob,&error);
 if(error){fprintf(stderr,"%s",(char*)error->GetBufferPointer());error->Release();}
 return SUCCEEDED(hr)?blob:nullptr;
}
static bool wait(ID3D11DeviceContext *ctx,ID3D11Query *q,void *data,unsigned bytes) {
 ULONGLONG deadline=GetTickCount64()+30000;HRESULT hr;
 while((hr=ctx->GetData(q,data,bytes,0))==S_FALSE && GetTickCount64()<deadline)Sleep(1);
 return hr==S_OK;
}
int main(int argc,char **argv) {
 bool two_draws=argc>1 && !strcmp(argv[1],"--two-draws");
 bool depth=argc>1 && !strcmp(argv[1],"--depth");
 bool msaa=argc>1 && !strcmp(argv[1],"--msaa");
 bool no_clear=argc>1 && !strcmp(argv[1],"--no-clear");
 bool no_uav=argc>1 && !strcmp(argv[1],"--no-uav");
 ID3D11Device *dev=nullptr;ID3D11DeviceContext *ctx=nullptr;
 CHECK(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,nullptr,&ctx)));
 const char *shader="float4 vs(uint i:SV_VertexID):SV_Position {return float4(i==2?3:-1,i==1?3:-1,0,1);}"
  "float4 ps(float4 p:SV_Position):SV_Target {float2 uv=p.xy/2048.0;float a=uv.x+uv.y;"
  "[loop]for(uint i=0;i<128;++i)a=sin(a+float(i)*0.01);return float4(uv,0.25+a*0.000001,1);}";
 auto vs=compile(shader,"vs","vs_5_0"),ps=compile(shader,"ps","ps_5_0");CHECK(vs && ps);
 hash("DXMT_BASE_FOVEATION_VS",vs);hash("DXMT_BASE_FOVEATION_PS",ps);fflush(stdout);
 ID3D11VertexShader *vertex=nullptr;ID3D11PixelShader *pixel=nullptr;
 CHECK(SUCCEEDED(dev->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex)));
 CHECK(SUCCEEDED(dev->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel)));
 vs->Release();ps->Release();ctx->VSSetShader(vertex,nullptr,0);ctx->PSSetShader(pixel,nullptr,0);
 ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 D3D11_VIEWPORT vp={0,0,size,size,0,1};ctx->RSSetViewports(1,&vp);
 D3D11_RASTERIZER_DESC rs={};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;
 ID3D11RasterizerState *raster=nullptr;CHECK(SUCCEEDED(dev->CreateRasterizerState(&rs,&raster)));ctx->RSSetState(raster);
 D3D11_TEXTURE2D_DESC d={};d.Width=d.Height=size;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
 d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE|((no_uav||msaa)?0:D3D11_BIND_UNORDERED_ACCESS);
 if(msaa)d.SampleDesc.Count=4;
 ID3D11Texture2D *target=nullptr,*consumer=nullptr,*staging=nullptr,*resolved=nullptr,*depth_texture=nullptr;
 ID3D11DepthStencilView *dsv=nullptr;
 CHECK(SUCCEEDED(dev->CreateTexture2D(&d,nullptr,&target)));
 ID3D11RenderTargetView *rtv=nullptr;CHECK(SUCCEEDED(dev->CreateRenderTargetView(target,nullptr,&rtv)));
 ID3D11ShaderResourceView *srv=nullptr;
 d.SampleDesc.Count=1;
 if(msaa){d.BindFlags=D3D11_BIND_SHADER_RESOURCE;CHECK(SUCCEEDED(dev->CreateTexture2D(&d,nullptr,&resolved)));}
 CHECK(SUCCEEDED(dev->CreateShaderResourceView(msaa?resolved:target,nullptr,&srv)));
 if(depth){auto dd=d;dd.Format=DXGI_FORMAT_D32_FLOAT;dd.BindFlags=D3D11_BIND_DEPTH_STENCIL;
  CHECK(SUCCEEDED(dev->CreateTexture2D(&dd,nullptr,&depth_texture)));
  CHECK(SUCCEEDED(dev->CreateDepthStencilView(depth_texture,nullptr,&dsv)));}
 d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;CHECK(SUCCEEDED(dev->CreateTexture2D(&d,nullptr,&consumer)));
 ID3D11UnorderedAccessView *uav=nullptr;CHECK(SUCCEEDED(dev->CreateUnorderedAccessView(consumer,nullptr,&uav)));
 d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
 CHECK(SUCCEEDED(dev->CreateTexture2D(&d,nullptr,&staging)));
 D3D11_QUERY_DESC qd={D3D11_QUERY_TIMESTAMP_DISJOINT,0};ID3D11Query *disjoint=nullptr,*begin[samples]={},*end[samples]={};
 CHECK(SUCCEEDED(dev->CreateQuery(&qd,&disjoint)));qd.Query=D3D11_QUERY_TIMESTAMP;
 for(unsigned i=0;i<samples;++i){CHECK(SUCCEEDED(dev->CreateQuery(&qd,&begin[i])));CHECK(SUCCEEDED(dev->CreateQuery(&qd,&end[i])));}
 LARGE_INTEGER freq;QueryPerformanceFrequency(&freq);double cpu[samples]={};
 ctx->Begin(disjoint);ctx->OMSetRenderTargets(1,&rtv,dsv);
 for(unsigned i=0;i<samples;++i){
  LARGE_INTEGER a,b;QueryPerformanceCounter(&a);ctx->End(begin[i]);
  float clear[4]={1,0,1,1};if(dsv)ctx->ClearDepthStencilView(dsv,D3D11_CLEAR_DEPTH,1,0);if(!no_clear)ctx->ClearRenderTargetView(rtv,clear);ctx->Draw(3,0);if(two_draws)ctx->Draw(3,0);
  ctx->End(end[i]);QueryPerformanceCounter(&b);cpu[i]=1000.*(b.QuadPart-a.QuadPart)/freq.QuadPart;
 }
 ctx->End(disjoint);ctx->OMSetRenderTargets(0,nullptr,nullptr);
 if(msaa)ctx->ResolveSubresource(resolved,0,target,0,DXGI_FORMAT_R8G8B8A8_UNORM);
 auto cs=compile("Texture2D<float4> src:register(t0);RWTexture2D<float4> dst:register(u0);"
  "[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){dst[p.xy]=src.Load(int3(p.xy,0));}","main","cs_5_0");CHECK(cs);
 ID3D11ComputeShader *compute=nullptr;CHECK(SUCCEEDED(dev->CreateComputeShader(cs->GetBufferPointer(),cs->GetBufferSize(),nullptr,&compute)));cs->Release();
 ctx->CSSetShader(compute,nullptr,0);ctx->CSSetShaderResources(0,1,&srv);ctx->CSSetUnorderedAccessViews(0,1,&uav,nullptr);ctx->Dispatch(size/8,size/8,1);
 ID3D11ShaderResourceView *null_srv=nullptr;ID3D11UnorderedAccessView *null_uav=nullptr;
 ctx->CSSetShaderResources(0,1,&null_srv);ctx->CSSetUnorderedAccessViews(0,1,&null_uav,nullptr);ctx->CopyResource(staging,consumer);
 D3D11_MAPPED_SUBRESOURCE mapped={};CHECK(SUCCEEDED(ctx->Map(staging,0,D3D11_MAP_READ,0,&mapped)));
 unsigned wrong=0;
 for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x){
  unsigned p=*((unsigned*)((char*)mapped.pData+y*mapped.RowPitch)+x);
  unsigned expected=(unsigned)lround((x+.5)*255/size)|((unsigned)lround((y+.5)*255/size)<<8)|0xff400000u;
  for(unsigned c=0;c<4;++c)if(abs(int((p>>(8*c))&255)-int((expected>>(8*c))&255))>3){
   if(wrong<4)fprintf(stderr,"pixel %u,%u actual=%08x expected=%08x\n",x,y,p,expected);++wrong;break;
  }
 }
 ctx->Unmap(staging,0);
 D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj={};CHECK(wait(ctx,disjoint,&dj,sizeof(dj)) && !dj.Disjoint && dj.Frequency);
 double gpu[samples-3],record[samples-3];
 for(unsigned i=0;i<samples;++i){UINT64 a=0,b=0;CHECK(wait(ctx,begin[i],&a,sizeof(a)) && wait(ctx,end[i],&b,sizeof(b)) && b>a);
  if(i>=3){gpu[i-3]=double(b-a)*1000./dj.Frequency;record[i-3]=cpu[i];}}
 std::sort(gpu,gpu+samples-3);std::sort(record,record+samples-3);
 printf("ORDINARY_D3D11 size=%u samples=%u warmup=3 scene_and_reconstruction_gpu_ms=%.6f cpu_record_ms=%.6f pixels=%u wrong=%u two_draws=%u no_uav=%u\n",
  size,samples,gpu[(samples-3)/2],record[(samples-3)/2],size*size,wrong,two_draws,no_uav);
 printf("FALLBACK depth=%u msaa=%u no_clear=%u\n",depth,msaa,no_clear);
 ctx->ClearState();ctx->Flush();
 for(unsigned i=0;i<samples;++i){begin[i]->Release();end[i]->Release();}disjoint->Release();
 compute->Release();staging->Release();uav->Release();consumer->Release();srv->Release();rtv->Release();target->Release();
 if(dsv)dsv->Release();if(depth_texture)depth_texture->Release();if(resolved)resolved->Release();
 raster->Release();pixel->Release();vertex->Release();ctx->Release();dev->Release();return wrong?1:0;
}

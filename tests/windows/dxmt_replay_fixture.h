#pragma once
// Copyright 2026 Nick Kennedy; SPDX-License-Identifier: LGPL-2.1-or-later
// Private DXMT ownership gate. No foveation, timing or game compatibility claim.
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxmt_replay_probe.h>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
template<class T> struct Ptr {T *p=nullptr;~Ptr(){if(p)p->Release();}T **out(){CHECK(!p);return &p;}operator T*()const{return p;}T*operator->()const{return p;}void reset(){if(p)p->Release();p=nullptr;}};
static ID3DBlob *compile(const char *source,const char *entry,const char *profile,const D3D_SHADER_MACRO *defs=nullptr){ID3DBlob *code=nullptr,*err=nullptr;HRESULT hr=D3DCompile(source,strlen(source),nullptr,defs,nullptr,entry,profile,0,0,&code,&err);if(err){fprintf(stderr,"%s",(char*)err->GetBufferPointer());err->Release();}CHECK(SUCCEEDED(hr));return code;}
static UINT size=128;
static DXGI_FORMAT colour_format=DXGI_FORMAT_R8G8B8A8_UNORM;
struct Scene {
 Ptr<ID3D11Texture2D> colour,depth,texture;Ptr<ID3D11RenderTargetView> rtv;Ptr<ID3D11DepthStencilView> dsv;
 Ptr<ID3D11ShaderResourceView> srv;Ptr<ID3D11Buffer> vb,cb;Ptr<ID3D11InputLayout> layout;
 Ptr<ID3D11VertexShader> vs;Ptr<ID3D11PixelShader> ps;Ptr<ID3D11RasterizerState> rs;Ptr<ID3D11DepthStencilState> ds;
 Scene(ID3D11Device *d,UINT samples){
  D3D11_TEXTURE2D_DESC td={};td.Width=td.Height=size;td.MipLevels=td.ArraySize=1;td.SampleDesc.Count=samples;td.Format=colour_format;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
  CHECK(SUCCEEDED(d->CreateTexture2D(&td,nullptr,colour.out())));CHECK(SUCCEEDED(d->CreateRenderTargetView(colour,nullptr,rtv.out())));
  td.Format=DXGI_FORMAT_R32G8X24_TYPELESS;td.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
  CHECK(SUCCEEDED(d->CreateTexture2D(&td,nullptr,depth.out())));D3D11_DEPTH_STENCIL_VIEW_DESC ddview={};ddview.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;ddview.ViewDimension=samples==1?D3D11_DSV_DIMENSION_TEXTURE2D:D3D11_DSV_DIMENSION_TEXTURE2DMS;CHECK(SUCCEEDED(d->CreateDepthStencilView(depth,&ddview,dsv.out())));
  td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  std::vector<UINT> tex(size*size);for(UINT y=0;y<size;++y)for(UINT x=0;x<size;++x)tex[y*size+x]=0xff000000|((x*2)&255)|(((y*2)&255)<<8)|(((x^y)&255)<<16);
  D3D11_SUBRESOURCE_DATA data={tex.data(),size*4,0};CHECK(SUCCEEDED(d->CreateTexture2D(&td,&data,texture.out())));CHECK(SUCCEEDED(d->CreateShaderResourceView(texture,nullptr,srv.out())));
  D3D11_BUFFER_DESC bd={};bd.ByteWidth=3*16;bd.Usage=D3D11_USAGE_DYNAMIC;bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
  float vertices[12]={-1,-1,.25,1,-.83f,1,.25,1,1,-.73f,.25,1};data={vertices,0,0};CHECK(SUCCEEDED(d->CreateBuffer(&bd,&data,vb.out())));
  bd.ByteWidth=512;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;float constants[128]={};constants[64]=2.5;constants[65]=1.75;constants[66]=3;constants[67]=1;data={constants,0,0};CHECK(SUCCEEDED(d->CreateBuffer(&bd,&data,cb.out())));
  const char *source="cbuffer C:register(b0){float4 tint;}Texture2D<float4> tex:register(t0);float4 vs(float4 p:POSITION):SV_Position{return p;}float4 ps(float4 p:SV_Position):SV_Target{float4 t=tex.Load(int3(p.xy,0));if(((uint(p.x)/7+uint(p.y)/9)%3)==0)discard;return t*tint;}";
  Ptr<ID3DBlob> v,p;v.p=compile(source,"vs","vs_5_0");p.p=compile(source,"ps","ps_5_0");CHECK(SUCCEEDED(d->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,vs.out())));CHECK(SUCCEEDED(d->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,ps.out())));
  D3D11_INPUT_ELEMENT_DESC e={"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};CHECK(SUCCEEDED(d->CreateInputLayout(&e,1,v->GetBufferPointer(),v->GetBufferSize(),layout.out())));
  D3D11_RASTERIZER_DESC rd={};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;rd.MultisampleEnable=TRUE;rd.ScissorEnable=TRUE;CHECK(SUCCEEDED(d->CreateRasterizerState(&rd,rs.out())));
  D3D11_DEPTH_STENCIL_DESC dd={};dd.DepthEnable=TRUE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;dd.DepthFunc=D3D11_COMPARISON_LESS;dd.StencilEnable=TRUE;dd.StencilReadMask=0xff;dd.StencilWriteMask=0xf0;dd.FrontFace.StencilFunc=D3D11_COMPARISON_ALWAYS;dd.FrontFace.StencilFailOp=dd.FrontFace.StencilDepthFailOp=D3D11_STENCIL_OP_KEEP;dd.FrontFace.StencilPassOp=D3D11_STENCIL_OP_REPLACE;dd.BackFace=dd.FrontFace;CHECK(SUCCEEDED(d->CreateDepthStencilState(&dd,ds.out())));
 }
 void bind(ID3D11DeviceContext1 *c){c->ClearState();float clear[4]={.125,.25,.375,1};c->ClearRenderTargetView(rtv,clear);c->ClearDepthStencilView(dsv,D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.8,0x25);ID3D11RenderTargetView *r=rtv;c->OMSetRenderTargets(1,&r,dsv);c->OMSetDepthStencilState(ds,0x85);c->VSSetShader(vs,nullptr,0);c->PSSetShader(ps,nullptr,0);c->IASetInputLayout(layout);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);UINT stride=16,off=0;ID3D11Buffer*b=vb;c->IASetVertexBuffers(0,1,&b,&stride,&off);b=cb;UINT first=16,num=16;c->PSSetConstantBuffers1(0,1,&b,&first,&num);ID3D11ShaderResourceView*s=srv;c->PSSetShaderResources(0,1,&s);c->RSSetState(rs);D3D11_VIEWPORT vp={0,0,FLOAT(size),FLOAT(size),0,1};c->RSSetViewports(1,&vp);D3D11_RECT sc={8,12,120,116};c->RSSetScissorRects(1,&sc);}
 void mutate(ID3D11DeviceContext1 *c){D3D11_MAPPED_SUBRESOURCE m={};CHECK(SUCCEEDED(c->Map(vb,0,D3D11_MAP_WRITE_DISCARD,0,&m)));memset(m.pData,0,48);c->Unmap(vb,0);CHECK(SUCCEEDED(c->Map(cb,0,D3D11_MAP_WRITE_DISCARD,0,&m)));memset(m.pData,0,512);c->Unmap(cb,0);std::vector<UINT> black(size*size,0xff000000);c->UpdateSubresource(texture,0,nullptr,black.data(),size*4,0);}
 void release_inputs(){srv.reset();texture.reset();cb.reset();vb.reset();layout.reset();vs.reset();ps.reset();rs.reset();ds.reset();}
};
static std::vector<UINT> read(ID3D11Device*d,ID3D11DeviceContext1*c,ID3D11Texture2D*colour,ID3D11Texture2D*depth,UINT n){
 c->ClearState();char ns[8];snprintf(ns,sizeof(ns),"%u",n);D3D_SHADER_MACRO macros[]={{"N",ns},{nullptr,nullptr}};
 const char *shader=
 "#if N == 1\nTexture2D<float4> col:register(t0);Texture2D<float> dep:register(t1);Texture2D<uint4> st:register(t2);\n"
 "#else\nTexture2DMS<float4,N> col:register(t0);Texture2DMS<float,N> dep:register(t1);Texture2DMS<uint4,N> st:register(t2);\n#endif\n"
 "RWTexture2D<uint4> outp:register(u0);[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){[unroll]for(uint s=0;s<N;++s){\n"
 "#if N == 1\nfloat4 v=col.Load(int3(p.xy,0));float z=dep.Load(int3(p.xy,0));uint t=st.Load(int3(p.xy,0)).y;\n"
 "#else\nfloat4 v=col.Load(p.xy,s);float z=dep.Load(p.xy,s);uint t=st.Load(p.xy,s).y;\n#endif\n"
 "outp[uint2((p.x*N+s)*2,p.y)]=asuint(v);outp[uint2((p.x*N+s)*2+1,p.y)]=uint4(asuint(z),t,0,0);}}";
 Ptr<ID3DBlob> code;code.p=compile(shader,"main","cs_5_0",macros);Ptr<ID3D11ComputeShader> cs;CHECK(SUCCEEDED(d->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,cs.out())));
 Ptr<ID3D11ShaderResourceView> cv,dv,sv;CHECK(SUCCEEDED(d->CreateShaderResourceView(colour,nullptr,cv.out())));D3D11_SHADER_RESOURCE_VIEW_DESC view={};view.ViewDimension=n==1?D3D11_SRV_DIMENSION_TEXTURE2D:D3D11_SRV_DIMENSION_TEXTURE2DMS;view.Texture2D.MipLevels=1;view.Format=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;CHECK(SUCCEEDED(d->CreateShaderResourceView(depth,&view,dv.out())));view.Format=DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;CHECK(SUCCEEDED(d->CreateShaderResourceView(depth,&view,sv.out())));
 D3D11_TEXTURE2D_DESC td={};td.Width=size*n*2;td.Height=size;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32G32B32A32_UINT;td.BindFlags=D3D11_BIND_UNORDERED_ACCESS;Ptr<ID3D11Texture2D> out,stage;CHECK(SUCCEEDED(d->CreateTexture2D(&td,nullptr,out.out())));Ptr<ID3D11UnorderedAccessView> uav;CHECK(SUCCEEDED(d->CreateUnorderedAccessView(out,nullptr,uav.out())));td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;CHECK(SUCCEEDED(d->CreateTexture2D(&td,nullptr,stage.out())));
 ID3D11ShaderResourceView *views[]={cv,dv,sv};ID3D11UnorderedAccessView*u=uav;c->CSSetShader(cs,nullptr,0);c->CSSetShaderResources(0,3,views);c->CSSetUnorderedAccessViews(0,1,&u,nullptr);c->Dispatch(size/8,size/8,1);c->ClearState();c->CopyResource(stage,out);D3D11_MAPPED_SUBRESOURCE map={};CHECK(SUCCEEDED(c->Map(stage,0,D3D11_MAP_READ,0,&map)));std::vector<UINT> pixels(size*size*n*8);for(UINT y=0;y<size;++y)memcpy(pixels.data()+y*size*n*8,(char*)map.pData+y*map.RowPitch,size*n*32);c->Unmap(stage,0);return pixels;
}

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Ordinary D3D11 regression: no GPU timestamp queries or DXMT interfaces.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(){
 ID3D11Device *d=nullptr;ID3D11DeviceContext *c=nullptr;
 CHECK(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,nullptr,&c)));
 const char *source="float4 vs(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}float4 ps():SV_Target{return float4(0,1,0,1);}";
 ID3DBlob *v=nullptr,*p=nullptr;
 CHECK(SUCCEEDED(D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&v,nullptr)));
 CHECK(SUCCEEDED(D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&p,nullptr)));
 ID3D11VertexShader *vs=nullptr;ID3D11PixelShader *ps=nullptr;
 CHECK(SUCCEEDED(d->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&vs)));
 CHECK(SUCCEEDED(d->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,&ps)));
 v->Release();p->Release();c->VSSetShader(vs,nullptr,0);c->PSSetShader(ps,nullptr,0);
 c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 D3D11_RASTERIZER_DESC rs={};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;
 ID3D11RasterizerState *r=nullptr;CHECK(SUCCEEDED(d->CreateRasterizerState(&rs,&r)));c->RSSetState(r);
 D3D11_VIEWPORT vp={0,0,64,64,0,1};c->RSSetViewports(1,&vp);
 D3D11_TEXTURE2D_DESC desc={};desc.Width=desc.Height=64;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
 desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
 ID3D11Texture2D *a=nullptr,*b=nullptr,*resolved=nullptr,*stage=nullptr;
 CHECK(SUCCEEDED(d->CreateTexture2D(&desc,nullptr,&a)));
 desc.SampleDesc.Count=2;CHECK(SUCCEEDED(d->CreateTexture2D(&desc,nullptr,&b)));
 desc.SampleDesc.Count=1;CHECK(SUCCEEDED(d->CreateTexture2D(&desc,nullptr,&resolved)));
 desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
 CHECK(SUCCEEDED(d->CreateTexture2D(&desc,nullptr,&stage)));
 ID3D11RenderTargetView *av=nullptr,*bv=nullptr;
 CHECK(SUCCEEDED(d->CreateRenderTargetView(a,nullptr,&av)));CHECK(SUCCEEDED(d->CreateRenderTargetView(b,nullptr,&bv)));
 float red[4]={1,0,0,1};c->ClearRenderTargetView(av,red);c->OMSetRenderTargets(1,&av,nullptr);c->Draw(3,0);
 // Rebinding must preserve the result and leave existing optimizer decisions intact.
 c->OMSetRenderTargets(0,nullptr,nullptr);c->OMSetRenderTargets(1,&av,nullptr);c->Draw(3,0);
 c->ClearRenderTargetView(bv,red);c->OMSetRenderTargets(1,&bv,nullptr);c->Draw(3,0);
 c->OMSetRenderTargets(0,nullptr,nullptr);c->ResolveSubresource(resolved,0,b,0,desc.Format);
 unsigned wrong=0;
 for(auto texture:{a,resolved}){
  c->CopyResource(stage,texture);D3D11_MAPPED_SUBRESOURCE mapped={};CHECK(SUCCEEDED(c->Map(stage,0,D3D11_MAP_READ,0,&mapped)));
  for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)wrong+=reinterpret_cast<unsigned*>(static_cast<char*>(mapped.pData)+y*mapped.RowPitch)[x]!=0xff00ff00;
  c->Unmap(stage,0);
 }
 printf("TBDR_PROBE pixels=8192 wrong=%u\n",wrong);c->ClearState();c->Flush();
 av->Release();bv->Release();a->Release();b->Release();resolved->Release();stage->Release();r->Release();vs->Release();ps->Release();c->Release();d->Release();return wrong?1:0;
}

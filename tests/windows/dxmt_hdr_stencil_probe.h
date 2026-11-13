// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// D3D-owned HDR/combined-stencil correctness gate, not a replay implementation.
namespace hdr_stencil {
struct Surface {Target t;Ptr<ID3D11ShaderResourceView> stencil;Ptr<ID3D11DepthStencilView> read_only_depth;};
static void create(ID3D11Device *d,Surface &s,unsigned size,unsigned n){
 D3D11_TEXTURE2D_DESC td={};td.Width=td.Height=size;td.MipLevels=td.ArraySize=1;td.SampleDesc.Count=n;td.Format=DXGI_FORMAT_R11G11B10_FLOAT;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
 CHECK(SUCCEEDED(d->CreateTexture2D(&td,nullptr,s.t.color.out())));CHECK(SUCCEEDED(d->CreateRenderTargetView(s.t.color,nullptr,s.t.rtv.out())));CHECK(SUCCEEDED(d->CreateShaderResourceView(s.t.color,nullptr,s.t.csrv.out())));
 td.Format=DXGI_FORMAT_R32G8X24_TYPELESS;td.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;CHECK(SUCCEEDED(d->CreateTexture2D(&td,nullptr,s.t.depth.out())));
 D3D11_DEPTH_STENCIL_VIEW_DESC dd={};dd.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;dd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2DMS;CHECK(SUCCEEDED(d->CreateDepthStencilView(s.t.depth,&dd,s.t.dsv.out())));
 dd.Flags=D3D11_DSV_READ_ONLY_DEPTH;CHECK(SUCCEEDED(d->CreateDepthStencilView(s.t.depth,&dd,s.read_only_depth.out())));
 D3D11_SHADER_RESOURCE_VIEW_DESC sv={};sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DMS;sv.Format=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;CHECK(SUCCEEDED(d->CreateShaderResourceView(s.t.depth,&sv,s.t.dsrv.out())));
 sv.Format=DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;CHECK(SUCCEEDED(d->CreateShaderResourceView(s.t.depth,&sv,s.stencil.out())));
}
static std::vector<unsigned> read_stencil(ID3D11Device *d,ID3D11DeviceContext *c,Surface &s,unsigned size,unsigned n){
 char ns[8];snprintf(ns,sizeof(ns),"%u",n);D3D_SHADER_MACRO m[]={{"N",ns},{nullptr,nullptr}};Ptr<ID3DBlob> code;code.p=compile("Texture2DMS<uint4,N> src:register(t0);RWTexture2D<uint> dst:register(u0);[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){uint w,h,n;src.GetDimensions(w,h,n);if(p.x>=w||p.y>=h)return;[unroll]for(uint s=0;s<N;++s)dst[uint2(p.x*N+s,p.y)]=src.Load(p.xy,s).y;}","main","cs_5_0",m);
 Ptr<ID3D11ComputeShader> cs;CHECK(SUCCEEDED(d->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,cs.out())));
 D3D11_TEXTURE2D_DESC td={};td.Width=size*n;td.Height=size;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32_UINT;td.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
 Ptr<ID3D11Texture2D> out,stage;CHECK(SUCCEEDED(d->CreateTexture2D(&td,nullptr,out.out())));Ptr<ID3D11UnorderedAccessView> uav;CHECK(SUCCEEDED(d->CreateUnorderedAccessView(out,nullptr,uav.out())));
 td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;CHECK(SUCCEEDED(d->CreateTexture2D(&td,nullptr,stage.out())));
 c->OMSetRenderTargets(0,nullptr,nullptr);ID3D11ShaderResourceView *srv=s.stencil;ID3D11UnorderedAccessView *u=uav;c->CSSetShader(cs,nullptr,0);c->CSSetShaderResources(0,1,&srv);c->CSSetUnorderedAccessViews(0,1,&u,nullptr);c->Dispatch((size+7)/8,(size+7)/8,1);
 srv=nullptr;u=nullptr;c->CSSetShaderResources(0,1,&srv);c->CSSetUnorderedAccessViews(0,1,&u,nullptr);c->CopyResource(stage,out);D3D11_MAPPED_SUBRESOURCE map={};CHECK(SUCCEEDED(c->Map(stage,0,D3D11_MAP_READ,0,&map)));std::vector<unsigned> v(size*size*n);for(unsigned y=0;y<size;++y)memcpy(v.data()+y*size*n,(char*)map.pData+y*map.RowPitch,size*n*4);c->Unmap(stage,0);return v;
}
static float resolved_peak(ID3D11Device *d,ID3D11DeviceContext *c,ID3D11Texture2D *src,unsigned size){
 Ptr<ID3DBlob> code;code.p=compile("Texture2D<float4> src:register(t0);RWTexture2D<float4> dst:register(u0);[numthreads(8,8,1)]void main(uint3 i:SV_DispatchThreadID){dst[i.xy]=src.Load(int3(i.xy,0));}","main","cs_5_0");Ptr<ID3D11ComputeShader> cs;CHECK(SUCCEEDED(d->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,cs.out())));
 Ptr<ID3D11ShaderResourceView> srv;CHECK(SUCCEEDED(d->CreateShaderResourceView(src,nullptr,srv.out())));D3D11_TEXTURE2D_DESC td={};src->GetDesc(&td);td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
 Ptr<ID3D11Texture2D> out,stage;CHECK(SUCCEEDED(d->CreateTexture2D(&td,nullptr,out.out())));Ptr<ID3D11UnorderedAccessView> uav;CHECK(SUCCEEDED(d->CreateUnorderedAccessView(out,nullptr,uav.out())));td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;CHECK(SUCCEEDED(d->CreateTexture2D(&td,nullptr,stage.out())));
 ID3D11ShaderResourceView *v=srv;ID3D11UnorderedAccessView *u=uav;c->CSSetShader(cs,nullptr,0);c->CSSetShaderResources(0,1,&v);c->CSSetUnorderedAccessViews(0,1,&u,nullptr);c->Dispatch(size/8,size/8,1);v=nullptr;u=nullptr;c->CSSetShaderResources(0,1,&v);c->CSSetUnorderedAccessViews(0,1,&u,nullptr);c->CopyResource(stage,out);D3D11_MAPPED_SUBRESOURCE map={};CHECK(SUCCEEDED(c->Map(stage,0,D3D11_MAP_READ,0,&map)));float peak=0;for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x){float *p=(float*)((char*)map.pData+y*map.RowPitch)+4*x;peak=std::max(peak,std::max(p[0],std::max(p[1],p[2])));}c->Unmap(stage,0);return peak;
}
struct Scene {
 Ptr<ID3D11Buffer> vb,cb;Ptr<ID3D11VertexShader> vs,fullscreen;Ptr<ID3D11PixelShader> main_ps,mask_ps,marker_ps;
 Ptr<ID3D11InputLayout> layout;Ptr<ID3D11DepthStencilState> pre,main,one,consumer;
 Ptr<ID3D11Texture2D> alpha;Ptr<ID3D11ShaderResourceView> alpha_srv;Ptr<ID3D11SamplerState> alpha_sampler;
 Scene(ID3D11Device *d,unsigned n,unsigned work=0,bool textured=false){
  Vertex v[]={ {-1,-1,.1,2,.5,.25,0,1},{3,-1,.1,2,.5,.25,2,1},{-1,3,.1,2,.5,.25,0,-1},
   {-.9,-.7,.6,.25,3,.5,.05,.85},{.65,-.6,.6,.25,3,.5,.825,.8},{-.25,.85,.6,.25,3,.5,.375,.075},
   {-.81,-.8,.8,4,.1,2,.095,.9},{-.80,-.8,.8,4,.1,2,.1,.9},{-.81,.8,.8,4,.1,2,.095,.1},
   {-.81,.8,.8,4,.1,2,.095,.1},{-.80,-.8,.8,4,.1,2,.1,.9},{-.80,.8,.8,4,.1,2,.1,.1}};
  D3D11_BUFFER_DESC bd={};bd.ByteWidth=sizeof(v);bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;D3D11_SUBRESOURCE_DATA data={v,0,0};CHECK(SUCCEEDED(d->CreateBuffer(&bd,&data,vb.out())));
  bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;CHECK(SUCCEEDED(d->CreateBuffer(&bd,nullptr,cb.out())));
  const char *hlsl="Texture2D<float> alpha:register(t0);SamplerState alphaSampler:register(s0);cbuffer C:register(b0){uint kind;};struct I{float3 p:POSITION;float3 c:COLOR;float2 uv:TEXCOORD;};struct V{float4 p:SV_Position;float3 c:COLOR;float2 uv:TEXCOORD;};V vs(I i){V o;o.p=float4(i.p,1);o.c=i.c;o.uv=i.uv;return o;}"
   "uint cov(V i){if(kind!=0){float a=TEXTURED?alpha.SampleBias(alphaSampler,i.uv,0):frac(i.uv.x*13+i.uv.y*7);clip(a-.25);return ((uint)(i.uv.y*19)&1)?ALL:1;}return ALL;}"
   "uint mask(V i):SV_Coverage{return cov(i);}struct O{float3 c:SV_Target;uint mask:SV_Coverage;};O main(V i){O o;o.mask=cov(i);float a=i.uv.x+i.uv.y;[loop]for(uint j=0;j<WORK;++j)a=sin(a+j*.01);o.c=i.c*(WORK?(.8+.1*a):1);return o;}"
   "float4 full(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}float4 mark():SV_Target{return float4(0,1,1,1);}";
  char all[8];snprintf(all,sizeof(all),"%u",(1u<<n)-1);char iterations[16];snprintf(iterations,sizeof(iterations),"%u",work);D3D_SHADER_MACRO macros[]={{"ALL",all},{"WORK",iterations},{"TEXTURED",textured?"1":"0"},{nullptr,nullptr}};
  Ptr<ID3DBlob> a,b,c,f,g;a.p=compile(hlsl,"vs","vs_5_0",macros);b.p=compile(hlsl,"main","ps_5_0",macros);c.p=compile(hlsl,"mask","ps_5_0",macros);f.p=compile(hlsl,"full","vs_5_0",macros);g.p=compile(hlsl,"mark","ps_5_0",macros);
  CHECK(SUCCEEDED(d->CreateVertexShader(a->GetBufferPointer(),a->GetBufferSize(),nullptr,vs.out())));CHECK(SUCCEEDED(d->CreateVertexShader(f->GetBufferPointer(),f->GetBufferSize(),nullptr,fullscreen.out())));
  CHECK(SUCCEEDED(d->CreatePixelShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,main_ps.out())));CHECK(SUCCEEDED(d->CreatePixelShader(c->GetBufferPointer(),c->GetBufferSize(),nullptr,mask_ps.out())));CHECK(SUCCEEDED(d->CreatePixelShader(g->GetBufferPointer(),g->GetBufferSize(),nullptr,marker_ps.out())));
  D3D11_INPUT_ELEMENT_DESC e[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"COLOR",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},{"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,24,D3D11_INPUT_PER_VERTEX_DATA,0}};CHECK(SUCCEEDED(d->CreateInputLayout(e,3,a->GetBufferPointer(),a->GetBufferSize(),layout.out())));
  if(textured){
   unsigned char pixels[64*64];for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)pixels[y*64+x]=((x/4+y/5)%3==0)?0:((x/4+y/5)%3==1)?128:255;
   D3D11_TEXTURE2D_DESC td={};td.Width=td.Height=64;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8_UNORM;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;td.Usage=D3D11_USAGE_IMMUTABLE;D3D11_SUBRESOURCE_DATA init={pixels,64,0};CHECK(SUCCEEDED(d->CreateTexture2D(&td,&init,alpha.out())));CHECK(SUCCEEDED(d->CreateShaderResourceView(alpha,nullptr,alpha_srv.out())));
   D3D11_SAMPLER_DESC sd={};sd.Filter=D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;sd.MaxLOD=D3D11_FLOAT32_MAX;CHECK(SUCCEEDED(d->CreateSamplerState(&sd,alpha_sampler.out())));
  }
  D3D11_DEPTH_STENCIL_DESC ds={};ds.DepthEnable=TRUE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ds.DepthFunc=D3D11_COMPARISON_GREATER_EQUAL;CHECK(SUCCEEDED(d->CreateDepthStencilState(&ds,pre.out())));
  ds.StencilEnable=TRUE;ds.StencilReadMask=255;ds.StencilWriteMask=240;ds.FrontFace.StencilFunc=D3D11_COMPARISON_ALWAYS;ds.FrontFace.StencilFailOp=ds.FrontFace.StencilDepthFailOp=D3D11_STENCIL_OP_KEEP;ds.FrontFace.StencilPassOp=D3D11_STENCIL_OP_REPLACE;ds.BackFace=ds.FrontFace;CHECK(SUCCEEDED(d->CreateDepthStencilState(&ds,one.out())));
  ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;CHECK(SUCCEEDED(d->CreateDepthStencilState(&ds,main.out())));
  ds.DepthEnable=FALSE;ds.StencilReadMask=0x81;ds.StencilWriteMask=0;ds.FrontFace.StencilFunc=D3D11_COMPARISON_EQUAL;ds.FrontFace.StencilPassOp=D3D11_STENCIL_OP_KEEP;ds.BackFace=ds.FrontFace;CHECK(SUCCEEDED(d->CreateDepthStencilState(&ds,consumer.out())));
 }
 void draw(ID3D11DeviceContext *c,Surface &s,int mode,bool drop_mask=false,bool clear_color=true,bool coverage_only=false){ // 0 prepass, 1 shade, 2 one pass
  if(mode!=1)c->ClearDepthStencilView(s.t.dsv,D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,0,5);
  ID3D11RenderTargetView *rt=s.t.rtv;c->OMSetRenderTargets(mode==0?0:1,mode==0?nullptr:&rt,mode==1?s.read_only_depth.p:s.t.dsv.p);if(mode&&clear_color){float clear[4]={};c->ClearRenderTargetView(rt,clear);}
  UINT stride=sizeof(Vertex),offset=0;ID3D11Buffer *v=vb,*constant=cb;c->IASetVertexBuffers(0,1,&v,&stride,&offset);c->IASetInputLayout(layout);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c->VSSetShader(vs,nullptr,0);c->PSSetShader(mode?(coverage_only?mask_ps.p:main_ps.p):drop_mask?nullptr:mask_ps.p,nullptr,0);c->PSSetConstantBuffers(0,1,&constant);
  ID3D11ShaderResourceView *a=alpha_srv;ID3D11SamplerState *sm=alpha_sampler;c->PSSetShaderResources(0,1,&a);c->PSSetSamplers(0,1,&sm);
  for(unsigned i=0;i<3;++i){unsigned params[4]={i==1?1u:0u,0,0,0};c->UpdateSubresource(cb,0,nullptr,params,0,0);c->OMSetDepthStencilState(mode==0?pre.p:mode==1?main.p:one.p,(i+1)*0x40);c->Draw(i==2?6:3,i*3);}
  c->OMSetRenderTargets(0,nullptr,nullptr);
 }
 unsigned marker(ID3D11Device *d,ID3D11DeviceContext *c,Surface &s,unsigned size,unsigned n){
  Target out;D3D11_TEXTURE2D_DESC td={};s.t.color->GetDesc(&td);td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;CHECK(SUCCEEDED(d->CreateTexture2D(&td,nullptr,out.color.out())));CHECK(SUCCEEDED(d->CreateRenderTargetView(out.color,nullptr,out.rtv.out())));CHECK(SUCCEEDED(d->CreateShaderResourceView(out.color,nullptr,out.csrv.out())));
  out.depth.p=s.t.depth;out.depth->AddRef();out.dsv.p=s.read_only_depth;out.dsv->AddRef();out.dsrv.p=s.t.dsrv;out.dsrv->AddRef();float clear[4]={};c->ClearRenderTargetView(out.rtv,clear);ID3D11RenderTargetView *rt=out.rtv;c->OMSetRenderTargets(1,&rt,out.dsv);c->IASetInputLayout(nullptr);c->VSSetShader(fullscreen,nullptr,0);c->PSSetShader(marker_ps,nullptr,0);c->OMSetDepthStencilState(consumer,0x81);c->Draw(3,0);c->OMSetRenderTargets(0,nullptr,nullptr);
  auto pixels=read_samples(d,c,out,size,n);unsigned count=0;for(auto &p:pixels)count+=p.g>.9;return count;
 }
};
static int run(ID3D11Device *d,ID3D11DeviceContext *c,IDXMTNativeContext1 *rate,LONG(WINAPI *probe)(UINT,void*),depth_foveation_params *p,unsigned size,unsigned n,float peripheral){
 Surface ordinary,mapped;create(d,ordinary,size,n);create(d,mapped,size,n);Scene draw(d,n);
 draw.draw(c,ordinary,2);auto reference=read_samples(d,c,ordinary.t,size,n);auto stencil=read_stencil(d,c,ordinary,size,n);
 unsigned expected_marker=0;float peak=0;for(size_t i=0;i<reference.size();++i){peak=std::max(peak,std::max(reference[i].r,std::max(reference[i].g,reference[i].b)));expected_marker+=(stencil[i]&0x81)==0x81;CHECK((stencil[i]&15)==5);}
 CHECK(peak>1&&expected_marker>0);unsigned marker_reference=draw.marker(d,c,ordinary,size,n);CHECK(marker_reference==expected_marker);
 draw.draw(c,ordinary,0);draw.draw(c,ordinary,1);auto two=read_samples(d,c,ordinary.t,size,n);auto two_st=read_stencil(d,c,ordinary,size,n);unsigned identity_bad=0;for(size_t i=0;i<two.size();++i)identity_bad+=memcmp(&two[i],&reference[i],sizeof(Pixel))||two_st[i]!=stencil[i];CHECK(!identity_bad);
 CHECK(SUCCEEDED(rate->SetRasterizationRateMap(p[1].map)));draw.draw(c,mapped,2);CHECK(SUCCEEDED(rate->SetRasterizationRateMap(0)));auto mapped_ref=read_samples(d,c,mapped.t,size,n);auto mapped_st=read_stencil(d,c,mapped,size,n);unsigned fullrate_bad=0;if(peripheral==1){for(size_t i=0;i<mapped_ref.size();++i)fullrate_bad+=memcmp(&mapped_ref[i],&reference[i],sizeof(Pixel))||mapped_st[i]!=stencil[i];CHECK(!fullrate_bad);}
 CHECK(SUCCEEDED(rate->SetRasterizationRateMap(p[1].map)));draw.draw(c,mapped,0);draw.draw(c,mapped,1);CHECK(SUCCEEDED(rate->SetRasterizationRateMap(0)));auto mapped_two=read_samples(d,c,mapped.t,size,n);auto mapped_two_st=read_stencil(d,c,mapped,size,n);unsigned mapped_bad=0;
 for(unsigned y=0;y<p[1].physical_height;++y)for(unsigned x=0;x<p[1].physical_width;++x)for(unsigned s=0;s<n;++s){size_t i=(y*size+x)*n+s;mapped_bad+=memcmp(&mapped_ref[i],&mapped_two[i],sizeof(Pixel))||mapped_st[i]!=mapped_two_st[i];}CHECK(!mapped_bad);
 // A naive null-PS depth replay loses alpha/coverage semantics.
 draw.draw(c,ordinary,0,true);draw.draw(c,ordinary,1);auto dropped=read_samples(d,c,ordinary.t,size,n);unsigned drop_bad=0;for(size_t i=0;i<dropped.size();++i)drop_bad+=memcmp(&dropped[i],&reference[i],sizeof(Pixel))!=0;CHECK(drop_bad>0);
 // Retaining ordinary prepass depth while shading only hidden targets leaves ordinary stencil stale.
 draw.draw(c,ordinary,0);auto retained=read_samples(d,c,ordinary.t,size,n);auto retained_st=read_stencil(d,c,ordinary,size,n);unsigned depth_bad=0,stale_stencil=0;for(size_t i=0;i<retained.size();++i){depth_bad+=retained[i].d!=reference[i].d;stale_stencil+=retained_st[i]!=stencil[i];}CHECK(!depth_bad&&stale_stencil>0);
 unsigned marker_stale=draw.marker(d,c,ordinary,size,n);CHECK(marker_stale==0&&marker_reference>marker_stale);
 // Exact stencil-only replay is a correctness control, not a free operation.
 D3D11_BLEND_DESC blend={};blend.RenderTarget[0].RenderTargetWriteMask=0;Ptr<ID3D11BlendState> no_color;CHECK(SUCCEEDED(d->CreateBlendState(&blend,no_color.out())));c->OMSetBlendState(no_color,nullptr,0xffffffff);
 draw.draw(c,ordinary,1,false,false);c->OMSetBlendState(nullptr,nullptr,0xffffffff);
 auto restored=read_stencil(d,c,ordinary,size,n);auto after=read_samples(d,c,ordinary.t,size,n);unsigned restore_bad=0,restore_color_depth_bad=0;
 for(size_t i=0;i<restored.size();++i){restore_bad+=restored[i]!=stencil[i];restore_color_depth_bad+=memcmp(&after[i],&retained[i],sizeof(Pixel))!=0;}
 CHECK(!restore_bad&&!restore_color_depth_bad);unsigned marker_restored=draw.marker(d,c,ordinary,size,n);CHECK(marker_restored==marker_reference);
 // Native MSAA HDR resolve must preserve values above 1, independently of the marker target.
 D3D11_TEXTURE2D_DESC td={};mapped.t.color->GetDesc(&td);td.SampleDesc.Count=1;Ptr<ID3D11Texture2D> resolved;c->OMSetRenderTargets(0,nullptr,nullptr);CHECK(SUCCEEDED(d->CreateTexture2D(&td,nullptr,resolved.out())));c->ResolveSubresource(resolved,0,mapped.t.color,0,td.Format);float resolve_peak=resolved_peak(d,c,resolved,size);CHECK(resolve_peak>1);
 printf("HDR_STENCIL size=%u samples=%u rate=%.3f peak_hdr=%.3f identity_bad=%u mapped_bad=%u dropped_mask_bad=%u retained_depth_bad=%u stale_stencil_samples=%u marker_reference=%u marker_stale=%u restored_stencil_bad=%u restored_color_depth_bad=%u marker_restored=%u resolved_peak_hdr=%.3f fullrate_bad=%u\n",size,n,peripheral,peak,identity_bad,mapped_bad,drop_bad,depth_bad,stale_stencil,marker_reference,marker_stale,restore_bad,restore_color_depth_bad,marker_restored,resolve_peak,fullrate_bad);fflush(stdout);
 c->ClearState();c->Flush();for(unsigned i=0;i<2;++i)CHECK(probe(DEPTH_RELEASE,&p[i])==0);return 0;
}
}

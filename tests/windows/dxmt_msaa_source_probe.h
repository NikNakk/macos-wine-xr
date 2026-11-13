// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Eager per-sample HDR materialisation and ordinary subsequent source uses.
struct MSAASource {
 Ptr<ID3D11Buffer> axes;Ptr<ID3D11ShaderResourceView> axes_srv,alias_srv,snapshot_srv;
 Ptr<ID3D11RenderTargetView> alias_rtv,snapshot_rtv,collapse_rtv;
 Ptr<ID3D11Texture2D> snapshot,collapse_texture;Ptr<ID3D11ShaderResourceView> collapse_srv;
 Ptr<ID3D11PixelShader> reconstruct,read_ms,load_patch;Ptr<ID3D11DepthStencilState> off;
 Ptr<ID3D11BlendState> add;std::vector<float> xy;unsigned size,n,pw,ph;
 MSAASource(ID3D11Device *d,LONG(WINAPI *probe)(UINT,void*),depth_foveation_params &p,hdr_stencil::Surface &ordinary):size(p.size),n(p.samples),pw(p.physical_width),ph(p.physical_height){
  xy.resize(n*(2*size+2));p.axis_lut=(uint64_t)(uintptr_t)xy.data();CHECK(probe(DEPTH_SAMPLE_AXIS_LUT,&p)==0&&!p.status);p.axis_lut=0;
  D3D11_BUFFER_DESC bd={};bd.ByteWidth=xy.size()*4;bd.BindFlags=D3D11_BIND_SHADER_RESOURCE;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.StructureByteStride=4;D3D11_SUBRESOURCE_DATA data={xy.data(),0,0};CHECK(SUCCEEDED(d->CreateBuffer(&bd,&data,axes.out())));CHECK(SUCCEEDED(d->CreateShaderResourceView(axes,nullptr,axes_srv.out())));
  CHECK(SUCCEEDED(d->CreateShaderResourceView(ordinary.t.color,nullptr,alias_srv.out())));CHECK(SUCCEEDED(d->CreateRenderTargetView(ordinary.t.color,nullptr,alias_rtv.out())));
  ID3D11Resource *r=nullptr;alias_srv->GetResource(&r);CHECK(r==static_cast<ID3D11Resource*>(ordinary.t.color.p));r->Release();alias_rtv->GetResource(&r);CHECK(r==static_cast<ID3D11Resource*>(ordinary.t.color.p));r->Release();
  D3D11_TEXTURE2D_DESC td={};ordinary.t.color->GetDesc(&td);CHECK(SUCCEEDED(d->CreateTexture2D(&td,nullptr,collapse_texture.out())));CHECK(SUCCEEDED(d->CreateRenderTargetView(collapse_texture,nullptr,collapse_rtv.out())));CHECK(SUCCEEDED(d->CreateShaderResourceView(collapse_texture,nullptr,collapse_srv.out())));
  td.SampleDesc.Count=1;CHECK(SUCCEEDED(d->CreateTexture2D(&td,nullptr,snapshot.out())));CHECK(SUCCEEDED(d->CreateRenderTargetView(snapshot,nullptr,snapshot_rtv.out())));CHECK(SUCCEEDED(d->CreateShaderResourceView(snapshot,nullptr,snapshot_srv.out())));
  char ns[8],sz[16],wp[16],hp[16];snprintf(ns,sizeof(ns),"%u",n);snprintf(sz,sizeof(sz),"%u",size);snprintf(wp,sizeof(wp),"%u",pw);snprintf(hp,sizeof(hp),"%u",ph);D3D_SHADER_MACRO macros[]={{"N",ns},{"W",sz},{"PW",wp},{"PH",hp},{nullptr,nullptr}};
  const char *code="Texture2DMS<float4,N> src:register(t0);StructuredBuffer<float> xy:register(t1);"
   "float3 materialize(float4 p:SV_Position,uint s:SV_SampleIndex):SV_Target{uint2 i=uint2(p.xy);uint k=s*2*W;float2 offset=float2(xy[2*W*N+2*s],xy[2*W*N+2*s+1]);float2 q=float2(xy[k+i.x],xy[k+W+i.y])-offset;int2 lo=int2(floor(q));float2 f=q-float2(lo);int2 hi=int2(PW-1,PH-1);"
   "float3 a=src.Load(clamp(lo,int2(0,0),hi),s).rgb,b=src.Load(clamp(lo+int2(1,0),int2(0,0),hi),s).rgb,c=src.Load(clamp(lo+int2(0,1),int2(0,0),hi),s).rgb,d=src.Load(clamp(lo+1,int2(0,0),hi),s).rgb;return lerp(lerp(a,b,f.x),lerp(c,d,f.x),f.y);}"
   "float3 readAll(float4 p:SV_Position):SV_Target{float3 a=0;[unroll]for(uint s=0;s<N;++s)a+=src.Load(int2(p.xy),s).rgb;return a/N;}";
  Ptr<ID3DBlob> a,b,c;a.p=compile(code,"materialize","ps_5_0",macros);b.p=compile(code,"readAll","ps_5_0",macros);
  CHECK(SUCCEEDED(d->CreatePixelShader(a->GetBufferPointer(),a->GetBufferSize(),nullptr,reconstruct.out())));CHECK(SUCCEEDED(d->CreatePixelShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,read_ms.out())));
  c.p=compile("Texture2D<float4> prior:register(t0);float3 patch(float4 p:SV_Position):SV_Target{uint w,h;prior.GetDimensions(w,h);if(p.x<w/16||p.x>=w*7/16||p.y<h/8||p.y>=h*6/8)discard;return prior.Load(int3(int2(p.xy),0)).rgb*.125;}","patch","ps_5_0");CHECK(SUCCEEDED(d->CreatePixelShader(c->GetBufferPointer(),c->GetBufferSize(),nullptr,load_patch.out())));
  D3D11_DEPTH_STENCIL_DESC ds={};ds.DepthFunc=D3D11_COMPARISON_ALWAYS;CHECK(SUCCEEDED(d->CreateDepthStencilState(&ds,off.out())));
  D3D11_BLEND_DESC blend={};auto &rt=blend.RenderTarget[0];rt.BlendEnable=TRUE;rt.SrcBlend=rt.DestBlend=D3D11_BLEND_ONE;rt.BlendOp=D3D11_BLEND_OP_ADD;rt.SrcBlendAlpha=D3D11_BLEND_ONE;rt.DestBlendAlpha=D3D11_BLEND_ZERO;rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;CHECK(SUCCEEDED(d->CreateBlendState(&blend,add.out())));
 }
 void fullscreen(ID3D11DeviceContext *c,hdr_stencil::Scene &scene,ID3D11RenderTargetView *rt,ID3D11PixelShader *ps,ID3D11ShaderResourceView *src,bool lut){
  c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c->VSSetShader(scene.fullscreen,nullptr,0);c->PSSetShader(ps,nullptr,0);c->OMSetBlendState(nullptr,nullptr,0xffffffff);c->OMSetDepthStencilState(off,0);c->OMSetRenderTargets(1,&rt,nullptr);
  ID3D11ShaderResourceView *v[]={src,lut?axes_srv.p:nullptr};c->PSSetShaderResources(0,2,v);c->Draw(3,0);v[0]=v[1]=nullptr;c->PSSetShaderResources(0,2,v);c->OMSetRenderTargets(0,nullptr,nullptr);
 }
 void materialize(ID3D11DeviceContext *c,hdr_stencil::Scene &scene,hdr_stencil::Surface &hidden){fullscreen(c,scene,alias_rtv,reconstruct,hidden.t.csrv,true);}
 void read_source(ID3D11DeviceContext *c,hdr_stencil::Scene &scene,ID3D11ShaderResourceView *src=nullptr){fullscreen(c,scene,snapshot_rtv,read_ms,src?src:alias_srv.p,false);}
 // target: the surface whose colour is loaded and patched; defaults to the ordinary alias view.
 void load(ID3D11DeviceContext *c,hdr_stencil::Scene &scene,hdr_stencil::Surface &ordinary,bool own_view=false){
  ID3D11RenderTargetView *rt=own_view?ordinary.t.rtv.p:alias_rtv.p;c->OMSetRenderTargets(1,&rt,ordinary.read_only_depth);c->OMSetBlendState(add,nullptr,0xffffffff);c->OMSetDepthStencilState(scene.consumer,0x81);c->VSSetShader(scene.fullscreen,nullptr,0);c->PSSetShader(load_patch,nullptr,0);ID3D11ShaderResourceView *v=snapshot_srv;c->PSSetShaderResources(0,1,&v);c->Draw(3,0);v=nullptr;c->PSSetShaderResources(0,1,&v);c->OMSetRenderTargets(0,nullptr,nullptr);c->OMSetBlendState(nullptr,nullptr,0xffffffff);
 }
 bool patch(unsigned x,unsigned y,unsigned stencil)const{return x>=size/16&&x<size*7/16&&y>=size/8&&y<size*6/8&&(stencil&0x81)==0x81;}
 static bool close_colour(float a,float b){return std::isfinite(a)&&fabsf(a-b)<=std::max(1e-5f,fabsf(b)/32.f);}
 Pixel expected(const std::vector<Pixel> &src,unsigned x,unsigned y,unsigned s)const{
  float qx=xy[s*2*size+x]-xy[2*size*n+2*s],qy=xy[s*2*size+size+y]-xy[2*size*n+2*s+1];int bx=(int)floorf(qx),by=(int)floorf(qy);float fx=qx-bx,fy=qy-by;
  auto at=[&](int a,int b){return src[(std::max(0,std::min((int)ph-1,b))*size+std::max(0,std::min((int)pw-1,a)))*n+s];};
  Pixel a=at(bx,by),b=at(bx+1,by),c=at(bx,by+1),d=at(bx+1,by+1),o={};float aa[]={a.r,a.g,a.b},bb[]={b.r,b.g,b.b},cc[]={c.r,c.g,c.b},dd[]={d.r,d.g,d.b},out[3];for(unsigned k=0;k<3;++k)out[k]=(aa[k]+(bb[k]-aa[k])*fx)*(1-fy)+(cc[k]+(dd[k]-cc[k])*fx)*fy;o.r=out[0];o.g=out[1];o.b=out[2];return o;
 }
};

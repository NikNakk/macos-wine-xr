// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Joined HDR/stencil cost experiment. No game shader rewriting or replay interception.
namespace hdr_joined {
struct Post {
 Ptr<ID3D11Texture2D> resolved,reconstructed,final;Ptr<ID3D11RenderTargetView> reconstruct_rtv;
 Ptr<ID3D11ShaderResourceView> resolved_srv,reconstructed_srv,axis_srv,final_srv;Ptr<ID3D11Buffer> axis;
 Target consumer_target;Ptr<ID3D11PixelShader> unwarp,consume,present_ps;Ptr<ID3D11SamplerState> sampler;
 // EXD-style depth use: fullscreen max of the MSAA depth samples into a single-sample
 // depth target, which the stencil-tested consumer then reads with Load.
 Ptr<ID3D11Texture2D> depth_max;Ptr<ID3D11DepthStencilView> depth_max_dsv;Ptr<ID3D11ShaderResourceView> depth_max_srv;
 Ptr<ID3D11PixelShader> depth_max_ps,consume_depth;Ptr<ID3D11DepthStencilState> depth_always;
 void enable_depth(ID3D11Device *d,unsigned n){
  D3D11_TEXTURE2D_DESC td={};td.Width=td.Height=size;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32G8X24_TYPELESS;td.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;CHECK(SUCCEEDED(d->CreateTexture2D(&td,nullptr,depth_max.out())));
  D3D11_DEPTH_STENCIL_VIEW_DESC dd={};dd.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;dd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;CHECK(SUCCEEDED(d->CreateDepthStencilView(depth_max,&dd,depth_max_dsv.out())));
  D3D11_SHADER_RESOURCE_VIEW_DESC sv={};sv.Format=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MipLevels=1;CHECK(SUCCEEDED(d->CreateShaderResourceView(depth_max,&sv,depth_max_srv.out())));
  char ns[8];snprintf(ns,sizeof(ns),"%u",n);D3D_SHADER_MACRO macros[]={{"N",ns},{nullptr,nullptr}};
  const char *hlsl="Texture2DMS<float,N> msd:register(t0);float depthmax(float4 p:SV_Position):SV_Depth{float m=0;[unroll]for(uint s=0;s<N;++s)m=max(m,msd.Load(int2(p.xy),s));return m;}"
   "Texture2D<float4> src:register(t0);Texture2D<float> rd:register(t1);float3 consume(float4 p:SV_Position):SV_Target{int3 q=int3(int2(p.xy),0);return src.Load(q).rgb*(.5+rd.Load(q));}";
  Ptr<ID3DBlob> a,b;a.p=compile(hlsl,"depthmax","ps_5_0",macros);b.p=compile(hlsl,"consume","ps_5_0",macros);CHECK(SUCCEEDED(d->CreatePixelShader(a->GetBufferPointer(),a->GetBufferSize(),nullptr,depth_max_ps.out())));CHECK(SUCCEEDED(d->CreatePixelShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,consume_depth.out())));
  D3D11_DEPTH_STENCIL_DESC ds={};ds.DepthEnable=TRUE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ds.DepthFunc=D3D11_COMPARISON_ALWAYS;CHECK(SUCCEEDED(d->CreateDepthStencilState(&ds,depth_always.out())));
 }
 unsigned size;
 Post(ID3D11Device *d,LONG(WINAPI *probe)(UINT,void*),depth_foveation_params &p):size(p.size){
  std::vector<float> xy(2*size);p.axis_lut=(uint64_t)(uintptr_t)xy.data();CHECK(probe(DEPTH_AXIS_LUT,&p)==0&&!p.status);p.axis_lut=0;
  D3D11_BUFFER_DESC bd={};bd.ByteWidth=xy.size()*sizeof(float);bd.BindFlags=D3D11_BIND_SHADER_RESOURCE;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.StructureByteStride=4;D3D11_SUBRESOURCE_DATA data={xy.data(),0,0};CHECK(SUCCEEDED(d->CreateBuffer(&bd,&data,axis.out())));CHECK(SUCCEEDED(d->CreateShaderResourceView(axis,nullptr,axis_srv.out())));
  D3D11_TEXTURE2D_DESC td={};td.Width=td.Height=size;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R11G11B10_FLOAT;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
  CHECK(SUCCEEDED(d->CreateTexture2D(&td,nullptr,resolved.out())));CHECK(SUCCEEDED(d->CreateShaderResourceView(resolved,nullptr,resolved_srv.out())));
  CHECK(SUCCEEDED(d->CreateTexture2D(&td,nullptr,reconstructed.out())));CHECK(SUCCEEDED(d->CreateRenderTargetView(reconstructed,nullptr,reconstruct_rtv.out())));CHECK(SUCCEEDED(d->CreateShaderResourceView(reconstructed,nullptr,reconstructed_srv.out())));CHECK(SUCCEEDED(d->CreateTexture2D(&td,nullptr,final.out())));
  td.SampleDesc.Count=p.samples;CHECK(SUCCEEDED(d->CreateTexture2D(&td,nullptr,consumer_target.color.out())));CHECK(SUCCEEDED(d->CreateRenderTargetView(consumer_target.color,nullptr,consumer_target.rtv.out())));CHECK(SUCCEEDED(d->CreateShaderResourceView(consumer_target.color,nullptr,consumer_target.csrv.out())));
  const char *hlsl="Texture2D<float4> src:register(t0);StructuredBuffer<float> xy:register(t1);SamplerState smp:register(s0);"
   "float3 unwarp(float4 p:SV_Position):SV_Target{uint w,h;src.GetDimensions(w,h);uint2 i=uint2(p.xy);return src.SampleLevel(smp,float2(xy[i.x],xy[w+i.y])/float2(w,h),0).rgb;}"
   "float3 consume(float4 p:SV_Position):SV_Target{return src.Load(int3(int2(p.xy),0)).rgb;}"
   "float3 present(float4 p:SV_Position):SV_Target{uint w,h;src.GetDimensions(w,h);return src.SampleLevel(smp,p.xy/float2(w,h),0).rgb;}";
  Ptr<ID3DBlob> a,b;a.p=compile(hlsl,"unwarp","ps_5_0");b.p=compile(hlsl,"consume","ps_5_0");CHECK(SUCCEEDED(d->CreatePixelShader(a->GetBufferPointer(),a->GetBufferSize(),nullptr,unwarp.out())));CHECK(SUCCEEDED(d->CreatePixelShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,consume.out())));
  D3D11_SAMPLER_DESC sd={};sd.Filter=D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;CHECK(SUCCEEDED(d->CreateSamplerState(&sd,sampler.out())));
  Ptr<ID3DBlob> e;e.p=compile(hlsl,"present","ps_5_0");CHECK(SUCCEEDED(d->CreatePixelShader(e->GetBufferPointer(),e->GetBufferSize(),nullptr,present_ps.out())));
  CHECK(SUCCEEDED(d->CreateShaderResourceView(final,nullptr,final_srv.out())));
 }
 // Stay-warped: one ordinary bilinear read of the still-mapped final image (DXMT translates it).
 void present(ID3D11DeviceContext *c,hdr_stencil::Scene &scene){
  c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c->VSSetShader(scene.fullscreen,nullptr,0);c->OMSetBlendState(nullptr,nullptr,0xffffffff);c->OMSetDepthStencilState(nullptr,0);
  ID3D11RenderTargetView *rt=reconstruct_rtv;ID3D11ShaderResourceView *v=final_srv;ID3D11SamplerState *sm=sampler;c->OMSetRenderTargets(1,&rt,nullptr);c->PSSetShader(present_ps,nullptr,0);c->PSSetShaderResources(0,1,&v);c->PSSetSamplers(0,1,&sm);c->Draw(3,0);
  v=nullptr;c->PSSetShaderResources(0,1,&v);c->OMSetRenderTargets(0,nullptr,nullptr);
 }
 void finish(ID3D11DeviceContext *c,hdr_stencil::Scene &scene,hdr_stencil::Surface &ordinary,hdr_stencil::Surface &source,bool mapped){
  c->OMSetRenderTargets(0,nullptr,nullptr);c->ResolveSubresource(resolved,0,source.t.color,0,DXGI_FORMAT_R11G11B10_FLOAT);
  c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c->VSSetShader(scene.fullscreen,nullptr,0);c->OMSetBlendState(nullptr,nullptr,0xffffffff);
  ID3D11ShaderResourceView *v[2]={resolved_srv,axis_srv};ID3D11RenderTargetView *rt=reconstruct_rtv;ID3D11SamplerState *sm=sampler;
  if(mapped){
   c->OMSetRenderTargets(1,&rt,nullptr);c->PSSetShader(unwarp,nullptr,0);c->PSSetShaderResources(0,2,v);c->PSSetSamplers(0,1,&sm);c->Draw(3,0);
   v[0]=v[1]=nullptr;c->PSSetShaderResources(0,2,v);c->OMSetRenderTargets(0,nullptr,nullptr);
  }
  if(depth_max_ps){
   ID3D11ShaderResourceView *msd=source.t.dsrv;c->OMSetRenderTargets(0,nullptr,depth_max_dsv);c->OMSetDepthStencilState(depth_always,0);c->PSSetShader(depth_max_ps,nullptr,0);c->PSSetShaderResources(0,1,&msd);c->Draw(3,0);
   msd=nullptr;c->PSSetShaderResources(0,1,&msd);c->OMSetRenderTargets(0,nullptr,nullptr);
  }
  float clear[4]={};c->ClearRenderTargetView(consumer_target.rtv,clear);rt=consumer_target.rtv;c->OMSetRenderTargets(1,&rt,ordinary.read_only_depth);c->OMSetDepthStencilState(scene.consumer,0x81);
  v[0]=mapped?reconstructed_srv.p:resolved_srv.p;v[1]=depth_max_ps?depth_max_srv.p:nullptr;c->PSSetShader(depth_max_ps?consume_depth.p:consume.p,nullptr,0);c->PSSetShaderResources(0,2,v);c->Draw(3,0);v[0]=v[1]=nullptr;c->PSSetShaderResources(0,2,v);c->OMSetRenderTargets(0,nullptr,nullptr);
  c->ResolveSubresource(final,0,consumer_target.color,0,DXGI_FORMAT_R11G11B10_FLOAT);
 }
};
static std::vector<Pixel> read_final(ID3D11Device *d,ID3D11DeviceContext *c,ID3D11Texture2D *src,unsigned size){
 Ptr<ID3DBlob> code;code.p=compile("Texture2D<float4> src:register(t0);RWTexture2D<float4> dst:register(u0);[numthreads(8,8,1)]void main(uint3 i:SV_DispatchThreadID){dst[i.xy]=src.Load(int3(i.xy,0));}","main","cs_5_0");Ptr<ID3D11ComputeShader> cs;CHECK(SUCCEEDED(d->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,cs.out())));
 Ptr<ID3D11ShaderResourceView> srv;CHECK(SUCCEEDED(d->CreateShaderResourceView(src,nullptr,srv.out())));D3D11_TEXTURE2D_DESC td={};src->GetDesc(&td);td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_UNORDERED_ACCESS;Ptr<ID3D11Texture2D> out,stage;CHECK(SUCCEEDED(d->CreateTexture2D(&td,nullptr,out.out())));Ptr<ID3D11UnorderedAccessView> uav;CHECK(SUCCEEDED(d->CreateUnorderedAccessView(out,nullptr,uav.out())));td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;CHECK(SUCCEEDED(d->CreateTexture2D(&td,nullptr,stage.out())));
 ID3D11ShaderResourceView *v=srv;ID3D11UnorderedAccessView *u=uav;c->CSSetShader(cs,nullptr,0);c->CSSetShaderResources(0,1,&v);c->CSSetUnorderedAccessViews(0,1,&u,nullptr);c->Dispatch(size/8,size/8,1);v=nullptr;u=nullptr;c->CSSetShaderResources(0,1,&v);c->CSSetUnorderedAccessViews(0,1,&u,nullptr);c->CopyResource(stage,out);D3D11_MAPPED_SUBRESOURCE m={};CHECK(SUCCEEDED(c->Map(stage,0,D3D11_MAP_READ,0,&m)));std::vector<Pixel> result(size*size);for(unsigned y=0;y<size;++y)memcpy(result.data()+y*size,(char*)m.pData+y*m.RowPitch,size*sizeof(Pixel));c->Unmap(stage,0);return result;
}
static int run(ID3D11Device *d,ID3D11DeviceContext *c,IDXMTNativeContext1 *rate,LONG(WINAPI *probe)(UINT,void*),depth_foveation_params *p,unsigned size,unsigned n,float peripheral,bool reverse,bool source_materialized=false,bool stay_warped=false){
 CHECK(!stay_warped||source_materialized);
 CHECK(size%8==0);hdr_stencil::Surface ordinary,mapped;hdr_stencil::create(d,ordinary,size,n);hdr_stencil::create(d,mapped,size,n);hdr_stencil::Scene draw(d,n,128,true);Post post(d,probe,p[1]);if(stay_warped)post.enable_depth(d,n);
 std::unique_ptr<MSAASource> material;if(source_materialized)material.reset(new MSAASource(d,probe,p[1],ordinary));
 const unsigned modes=source_materialized?2:3;
 std::vector<Pixel> pre_load[2],first_read[2];unsigned alias_bad[2]={};
 D3D11_BLEND_DESC blend={};Ptr<ID3D11BlendState> no_color;CHECK(SUCCEEDED(d->CreateBlendState(&blend,no_color.out())));
 draw.draw(c,ordinary,2);auto reference=read_samples(d,c,ordinary.t,size,n);auto stencil=hdr_stencil::read_stencil(d,c,ordinary,size,n);if(material){material->read_source(c,draw);material->load(c,draw,ordinary);}post.finish(c,draw,ordinary,ordinary,false);auto final_reference=read_final(d,c,post.final,size);
 unsigned expected_hits=0;for(auto value:stencil)expected_hits+=(value&0x81)==0x81;CHECK(expected_hits>0);
 CHECK(SUCCEEDED(rate->SetRasterizationRateMap(p[1].map)));draw.draw(c,mapped,2);CHECK(SUCCEEDED(rate->SetRasterizationRateMap(0)));auto mapped_ref=read_samples(d,c,mapped.t,size,n);auto mapped_st=hdr_stencil::read_stencil(d,c,mapped,size,n);
 auto chain=[&](unsigned mode,bool inspect=false){
  if(stay_warped&&mode){
   // Every scene pass is rasterised once with the map; targets stay in mapped layout until the final read.
   c->OMSetBlendState(nullptr,nullptr,0xffffffff);CHECK(SUCCEEDED(rate->SetRasterizationRateMap(p[1].map)));
   draw.draw(c,mapped,0);draw.draw(c,mapped,1);material->read_source(c,draw,mapped.t.csrv);material->load(c,draw,mapped,true);post.finish(c,draw,mapped,mapped,false);
   CHECK(SUCCEEDED(rate->SetRasterizationRateMap(0)));post.present(c,draw);return;
  }
  CHECK(SUCCEEDED(rate->SetRasterizationRateMap(0)));c->OMSetBlendState(nullptr,nullptr,0xffffffff);draw.draw(c,ordinary,0);
  if(mode){CHECK(SUCCEEDED(rate->SetRasterizationRateMap(p[1].map)));draw.draw(c,mapped,0);draw.draw(c,mapped,1);CHECK(SUCCEEDED(rate->SetRasterizationRateMap(0)));
   c->OMSetBlendState(no_color,nullptr,0xffffffff);draw.draw(c,ordinary,1,false,false,mode==2);c->OMSetBlendState(nullptr,nullptr,0xffffffff);
  }else draw.draw(c,ordinary,1);
  if(material){
   if(mode)material->materialize(c,draw,mapped);
   if(inspect){
    pre_load[mode]=read_samples(d,c,ordinary.t,size,n);Target alias;alias.csrv.p=material->alias_srv;alias.csrv->AddRef();alias.dsrv.p=ordinary.t.dsrv;alias.dsrv->AddRef();auto a=read_samples(d,c,alias,size,n);alias_bad[mode]=0;for(size_t i=0;i<a.size();++i)alias_bad[mode]+=memcmp(&a[i],&pre_load[mode][i],sizeof(Pixel))!=0;
   }
   material->read_source(c,draw);if(inspect)first_read[mode]=read_final(d,c,material->snapshot,size);
   material->load(c,draw,ordinary);post.finish(c,draw,ordinary,ordinary,false);
  }else post.finish(c,draw,ordinary,mode?mapped:ordinary,mode!=0);
 };
 std::vector<Pixel> checked[3];
 auto check=[&](unsigned mode,unsigned after){
  if(stay_warped&&mode){
   checked[mode]=read_final(d,c,post.reconstructed,size);unsigned final_bad=0,center_bad=0;float peak=0;double mae=0;
   std::vector<unsigned> preview(size*size),difference(size*size,0xff000000u);
   for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x){size_t i=y*size+x;auto a=checked[mode][i],b=final_reference[i];float aa[]={a.r,a.g,a.b},bb[]={b.r,b.g,b.b};bool bad=false;unsigned packed=0xff000000u;
    for(unsigned k=0;k<3;++k){CHECK(std::isfinite(aa[k]));float e=fabs(aa[k]-bb[k]);mae+=e;bad|=e>std::max(.03f,.02f*fabsf(bb[k]));peak=std::max(peak,aa[k]);packed|=unsigned(lround(255*aa[k]/(1+aa[k])))<<(8*k);}
    final_bad+=bad;center_bad+=bad&&x>.44*size&&x<.56*size&&y>.44*size&&y<.56*size;preview[i]=packed;if(bad)difference[i]=0xff1040ffu;}
   if(after){char name[80];snprintf(name,sizeof(name),"stay-warped-r%03u",(unsigned)lround(peripheral*100));dump(name,preview,size,n);char mask_name[100];snprintf(mask_name,sizeof(mask_name),"%s-difference",name);dump(mask_name,difference,size,n);}
   printf("STAY_WARPED_CONTROL after=%u size=%u samples=%u rate=%.3f final_bad=%u center_bad=%u peak_hdr=%.6f mae=%.6f\n",after,size,n,peripheral,final_bad,center_bad,peak,mae/(size*size*3));fflush(stdout);
   CHECK(!center_bad&&peak>1);if(peripheral==1)CHECK(!final_bad);return;
  }
  auto ds=read_samples(d,c,ordinary.t,size,n);auto st=hdr_stencil::read_stencil(d,c,ordinary,size,n);checked[mode]=read_final(d,c,post.final,size);
  unsigned depth_bad=0,stencil_bad=0,layout_bad=0,final_bad=0,center_bad=0;float peak=0;double mae=0;
  for(size_t i=0;i<ds.size();++i){depth_bad+=ds[i].d!=reference[i].d;stencil_bad+=st[i]!=stencil[i];}
  if(mode){auto m=read_samples(d,c,mapped.t,size,n);auto ms=hdr_stencil::read_stencil(d,c,mapped,size,n);for(unsigned y=0;y<p[1].physical_height;++y)for(unsigned x=0;x<p[1].physical_width;++x)for(unsigned s=0;s<n;++s){size_t i=(y*size+x)*n+s;layout_bad+=memcmp(&m[i],&mapped_ref[i],sizeof(Pixel))||ms[i]!=mapped_st[i];}}
  unsigned source_bad=0,snapshot_bad=0,load_bad=0,modified=0,preserved=0;
  if(material){
   CHECK(!alias_bad[mode]);D3D11_TEXTURE2D_DESC desc={};ordinary.t.color->GetDesc(&desc);CHECK(desc.Width==size&&desc.Height==size&&desc.SampleDesc.Count==n&&desc.Format==DXGI_FORMAT_R11G11B10_FLOAT&&desc.MipLevels==1&&desc.ArraySize==1);
   for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x){size_t pixel=y*size+x;float avg[3]={};
    for(unsigned sample=0;sample<n;++sample){size_t i=pixel*n+sample;auto a=pre_load[mode][i],expected=mode?material->expected(mapped_ref,x,y,sample):reference[i];source_bad+=!MSAASource::close_colour(a.r,expected.r)||!MSAASource::close_colour(a.g,expected.g)||!MSAASource::close_colour(a.b,expected.b);
     if(!mode||peripheral==1)CHECK(a.r==reference[i].r&&a.g==reference[i].g&&a.b==reference[i].b);
     avg[0]+=a.r/n;avg[1]+=a.g/n;avg[2]+=a.b/n;
    }
    auto snap=first_read[mode][pixel];snapshot_bad+=!MSAASource::close_colour(snap.r,avg[0])||!MSAASource::close_colour(snap.g,avg[1])||!MSAASource::close_colour(snap.b,avg[2]);
    for(unsigned sample=0;sample<n;++sample){size_t i=pixel*n+sample;auto a=pre_load[mode][i],b=ds[i];if(material->patch(x,y,stencil[i])){
      load_bad+=!MSAASource::close_colour(b.r,a.r+.125f*snap.r)||!MSAASource::close_colour(b.g,a.g+.125f*snap.g)||!MSAASource::close_colour(b.b,a.b+.125f*snap.b);modified+=a.r!=b.r||a.g!=b.g||a.b!=b.b;
     }else{load_bad+=memcmp(&a,&b,sizeof(Pixel))!=0;++preserved;}
    }
   }
   CHECK(!source_bad&&!snapshot_bad&&!load_bad&&modified&&preserved);
   printf("MSAA_SOURCE_CONTROL mode=%u after=%u source_bad=%u alias_bad=%u snapshot_bad=%u load_bad=%u modified_samples=%u preserved_samples=%u\n",mode,after,source_bad,alias_bad[mode],snapshot_bad,load_bad,modified,preserved);fflush(stdout);
  }
  for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x){size_t i=y*size+x;auto a=checked[mode][i],b=final_reference[i];float aa[]={a.r,a.g,a.b},bb[]={b.r,b.g,b.b};bool bad=false;for(unsigned k=0;k<3;++k){CHECK(std::isfinite(aa[k]));float e=fabs(aa[k]-bb[k]);mae+=e;bad|=e>std::max(.03f,.02f*fabsf(bb[k]));peak=std::max(peak,aa[k]);}final_bad+=bad;center_bad+=bad&&x>.44*size&&x<.56*size&&y>.44*size&&y<.56*size;if(!mode||peripheral==1)CHECK(!memcmp(&a,&b,sizeof(Pixel)));}
  if(after && mode<2){
   std::vector<unsigned> preview(size*size),difference(size*size,0xff000000u);
   for(size_t i=0;i<checked[mode].size();++i){auto a=checked[mode][i],b=final_reference[i];float aa[]={a.r,a.g,a.b},bb[]={b.r,b.g,b.b};unsigned packed=0xff000000u;bool bad=false;
    for(unsigned k=0;k<3;++k){packed|=unsigned(lround(255*aa[k]/(1+aa[k])))<<(8*k);bad|=fabsf(aa[k]-bb[k])>std::max(.03f,.02f*fabsf(bb[k]));}
    preview[i]=packed;if(bad)difference[i]=0xff1040ffu;
   }
   char name[80];if(mode)snprintf(name,sizeof(name),source_materialized?"msaa-source-r%03u":"hdr-joined-r%03u",(unsigned)lround(peripheral*100));else snprintf(name,sizeof(name),source_materialized?"msaa-source-baseline":"hdr-joined-baseline");dump(name,preview,size,n);
   if(mode){char mask_name[100];snprintf(mask_name,sizeof(mask_name),"%s-difference",name);dump(mask_name,difference,size,n);}
  }
  // Read the actual timed consumer's MSAA output, not just the separate marker control.
  Target consumer_view;consumer_view.csrv.p=post.consumer_target.csrv;consumer_view.csrv->AddRef();consumer_view.dsrv.p=ordinary.t.dsrv;consumer_view.dsrv->AddRef();
  auto consumed=read_samples(d,c,consumer_view,size,n);unsigned timed_consumer_bad=0;
  for(size_t i=0;i<consumed.size();++i){bool visible=consumed[i].r+consumed[i].g+consumed[i].b>0;timed_consumer_bad+=visible!=((stencil[i]&0x81)==0x81);}CHECK(!timed_consumer_bad);
  // The following marker draw independently checks the exact ordinary stencil consumer.
  unsigned hits=draw.marker(d,c,ordinary,size,n);CHECK(hits==expected_hits);
  printf("HDR_JOINED_CONTROL mode=%u after=%u size=%u samples=%u rate=%.3f depth_bad=%u stencil_bad=%u mapped_bad=%u timed_consumer_bad=%u final_bad=%u center_bad=%u consumer_samples=%u expected_consumer_samples=%u peak_hdr=%.6f mae=%.6f\n",mode,after,size,n,peripheral,depth_bad,stencil_bad,layout_bad,timed_consumer_bad,final_bad,center_bad,hits,expected_hits,peak,mae/(size*size*3));fflush(stdout);CHECK(!depth_bad&&!stencil_bad&&!layout_bad&&peak>1);
 };
 for(unsigned m=0;m<modes;++m){chain(m,true);check(m,0);}
 constexpr unsigned rounds=65,warmup=24,measured=rounds-warmup;Ptr<ID3D11Query> dj,start[3][rounds],end[3][rounds];D3D11_QUERY_DESC q={D3D11_QUERY_TIMESTAMP_DISJOINT,0};CHECK(SUCCEEDED(d->CreateQuery(&q,dj.out())));q.Query=D3D11_QUERY_TIMESTAMP;
 for(unsigned m=0;m<modes;++m)for(unsigned i=0;i<rounds;++i){CHECK(SUCCEEDED(d->CreateQuery(&q,start[m][i].out())));CHECK(SUCCEEDED(d->CreateQuery(&q,end[m][i].out())));}
 double cpu[3][rounds]={};LARGE_INTEGER freq;QueryPerformanceFrequency(&freq);c->Begin(dj);
 for(unsigned i=0;i<rounds;++i)for(unsigned j=0;j<modes;++j){unsigned m=(i+j+unsigned(reverse))%modes;c->End(start[m][i]);LARGE_INTEGER a,b;QueryPerformanceCounter(&a);chain(m);QueryPerformanceCounter(&b);c->End(end[m][i]);cpu[m][i]=1000.*(b.QuadPart-a.QuadPart)/freq.QuadPart;}
 c->End(dj);D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint={};wait(c,dj,&disjoint,sizeof(disjoint));CHECK(!disjoint.Disjoint&&disjoint.Frequency);
 double gpu[3][measured];for(unsigned m=0;m<modes;++m)for(unsigned i=0;i<rounds;++i){UINT64 a,b;wait(c,start[m][i],&a,sizeof(a));wait(c,end[m][i],&b,sizeof(b));CHECK(b>a);double ms=1000.*(b-a)/disjoint.Frequency;printf("HDR_JOINED_SAMPLE mode=%u round=%u warmup=%u gpu_ms=%.6f cpu_record_ms=%.6f\n",m,i,i<warmup,ms,cpu[m][i]);if(i>=warmup)gpu[m][i-warmup]=ms;}
 for(unsigned m=0;m<modes;++m){std::sort(gpu[m],gpu[m]+measured);chain(m,true);check(m,1);}
 unsigned replay_difference=0;if(modes==3)for(size_t i=0;i<checked[1].size();++i)replay_difference+=memcmp(&checked[1][i],&checked[2][i],sizeof(Pixel))!=0;CHECK(!replay_difference);
 if(material){
  auto original=read_samples(d,c,ordinary.t,size,n);material->fullscreen(c,draw,material->collapse_rtv,material->read_ms,material->alias_srv,false);
  Target broadcast;broadcast.csrv.p=material->collapse_srv;broadcast.csrv->AddRef();broadcast.dsrv.p=ordinary.t.dsrv;broadcast.dsrv->AddRef();auto collapsed=read_samples(d,c,broadcast,size,n);unsigned bad=0,witness=0;
  for(size_t i=0;i<original.size();++i){bad+=!MSAASource::close_colour(original[i].r,collapsed[i].r)||!MSAASource::close_colour(original[i].g,collapsed[i].g)||!MSAASource::close_colour(original[i].b,collapsed[i].b);if(i%n)witness+=original[i].r!=original[i-i%n].r||original[i].g!=original[i-i%n].g||original[i].b!=original[i-i%n].b;}
  CHECK(bad&&witness);printf("MSAA_SOURCE_NEGATIVE broadcast_bad=%u plane_witness=%u\n",bad,witness);fflush(stdout);
 }
 if(stay_warped)printf("STAY_WARPED size=%u samples=%u rate=%.3f reverse=%u baseline_gpu_ms=%.6f stay_warped_gpu_ms=%.6f measured=%u\n",size,n,peripheral,reverse,gpu[0][measured/2],gpu[1][measured/2],measured);
 else if(material)printf("HDR_JOINED size=%u samples=%u rate=%.3f reverse=%u source_materialized=1 baseline_gpu_ms=%.6f full_replay_gpu_ms=%.6f replay_output_bad=0 measured=%u\n",size,n,peripheral,reverse,gpu[0][measured/2],gpu[1][measured/2],measured);
 else printf("HDR_JOINED size=%u samples=%u rate=%.3f reverse=%u baseline_gpu_ms=%.6f full_replay_gpu_ms=%.6f coverage_replay_gpu_ms=%.6f replay_output_bad=%u measured=%u\n",size,n,peripheral,reverse,gpu[0][measured/2],gpu[1][measured/2],gpu[2][measured/2],replay_difference,measured);fflush(stdout);
 c->ClearState();c->Flush();for(unsigned i=0;i<2;++i)CHECK(probe(DEPTH_RELEASE,&p[i])==0);return 0;
}
}

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Included by the existing depth fixture. Prepared resources; no waits inside a chain.
struct JoinedScene {
 Ptr<ID3D11Buffer> vertices;Ptr<ID3D11VertexShader> vs;Ptr<ID3D11PixelShader> ps;
 Ptr<ID3D11InputLayout> layout;Ptr<ID3D11DepthStencilState> write,readonly;
 unsigned count;
 JoinedScene(ID3D11Device *d,unsigned size){
  auto data=scene(size,true);count=data.size();D3D11_BUFFER_DESC bd={};bd.ByteWidth=count*sizeof(Vertex);bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;D3D11_SUBRESOURCE_DATA init={data.data(),0,0};CHECK(SUCCEEDED(d->CreateBuffer(&bd,&init,vertices.out())));
  D3D_SHADER_MACRO macros[]={{"REVERSE_Z","1"},{nullptr,nullptr}};Ptr<ID3DBlob> v,p;v.p=compile(scene_hlsl,"vs","vs_5_0",macros);p.p=compile(scene_hlsl,"ps","ps_5_0",macros);
  CHECK(SUCCEEDED(d->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,vs.out())));CHECK(SUCCEEDED(d->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,ps.out())));
  D3D11_INPUT_ELEMENT_DESC e[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"COLOR",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},{"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,24,D3D11_INPUT_PER_VERTEX_DATA,0}};
  CHECK(SUCCEEDED(d->CreateInputLayout(e,3,v->GetBufferPointer(),v->GetBufferSize(),layout.out())));
  D3D11_DEPTH_STENCIL_DESC ds={};ds.DepthEnable=TRUE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ds.DepthFunc=D3D11_COMPARISON_GREATER_EQUAL;
  CHECK(SUCCEEDED(d->CreateDepthStencilState(&ds,write.out())));ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;CHECK(SUCCEEDED(d->CreateDepthStencilState(&ds,readonly.out())));
 }
 void draw(ID3D11DeviceContext *c,Target &t,bool depth_only){
  UINT stride=sizeof(Vertex),offset=0;ID3D11Buffer *b=vertices;c->IASetVertexBuffers(0,1,&b,&stride,&offset);c->IASetInputLayout(layout);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  c->VSSetShader(vs,nullptr,0);c->PSSetShader(depth_only?nullptr:ps.p,nullptr,0);c->OMSetDepthStencilState(depth_only?write.p:readonly.p,0);
  ID3D11RenderTargetView *rtv=t.rtv;c->OMSetRenderTargets(depth_only?0:1,depth_only?nullptr:&rtv,t.dsv);
  if(depth_only)c->ClearDepthStencilView(t.dsv,D3D11_CLEAR_DEPTH,0,0);else{float clear[4]={0,0,0,1};c->ClearRenderTargetView(t.rtv,clear);}
  c->Draw(count,0);
 }
};
struct JoinedPost {
 Ptr<ID3D11Texture2D> resolved[2],reconstructed,output[2];
 Ptr<ID3D11ShaderResourceView> resolved_srv[2],reconstructed_srv,axis_srv;
 Ptr<ID3D11UnorderedAccessView> reconstructed_uav,output_uav[2];
 Ptr<ID3D11Buffer> axis;Ptr<ID3D11ComputeShader> unwarp,consume;Ptr<ID3D11SamplerState> sampler;
 unsigned size;
 JoinedPost(ID3D11Device *d,LONG(WINAPI *probe)(UINT,void*),depth_foveation_params &p):size(p.size){
  std::vector<float> xy(2*size);p.axis_lut=(uint64_t)(uintptr_t)xy.data();CHECK(probe(DEPTH_AXIS_LUT,&p)==0&&!p.status);p.axis_lut=0;
  D3D11_BUFFER_DESC bd={};bd.ByteWidth=xy.size()*sizeof(float);bd.BindFlags=D3D11_BIND_SHADER_RESOURCE;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.StructureByteStride=sizeof(float);D3D11_SUBRESOURCE_DATA data={xy.data(),0,0};CHECK(SUCCEEDED(d->CreateBuffer(&bd,&data,axis.out())));
  CHECK(SUCCEEDED(d->CreateShaderResourceView(axis,nullptr,axis_srv.out())));
  D3D11_TEXTURE2D_DESC td={};td.Width=td.Height=size;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
  for(unsigned i=0;i<2;++i){td.BindFlags=D3D11_BIND_SHADER_RESOURCE;CHECK(SUCCEEDED(d->CreateTexture2D(&td,nullptr,resolved[i].out())));CHECK(SUCCEEDED(d->CreateShaderResourceView(resolved[i],nullptr,resolved_srv[i].out())));
   td.BindFlags=D3D11_BIND_UNORDERED_ACCESS;CHECK(SUCCEEDED(d->CreateTexture2D(&td,nullptr,output[i].out())));CHECK(SUCCEEDED(d->CreateUnorderedAccessView(output[i],nullptr,output_uav[i].out())));}
  td.BindFlags=D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE;CHECK(SUCCEEDED(d->CreateTexture2D(&td,nullptr,reconstructed.out())));CHECK(SUCCEEDED(d->CreateShaderResourceView(reconstructed,nullptr,reconstructed_srv.out())));CHECK(SUCCEEDED(d->CreateUnorderedAccessView(reconstructed,nullptr,reconstructed_uav.out())));
  const char *code="Texture2D<float4> src:register(t0);StructuredBuffer<float> xy:register(t1);SamplerState samp:register(s0);RWTexture2D<float4> dst:register(u0);"
   "[numthreads(8,8,1)]void unwarp(uint3 i:SV_DispatchThreadID){uint w,h;src.GetDimensions(w,h);if(i.x>=w||i.y>=h)return;dst[i.xy]=src.SampleLevel(samp,float2(xy[i.x],xy[w+i.y])/float2(w,h),0);}"
   "[numthreads(8,8,1)]void consume(uint3 i:SV_DispatchThreadID){uint w,h;src.GetDimensions(w,h);if(i.x<w&&i.y<h)dst[i.xy]=src.Load(int3(i.xy,0));}";
  Ptr<ID3DBlob> u,k;u.p=compile(code,"unwarp","cs_5_0");k.p=compile(code,"consume","cs_5_0");CHECK(SUCCEEDED(d->CreateComputeShader(u->GetBufferPointer(),u->GetBufferSize(),nullptr,unwarp.out())));CHECK(SUCCEEDED(d->CreateComputeShader(k->GetBufferPointer(),k->GetBufferSize(),nullptr,consume.out())));
  D3D11_SAMPLER_DESC sd={};sd.Filter=D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;CHECK(SUCCEEDED(d->CreateSamplerState(&sd,sampler.out())));
 }
 void dispatch(ID3D11DeviceContext *c,ID3D11ComputeShader *shader,ID3D11ShaderResourceView *src,ID3D11UnorderedAccessView *dst,bool axes){
  ID3D11ShaderResourceView *views[]={src,axes?axis_srv.p:nullptr};ID3D11SamplerState *s=sampler;
  c->CSSetShader(shader,nullptr,0);c->CSSetShaderResources(0,2,views);c->CSSetUnorderedAccessViews(0,1,&dst,nullptr);c->CSSetSamplers(0,1,&s);c->Dispatch((size+7)/8,(size+7)/8,1);
  ID3D11ShaderResourceView *none[2]={};ID3D11UnorderedAccessView *null_uav=nullptr;c->CSSetShaderResources(0,2,none);c->CSSetUnorderedAccessViews(0,1,&null_uav,nullptr);
 }
 void run(ID3D11DeviceContext *c,Target &t,unsigned mode){
  c->OMSetRenderTargets(0,nullptr,nullptr);c->ResolveSubresource(resolved[mode],0,t.color,0,DXGI_FORMAT_R8G8B8A8_UNORM);
  if(mode)dispatch(c,unwarp,resolved_srv[mode],reconstructed_uav,true);
  dispatch(c,consume,mode?reconstructed_srv.p:resolved_srv[0].p,output_uav[mode],false);
 }
};
static std::vector<unsigned> joined_read(ID3D11Device *d,ID3D11DeviceContext *c,ID3D11Texture2D *src,unsigned size){
 D3D11_TEXTURE2D_DESC td={};src->GetDesc(&td);td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Ptr<ID3D11Texture2D> staging;CHECK(SUCCEEDED(d->CreateTexture2D(&td,nullptr,staging.out())));c->CopyResource(staging,src);
 D3D11_MAPPED_SUBRESOURCE map={};CHECK(SUCCEEDED(c->Map(staging,0,D3D11_MAP_READ,0,&map)));std::vector<unsigned> pixels(size*size);for(unsigned y=0;y<size;++y)memcpy(pixels.data()+y*size,(char*)map.pData+y*map.RowPitch,size*4);c->Unmap(staging,0);return pixels;
}
static int joined_prepass_experiment(ID3D11Device *dev,ID3D11DeviceContext *ctx,IDXMTNativeDevice2 *native,IDXMTNativeContext1 *rate,
 LONG(WINAPI *probe)(UINT,void*),Target *targets,depth_foveation_params *p,unsigned size,unsigned samples,float peripheral_rate,bool reverse){
 // Independent one-pass references before timing; no compilation/allocation in a chain.
 draw_scene(dev,ctx,targets[0],size,false,false,true);auto depth_reference=read_samples(dev,ctx,targets[0],size,samples);double ignored=0;auto ordinary=overlay_resolve(dev,ctx,targets[0],size,ignored,false);
 CHECK(SUCCEEDED(rate->SetRasterizationRateMap(p[1].map)));draw_scene(dev,ctx,targets[1],size,false,false,true);CHECK(SUCCEEDED(rate->SetRasterizationRateMap(0)));auto mapped_reference=read_samples(dev,ctx,targets[1],size,samples);
 JoinedScene draw(dev,size);JoinedPost post(dev,probe,p[1]);
 auto chain=[&](unsigned mode){
  CHECK(SUCCEEDED(rate->SetRasterizationRateMap(0)));draw.draw(ctx,targets[0],true);
  if(mode){CHECK(SUCCEEDED(rate->SetRasterizationRateMap(p[1].map)));draw.draw(ctx,targets[1],true);draw.draw(ctx,targets[1],false);}
  else draw.draw(ctx,targets[0],false);
  CHECK(SUCCEEDED(rate->SetRasterizationRateMap(0)));post.run(ctx,targets[mode],mode);
 };
 // Correctness first; then rerun after measurement to detect reuse/state problems.
 auto check=[&](bool images){
  auto base=joined_read(dev,ctx,post.output[0],size),out=joined_read(dev,ctx,post.output[1],size);
  auto depth=read_samples(dev,ctx,targets[0],size,samples),mapped=read_samples(dev,ctx,targets[1],size,samples);
  unsigned baseline_bad=0,depth_bad=0,mapped_bad=0,color_bad=0,center_bad=0;double error=0;
  for(size_t i=0;i<depth.size();++i)depth_bad+=depth[i].d!=depth_reference[i].d;
  for(unsigned y=0;y<p[1].physical_height;++y)for(unsigned x=0;x<p[1].physical_width;++x)for(unsigned s=0;s<samples;++s){size_t i=(y*size+x)*samples+s;auto a=mapped[i],b=mapped_reference[i];mapped_bad+=fabs(a.r-b.r)>3./255||fabs(a.g-b.g)>3./255||fabs(a.b-b.b)>3./255||a.d!=b.d;}
  for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x){size_t i=y*size+x;baseline_bad+=base[i]!=ordinary[i];bool bad=false;for(unsigned k=0;k<3;++k){unsigned d=abs(int((out[i]>>(8*k))&255)-int((ordinary[i]>>(8*k))&255));bad|=d>3;error+=d;}color_bad+=bad;center_bad+=bad&&x>.44*size&&x<.56*size&&y>.44*size&&y<.56*size;}
  printf("JOINED_CONTROL size=%u samples=%u rate=%.3f after=%u baseline_bad=%u depth_bad=%u mapped_bad=%u color_bad=%u center_bad=%u mae=%.6f\n",size,samples,peripheral_rate,images,baseline_bad,depth_bad,mapped_bad,color_bad,center_bad,error/(size*size*3));fflush(stdout);
  CHECK(!baseline_bad&&!depth_bad&&!mapped_bad);if(peripheral_rate==1)CHECK(!color_bad);CHECK(!center_bad);
  if(images){dump("joined-baseline",base,size,samples);char name[80];snprintf(name,sizeof(name),"joined-r%03u",(unsigned)lround(peripheral_rate*100));dump(name,out,size,samples);}
 };
 chain(0);chain(1);check(false);
 constexpr unsigned rounds=65,warmup=24,measured=rounds-warmup;Ptr<ID3D11Query> dj,start[2][rounds],end[2][rounds];D3D11_QUERY_DESC q={D3D11_QUERY_TIMESTAMP_DISJOINT,0};CHECK(SUCCEEDED(dev->CreateQuery(&q,dj.out())));q.Query=D3D11_QUERY_TIMESTAMP;
 for(unsigned m=0;m<2;++m)for(unsigned i=0;i<rounds;++i){CHECK(SUCCEEDED(dev->CreateQuery(&q,start[m][i].out())));CHECK(SUCCEEDED(dev->CreateQuery(&q,end[m][i].out())));}
 double cpu[2][rounds]={};LARGE_INTEGER frequency;QueryPerformanceFrequency(&frequency);ctx->Begin(dj);
 for(unsigned i=0;i<rounds;++i)for(unsigned j=0;j<2;++j){unsigned m=(i+j+unsigned(reverse))%2;ctx->End(start[m][i]);LARGE_INTEGER a,b;QueryPerformanceCounter(&a);chain(m);QueryPerformanceCounter(&b);ctx->End(end[m][i]);cpu[m][i]=1000.*(b.QuadPart-a.QuadPart)/frequency.QuadPart;}
 ctx->End(dj);D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint={};wait(ctx,dj,&disjoint,sizeof(disjoint));CHECK(!disjoint.Disjoint&&disjoint.Frequency);
 double gpu[2][measured],record[2][measured];for(unsigned m=0;m<2;++m)for(unsigned i=0;i<rounds;++i){UINT64 a,b;wait(ctx,start[m][i],&a,sizeof(a));wait(ctx,end[m][i],&b,sizeof(b));CHECK(b>a);double ms=1000.*(b-a)/disjoint.Frequency;
  printf("JOINED_SAMPLE mode=%u round=%u warmup=%u gpu_ms=%.6f cpu_record_ms=%.6f\n",m,i,i<warmup,ms,cpu[m][i]);if(i>=warmup){gpu[m][i-warmup]=ms;record[m][i-warmup]=cpu[m][i];}}
 for(unsigned m=0;m<2;++m){std::sort(gpu[m],gpu[m]+measured);std::sort(record[m],record[m]+measured);}
 check(true);
 // Independent native resolve+unwarp reference, outside the timed sequences.
 CHECK(probe(DEPTH_RESOLVE_FIRST,&p[1])==0&&!p[1].status);auto native_image=read_single(dev,ctx,native,p[1].output_color,size);auto output=joined_read(dev,ctx,post.output[1],size);unsigned native_bad=0;
 for(size_t i=0;i<output.size();++i)for(unsigned c=0;c<3;++c)if(abs(int((output[i]>>(8*c))&255)-int((native_image[i]>>(8*c))&255))>1){++native_bad;break;}CHECK(!native_bad);
 printf("JOINED_PREPASS size=%u samples=%u rate=%.3f reverse=%u physical_width=%u physical_height=%u baseline_gpu_ms=%.6f foveated_gpu_ms=%.6f saving_gpu_ms=%.6f baseline_cpu_record_ms=%.6f foveated_cpu_record_ms=%.6f native_reference_bad=%u measured=%u\n",size,samples,peripheral_rate,reverse,p[1].physical_width,p[1].physical_height,gpu[0][measured/2],gpu[1][measured/2],gpu[0][measured/2]-gpu[1][measured/2],record[0][measured/2],record[1][measured/2],native_bad,measured);fflush(stdout);
 ctx->ClearState();ctx->Flush();for(unsigned i=0;i<2;++i)CHECK(probe(DEPTH_RELEASE,&p[i])==0);
 printf("PASS joined sequence, exact depth and mapped visibility controls; reduced-rate final colour is approximate.\n");return 0;
}

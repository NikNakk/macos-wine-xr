// Copyright 2026 Nick Kennedy; SPDX-License-Identifier: LGPL-2.1-or-later
#include "dxmt_replay_fixture.h"
#include <algorithm>
struct PassScene : Scene {
 Ptr<ID3D11Buffer> ib;Ptr<ID3D11PixelShader> coverage,heavy;Ptr<ID3D11BlendState> blend;Ptr<ID3D11DepthStencilState> lequal;
 PassScene(ID3D11Device*d,UINT n):Scene(d,n){
  UINT16 indices[]={999,888,1,2,3};D3D11_BUFFER_DESC b={};b.ByteWidth=sizeof(indices);b.Usage=D3D11_USAGE_DYNAMIC;b.BindFlags=D3D11_BIND_INDEX_BUFFER;b.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;D3D11_SUBRESOURCE_DATA data={indices,0,0};CHECK(SUCCEEDED(d->CreateBuffer(&b,&data,ib.out())));
  const char *shader="cbuffer C:register(b0){float4 tint;}Texture2D<float4> tex:register(t0);struct O{float4 colour:SV_Target;uint mask:SV_Coverage;};O main(float4 p:SV_Position){O o;float4 t=tex.Load(int3(p.xy,0));if(((uint(p.x)/7+uint(p.y)/9)%3)==0)discard;float a=t.x+t.y;[loop]for(uint k=0;k<128;++k)a=sin(a+float(k)*.01);o.colour=t*tint+float4(a*1e-6,0,0,0);o.mask=(uint(p.x)%5)==0?1:3;return o;}float4 plain(float4 p:SV_Position):SV_Target{return main(p).colour;}";
  Ptr<ID3DBlob> code;code.p=compile(shader,"main","ps_5_0");CHECK(SUCCEEDED(d->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,coverage.out())));code.reset();code.p=compile(shader,"plain","ps_5_0");CHECK(SUCCEEDED(d->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,heavy.out())));
  D3D11_BLEND_DESC bd={};auto&r=bd.RenderTarget[0];r.BlendEnable=TRUE;r.SrcBlend=r.DestBlend=D3D11_BLEND_ONE;r.BlendOp=D3D11_BLEND_OP_ADD;r.SrcBlendAlpha=D3D11_BLEND_ONE;r.DestBlendAlpha=D3D11_BLEND_ZERO;r.BlendOpAlpha=D3D11_BLEND_OP_ADD;r.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;CHECK(SUCCEEDED(d->CreateBlendState(&bd,blend.out())));
  D3D11_DEPTH_STENCIL_DESC dd={};ds->GetDesc(&dd);dd.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;CHECK(SUCCEEDED(d->CreateDepthStencilState(&dd,lequal.out())));
 }
 void prepare(ID3D11DeviceContext1*c,unsigned i,bool versions=true){
  if(versions){
   D3D11_MAPPED_SUBRESOURCE m={};CHECK(SUCCEEDED(c->Map(vb,0,D3D11_MAP_WRITE_DISCARD,0,&m)));float s=(float(i)-1.5f)*.12f,z=.65f-.12f*i;float v[]={-1+s,-1,z,1,-.83f+s,1,z,1,1+s,-.73f,z,1};memcpy(m.pData,v,sizeof(v));c->Unmap(vb,0);
   CHECK(SUCCEEDED(c->Map(cb,0,D3D11_MAP_WRITE_DISCARD,0,&m)));memset(m.pData,0,512);float *f=(float*)m.pData;f[64]=2.5f+.3f*i;f[65]=1.75f-.2f*i;f[66]=3-.1f*i;f[67]=1;c->Unmap(cb,0);
   CHECK(SUCCEEDED(c->Map(ib,0,D3D11_MAP_WRITE_DISCARD,0,&m)));UINT16 indices[]={UINT16(999-i),UINT16(888-i),1,2,3};memcpy(m.pData,indices,sizeof(indices));c->Unmap(ib,0);
   std::vector<UINT> tex(size*size);for(UINT y=0;y<size;++y)for(UINT x=0;x<size;++x)tex[y*size+x]=0xff000000|((x*2+i*17)&255)|(((y*2+i*11)&255)<<8)|(((x^y)+i*23)&255)<<16;c->UpdateSubresource(texture,0,nullptr,tex.data(),size*4,0);
  }
  c->PSSetShader(i==0?nullptr:i==2?heavy.p:coverage.p,nullptr,0);
  ID3D11ShaderResourceView *t=versions&&i==3?nullptr:srv.p;c->PSSetShaderResources(0,1,&t);
  ID3D11Buffer *constant=versions&&i==3?nullptr:cb.p;UINT first=16,num=16;c->PSSetConstantBuffers1(0,1,&constant,&first,&num);
  c->OMSetDepthStencilState(versions?ds.p:lequal.p,i==0?0x45:i==1?0x85:i==2?0xc5:0xe5);c->OMSetBlendState(i==2?blend.p:nullptr,nullptr,0xffffffff);
  D3D11_RECT rect={LONG(8+i*3),LONG(12+i*2),LONG(size-8-i*4),LONG(size-12-i*2)};c->RSSetScissorRects(1,&rect);
  D3D11_VIEWPORT vp={FLOAT(i==2?1:0),0,FLOAT(size-(i==2?2:0)),FLOAT(size),0,1};c->RSSetViewports(1,&vp);
  c->IASetIndexBuffer(ib,DXGI_FORMAT_R16_UINT,2);
 }
 void draw(ID3D11DeviceContext1*c,unsigned i){if(i%2)c->DrawIndexed(3,1,-1);else c->Draw(3,0);}
 void release_all(){release_inputs();ib.reset();coverage.reset();heavy.reset();blend.reset();lequal.reset();}
};
static void ordinary(PassScene&s,ID3D11DeviceContext1*c,bool versions=true){for(unsigned i=0;i<4;++i){s.prepare(c,i,versions);s.draw(c,i);}}
static void corrupt(ID3D11Device*d,ID3D11DeviceContext1*c,ID3D11Texture2D*col,ID3D11Texture2D*dep,UINT n){Ptr<ID3D11RenderTargetView>rt;CHECK(SUCCEEDED(d->CreateRenderTargetView(col,nullptr,rt.out())));D3D11_DEPTH_STENCIL_VIEW_DESC dd={};dd.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;dd.ViewDimension=n==1?D3D11_DSV_DIMENSION_TEXTURE2D:D3D11_DSV_DIMENSION_TEXTURE2DMS;Ptr<ID3D11DepthStencilView>ds;CHECK(SUCCEEDED(d->CreateDepthStencilView(dep,&dd,ds.out())));float bad[]={1,0,1,1};c->ClearRenderTargetView(rt,bad);c->ClearDepthStencilView(ds,D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.01,0xff);}
static void fault(ID3D11Device*d,ID3D11DeviceContext1*c,PassScene&s,PassScene&other,unsigned mode,UINT n){
 if(mode==0){float f[]={0,1,0,1};c->ClearRenderTargetView(s.rtv,f);}
 if(mode==1){Ptr<ID3D11RenderTargetView>alias;CHECK(SUCCEEDED(d->CreateRenderTargetView(s.colour,nullptr,alias.out())));ID3D11RenderTargetView*r=alias;c->OMSetRenderTargets(1,&r,s.dsv);}
 if(mode==2)c->CopyResource(other.colour,s.colour);
 if(mode==3){D3D11_QUERY_DESC desc={D3D11_QUERY_OCCLUSION,0};Ptr<ID3D11Query>q;CHECK(SUCCEEDED(d->CreateQuery(&desc,q.out())));c->Begin(q);c->End(q);}
 if(mode==4)c->DrawInstanced(3,1,0,0);
 if(mode==5){Ptr<ID3D11DeviceContext>def;CHECK(SUCCEEDED(d->CreateDeferredContext(0,def.out())));Ptr<ID3D11CommandList>list;CHECK(SUCCEEDED(def->FinishCommandList(FALSE,list.out())));c->ExecuteCommandList(list,TRUE);}
 if(mode==8)c->DiscardResource(s.colour);
 if(mode==9)c->Dispatch(1,1,1);
 if(mode==10)c->ClearDepthStencilView(s.dsv,D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.9,0x35);
 if(mode==11){D3D11_TEXTURE2D_DESC td={};s.colour->GetDesc(&td);td.SampleDesc.Count=1;Ptr<ID3D11Texture2D>out;CHECK(SUCCEEDED(d->CreateTexture2D(&td,nullptr,out.out())));if(n==2)c->ResolveSubresource(out,0,s.colour,0,colour_format);else c->CopySubresourceRegion(out,0,0,0,0,s.colour,0,nullptr);}
}
static bool cache_enabled(){char v[8]={};return GetEnvironmentVariableA("DXMT_REPLAY_CACHE_INPUTS",v,sizeof(v)) && !strcmp(v,"1");}
static double median(std::vector<double> v){std::sort(v.begin(),v.end());return v[v.size()/2];}
static void timing(ID3D11Device*d,ID3D11DeviceContext1*c,IDXMTReplayPassProbe*probe,UINT n){
 PassScene scene(d,n);scene.bind(c);Ptr<IDXMTReplayPacket>p;CHECK(probe->BeginPassCapture(64*1024*1024,4,p.out())==S_OK);ordinary(scene,c,false);CHECK(probe->EndPassCapture()==S_OK);Ptr<ID3D11CommandList>cached;Ptr<ID3D11Texture2D>out,dep;UINT64 bytes=0;CHECK(p->GetResult(cached.out(),out.out(),dep.out(),&bytes)==S_OK);p.reset();auto reference=read(d,c,scene.colour,scene.depth,n);
 Ptr<ID3D11Query>disjoint,a,b;D3D11_QUERY_DESC q={D3D11_QUERY_TIMESTAMP_DISJOINT,0};CHECK(SUCCEEDED(d->CreateQuery(&q,disjoint.out())));q.Query=D3D11_QUERY_TIMESTAMP;CHECK(SUCCEEDED(d->CreateQuery(&q,a.out())));CHECK(SUCCEEDED(d->CreateQuery(&q,b.out())));
 LARGE_INTEGER freq;QueryPerformanceFrequency(&freq);std::vector<double>ticks[3],cpu[3],paired;
 c->Begin(disjoint);
 for(unsigned iter=0;iter<25;++iter){double pair[3]={};for(unsigned k=0;k<3;++k){unsigned mode=(iter%2)?2-k:k;if(mode!=2)scene.bind(c);else c->ClearState();c->End(a);LARGE_INTEGER start,end;QueryPerformanceCounter(&start);Ptr<IDXMTReplayPacket>current;
  if(mode==1)CHECK(probe->BeginPassCapture(64*1024*1024,4,current.out())==S_OK);
  if(mode==2)c->ExecuteCommandList(cached,TRUE);else ordinary(scene,c,false);
  if(mode==1)CHECK(probe->EndPassCapture()==S_OK);
  QueryPerformanceCounter(&end);c->End(b);UINT64 x=0,y=0;ULONGLONG deadline=GetTickCount64()+30000;HRESULT hr;
  while((hr=c->GetData(b,&y,sizeof(y),0))==S_FALSE && GetTickCount64()<deadline)Sleep(1);CHECK(hr==S_OK);CHECK(c->GetData(a,&x,sizeof(x),0)==S_OK);CHECK(y>x);
  if(iter==0 || iter==24)CHECK(read(d,c,mode==2?out.p:scene.colour.p,mode==2?dep.p:scene.depth.p,n)==reference);
  if(iter>=8){ticks[mode].push_back(double(y-x));cpu[mode].push_back(1000.*(end.QuadPart-start.QuadPart)/freq.QuadPart);pair[mode]=double(y-x);}
 }if(iter>=8)paired.push_back(pair[1]-pair[0]);}
 c->End(disjoint);D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data={};ULONGLONG deadline=GetTickCount64()+30000;HRESULT hr;while((hr=c->GetData(disjoint,&data,sizeof(data),0))==S_FALSE&&GetTickCount64()<deadline)Sleep(1);CHECK(hr==S_OK&&!data.Disjoint&&data.Frequency);
 CHECK(read(d,c,scene.colour,scene.depth,n)==reference);CHECK(read(d,c,out,dep,n)==reference);
 printf("PASS_TIMING samples=%u cache=%u size=%u draws=4 measured_pairs=17 nominal_bytes=%llu baseline_gpu_ms=%.6f captured_original_gpu_ms=%.6f cached_replay_gpu_ms=%.6f paired_added_gpu_ms=%.6f baseline_cpu_ms=%.6f capture_cpu_ms=%.6f cached_replay_cpu_ms=%.6f correctness=1 PASS\n",n,cache_enabled(),size,(unsigned long long)bytes,median(ticks[0])*1000/data.Frequency,median(ticks[1])*1000/data.Frequency,median(ticks[2])*1000/data.Frequency,median(paired)*1000/data.Frequency,median(cpu[0]),median(cpu[1]),median(cpu[2]));
}
int main(int argc,char**argv){UINT n=argc>1?atoi(argv[1]):2;CHECK(n==1||n==2);colour_format=argc>2&&!strcmp(argv[2],"--hdr")?DXGI_FORMAT_R11G11B10_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM;bool bench=argc>3&&!strcmp(argv[3],"--timing");if(bench)size=768;
 Ptr<ID3D11Device>d;Ptr<ID3D11DeviceContext>base;CHECK(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,d.out(),nullptr,base.out())));Ptr<ID3D11DeviceContext1>c;CHECK(SUCCEEDED(base->QueryInterface(__uuidof(ID3D11DeviceContext1),(void**)c.out())));Ptr<IDXMTReplayPassProbe>probe;CHECK(SUCCEEDED(c->QueryInterface(DXMT_IID_REPLAY_PASS_PROBE,(void**)probe.out())));Ptr<ID3D11Multithread>mt;CHECK(SUCCEEDED(c->QueryInterface(__uuidof(ID3D11Multithread),(void**)mt.out())));mt->SetMultithreadProtected(TRUE);
 if(bench){timing(d,c,probe,n);return 0;}
 PassScene normal(d,n);normal.bind(c);ordinary(normal,c);auto expected=read(d,c,normal.colour,normal.depth,n);
 PassScene recorded(d,n);recorded.bind(c);Ptr<IDXMTReplayPacket>packet;CHECK(probe->BeginPassCapture(8*1024*1024,4,packet.out())==S_OK);ordinary(recorded,c);CHECK(packet->GetResult(nullptr,nullptr,nullptr,nullptr)==E_PENDING);CHECK(probe->EndPassCapture()==S_OK);
 Ptr<ID3D11CommandList>list;Ptr<ID3D11Texture2D>col,dep;UINT64 bytes=0;CHECK(packet->GetResult(list.out(),col.out(),dep.out(),&bytes)==S_OK);packet.reset();
 // No readback/flush between last input version capture and subsequent mutation.
 recorded.mutate(c);c->ClearState();recorded.release_all();
 unsigned comparisons=0;for(unsigned i=0;i<3;++i){corrupt(d,c,col,dep,n);normal.bind(c);c->ExecuteCommandList(list,TRUE);ordinary(normal,c);CHECK(read(d,c,normal.colour,normal.depth,n)==expected);CHECK(read(d,c,col,dep,n)==expected);comparisons+=size*size*n;}
 unsigned prepass_only=0,distinct=0,hdr=0;for(size_t i=0;i<expected.size();i+=8){prepass_only+=expected[i+5]==0x45;for(unsigned ch=0;ch<3;++ch){float v;memcpy(&v,&expected[i+ch],4);if(v>1){++hdr;break;}}}if(n==2){for(size_t i=0;i<expected.size();i+=16)distinct+=memcmp(expected.data()+i,expected.data()+i+8,32)!=0;CHECK(distinct>0);}CHECK(prepass_only>100);if(colour_format==DXGI_FORMAT_R11G11B10_FLOAT)CHECK(hdr>100);
 PassScene collapsed(d,n);collapsed.bind(c);collapsed.prepare(c,3);collapsed.draw(c,3);CHECK(read(d,c,collapsed.colour,collapsed.depth,n)!=expected);
 for(unsigned mode=0;mode<12;++mode){std::vector<UINT>ref;for(unsigned capture=0;capture<2;++capture){PassScene s(d,n),other(d,n);s.bind(c);Ptr<IDXMTReplayPacket>rejected;if(capture)CHECK(probe->BeginPassCapture(mode==7?1:8*1024*1024,mode==6?1:4,rejected.out())==S_OK);s.prepare(c,0);s.draw(c,0);fault(d,c,s,other,mode,n);for(unsigned i=1;i<4;++i){s.prepare(c,i);s.draw(c,i);}if(capture){CHECK(rejected->GetResult(nullptr,nullptr,nullptr,nullptr)==DXGI_ERROR_UNSUPPORTED);CHECK(probe->EndPassCapture()==DXGI_ERROR_INVALID_CALL);}auto image=read(d,c,s.colour,s.depth,n);if(!capture)ref=image;else CHECK(image==ref);}}
 Ptr<IDXMTReplayPacket>empty;CHECK(probe->BeginPassCapture(1024,1,empty.out())==S_OK);CHECK(probe->EndPassCapture()==DXGI_ERROR_UNSUPPORTED);CHECK(empty->GetResult(nullptr,nullptr,nullptr,nullptr)==DXGI_ERROR_UNSUPPORTED);
 // A forgotten scope must not retain the device through a public command-list
 // cycle. Keep only the packet after dropping all device/context owners.
 Ptr<IDXMTReplayPacket>abandoned;
 {
  Ptr<ID3D11Device>other_device;Ptr<ID3D11DeviceContext>other_base;
  CHECK(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,other_device.out(),nullptr,other_base.out())));
  Ptr<ID3D11DeviceContext1>other_context;CHECK(SUCCEEDED(other_base->QueryInterface(__uuidof(ID3D11DeviceContext1),(void**)other_context.out())));
  Ptr<IDXMTReplayPassProbe>other_probe;CHECK(SUCCEEDED(other_context->QueryInterface(DXMT_IID_REPLAY_PASS_PROBE,(void**)other_probe.out())));
  auto r0=other_device->AddRef();other_device->Release();fprintf(stderr,"ABANDON initial=%lu\n",r0-1);
  PassScene unfinished(other_device,n);unfinished.bind(other_context);CHECK(other_probe->BeginPassCapture(8*1024*1024,4,abandoned.out())==S_OK);unfinished.prepare(other_context,0);unfinished.draw(other_context,0);
auto r1=other_device->AddRef();other_device->Release();fprintf(stderr,"ABANDON captured=%lu\n",r1-1);
  D3D11_QUERY_DESC event_desc={D3D11_QUERY_EVENT,0};Ptr<ID3D11Query>drain;CHECK(SUCCEEDED(other_device->CreateQuery(&event_desc,drain.out())));other_context->End(drain);
  BOOL done=FALSE;ULONGLONG deadline=GetTickCount64()+30000;HRESULT waited;
  while((waited=other_context->GetData(drain,&done,sizeof(done),0))==S_FALSE && GetTickCount64()<deadline)Sleep(1);CHECK(waited==S_OK);drain.reset();
  other_context->ClearState();unfinished.release_all();unfinished.rtv.reset();unfinished.dsv.reset();unfinished.colour.reset();unfinished.depth.reset();
  other_probe.reset();other_context.reset();other_base.reset();
  auto remaining=other_device.p->Release();other_device.p=nullptr;
  fprintf(stderr,"ABANDON remaining_device_refs=%lu status=%08lx\n",remaining,(unsigned long)abandoned->GetResult(nullptr,nullptr,nullptr,nullptr));
 }
 CHECK(abandoned->GetResult(nullptr,nullptr,nullptr,nullptr)==DXGI_ERROR_UNSUPPORTED);
 printf("PASS_REPLAY samples=%u cache=%u hdr=%u draws=4 indexed_draws=2 prepass_samples=%u distinct_sample_pixels=%u above_one_samples=%u sample_comparisons=%u nominal_bytes=%llu input_versions=4 unbound_slots=1 seed_reset=1 caller_release=1 restored_draw=1 collapsed_negative=1 abandoned_scope=1 negative_cases=13 PASS\n",n,cache_enabled(),colour_format==DXGI_FORMAT_R11G11B10_FLOAT,prepass_only,distinct,hdr,comparisons,(unsigned long long)bytes);
 c->ClearState();c->Flush();return 0;
}

// Copyright 2026 Nick Kennedy; SPDX-License-Identifier: LGPL-2.1-or-later
#include "dxmt_replay_fixture.h"
int main(int argc,char **argv){
 bool disabled=argc>1&&!strcmp(argv[1],"--disabled");UINT n=argc>1&&!disabled?atoi(argv[1]):2;CHECK(n==1||n==2);if(argc>2&&!strcmp(argv[2],"--hdr"))colour_format=DXGI_FORMAT_R11G11B10_FLOAT;
 Ptr<ID3D11Device>d;Ptr<ID3D11DeviceContext> base;CHECK(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,d.out(),nullptr,base.out())));Ptr<ID3D11DeviceContext1> c;CHECK(SUCCEEDED(base->QueryInterface(__uuidof(ID3D11DeviceContext1),(void**)c.out())));Ptr<IDXMTReplayProbe> probe;CHECK(SUCCEEDED(c->QueryInterface(DXMT_IID_REPLAY_PROBE,(void**)probe.out())));
 Ptr<ID3D11Multithread> mt;CHECK(SUCCEEDED(c->QueryInterface(__uuidof(ID3D11Multithread),(void**)mt.out())));mt->SetMultithreadProtected(TRUE);
 Scene scene(d,n);scene.bind(c);Ptr<IDXMTReplayPacket> packet;
 HRESULT hr=probe->CaptureNextDraw(8*1024*1024,packet.out());
 if(disabled){CHECK(hr==DXGI_ERROR_UNSUPPORTED&&!packet.p);c->Draw(3,0);auto p=read(d,c,scene.colour,scene.depth,n);unsigned shaded=0;for(size_t i=0;i<p.size();i+=8)shaded+=p[i+5]==0x85;CHECK(shaded>0);printf("REPLAY disabled=1 normal_draw=1 shaded=%u PASS\n",shaded);return 0;}
 CHECK(hr==S_OK);CHECK(packet->GetResult(nullptr,nullptr,nullptr,nullptr)==E_PENDING);
 Ptr<IDXMTReplayPacket> duplicate;CHECK(probe->CaptureNextDraw(1024,duplicate.out())==DXGI_ERROR_INVALID_CALL);
 c->Draw(3,0);Ptr<ID3D11CommandList> list;Ptr<ID3D11Texture2D> colour,depth;UINT64 bytes=0;CHECK(packet->GetResult(list.out(),colour.out(),depth.out(),&bytes)==S_OK);CHECK(bytes>0&&bytes<8*1024*1024);packet.reset();
 auto expected=read(d,c,scene.colour,scene.depth,n);unsigned shaded=0,preserved=0;for(size_t i=0;i<expected.size();i+=8){shaded+=expected[i+5]==0x85;preserved+=expected[i+5]==0x25;}CHECK(shaded>1000&&preserved>1000);
 unsigned above_one=0;for(size_t i=0;i<expected.size();i+=8){for(unsigned ch=0;ch<3;++ch){float v;memcpy(&v,&expected[i+ch],4);if(v>1){++above_one;break;}}}if(colour_format==DXGI_FORMAT_R11G11B10_FLOAT)CHECK(above_one>1000);
 unsigned distinct=0;if(n==2){for(size_t i=0;i<expected.size();i+=16)distinct+=memcmp(expected.data()+i,expected.data()+i+8,32)!=0;CHECK(distinct>0);}
 scene.mutate(c);scene.bind(c);c->Draw(3,0);auto changed=read(d,c,scene.colour,scene.depth,n);CHECK(changed!=expected);scene.release_inputs();
 unsigned comparisons=0;
 for(unsigned replay=0;replay<3;++replay){
  Ptr<ID3D11RenderTargetView> r;CHECK(SUCCEEDED(d->CreateRenderTargetView(colour,nullptr,r.out())));Ptr<ID3D11DepthStencilView> ds;D3D11_DEPTH_STENCIL_VIEW_DESC ddview={};ddview.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;ddview.ViewDimension=n==1?D3D11_DSV_DIMENSION_TEXTURE2D:D3D11_DSV_DIMENSION_TEXTURE2DMS;CHECK(SUCCEEDED(d->CreateDepthStencilView(depth,&ddview,ds.out())));float bad[4]={1,0,1,1};c->ClearRenderTargetView(r,bad);c->ClearDepthStencilView(ds,D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.01,0xff);
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);D3D11_VIEWPORT vp={1,2,3,4,0,1};c->RSSetViewports(1,&vp);ID3D11RenderTargetView*rt=scene.rtv;c->OMSetRenderTargets(1,&rt,scene.dsv);
  c->ExecuteCommandList(list,TRUE);D3D11_PRIMITIVE_TOPOLOGY topology;c->IAGetPrimitiveTopology(&topology);CHECK(topology==D3D11_PRIMITIVE_TOPOLOGY_LINELIST);UINT count=1;D3D11_VIEWPORT after={};c->RSGetViewports(&count,&after);CHECK(count==1&&!memcmp(&vp,&after,sizeof(vp)));Ptr<ID3D11RenderTargetView> restored;c->OMGetRenderTargets(1,restored.out(),nullptr);CHECK(restored.p==scene.rtv.p);
  auto actual=read(d,c,colour,depth,n);CHECK(actual==expected);auto original=read(d,c,scene.colour,scene.depth,n);CHECK(original==changed);comparisons+=size*size*n;
 }
 // Queue mutation/renames immediately after capture, with no readback or flush
 // between capture and mutation. Also drop packet/list owners before GPU drain.
 {
  Scene queued(d,n);queued.bind(c);Ptr<IDXMTReplayPacket> p;
  CHECK(probe->CaptureNextDraw(8*1024*1024,p.out())==S_OK);c->Draw(3,0);
  Ptr<ID3D11CommandList> q;Ptr<ID3D11Texture2D> qc,qd;
  CHECK(p->GetResult(q.out(),qc.out(),qd.out(),nullptr)==S_OK);p.reset();
  queued.mutate(c);c->ClearState();queued.release_inputs();
  c->ExecuteCommandList(q,TRUE);q.reset();
  CHECK(read(d,c,qc,qd,n)==expected);comparisons+=size*size*n;
 }
 // Verify a real ordinary draw after ExecuteCommandList(TRUE), in addition
 // to the topology/viewport/attachment getters above.
 {
  Scene restored(d,n);restored.bind(c);c->ExecuteCommandList(list,TRUE);c->Draw(3,0);
  CHECK(read(d,c,restored.colour,restored.depth,n)==expected);
 }
 // Failed captures leave ordinary draws intact. Each case uses fresh mutable inputs.
 for(unsigned mode=0;mode<4;++mode){
  Scene reject(d,n),extra(d,n);reject.bind(c);Ptr<ID3D11Query> query;
  if(mode==1){ID3D11RenderTargetView*targets[]={reject.rtv,extra.rtv};c->OMSetRenderTargets(2,targets,reject.dsv);}
  if(mode==2){D3D11_QUERY_DESC q={D3D11_QUERY_OCCLUSION,0};CHECK(SUCCEEDED(d->CreateQuery(&q,query.out())));c->Begin(query);}
  Ptr<IDXMTReplayPacket> fail;CHECK(probe->CaptureNextDraw(mode==0?1:8*1024*1024,fail.out())==S_OK);
  if(mode==3)c->DrawInstanced(3,1,0,0);else c->Draw(3,0);
  CHECK(fail->GetResult(nullptr,nullptr,nullptr,nullptr)==DXGI_ERROR_UNSUPPORTED);
  if(query.p)c->End(query);
  auto normal=read(d,c,reject.colour,reject.depth,n);CHECK(normal==expected);
 }
 printf("REPLAY samples=%u hdr=%u above_one_samples=%u distinct_sample_pixels=%u nominal_bytes=%llu shaded=%u preserved=%u sample_comparisons=%u replay_count=3 input_mutation=1 queued_mutation=1 caller_release=1 seed_reset=1 context_restore=1 restored_draw=1 ordinary_output_unchanged=1 negative_cases=4 PASS\n",n,colour_format==DXGI_FORMAT_R11G11B10_FLOAT,above_one,distinct,(unsigned long long)bytes,shaded,preserved,comparisons);
 c->ClearState();c->Flush();return 0;
}

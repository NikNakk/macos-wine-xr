// Copyright 2026 Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Ownership wait attribution, DXGI presentation and GPU handoff regression.
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_3.h>
#include <cstdio>
#include <thread>
#include <atomic>
static std::atomic<int> failures{0};
static bool ok(HRESULT hr,const char* op) { if(hr != S_OK){printf("FAIL %s hr=%#lx\n",op,(unsigned long)hr);++failures;return false;}return true; }
int main(){
 ID3D11Device *a=nullptr,*b=nullptr;ID3D11DeviceContext *ac=nullptr,*bc=nullptr;
 if(!ok(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&a,nullptr,&ac),"device a")||!ok(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&b,nullptr,&bc),"device b"))return 1;
 D3D11_TEXTURE2D_DESC d={64,64,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX};
 ID3D11Texture2D *ta=nullptr,*tb=nullptr,*staging=nullptr;IDXGIResource *r=nullptr;HANDLE shared=nullptr;IDXGIKeyedMutex *ma=nullptr,*mb=nullptr;ID3D11RenderTargetView *rtv=nullptr;
 if(!ok(a->CreateTexture2D(&d,nullptr,&ta),"shared texture")||!ok(ta->QueryInterface(__uuidof(IDXGIResource),(void**)&r),"resource")||!ok(r->GetSharedHandle(&shared),"shared handle")||!ok(b->OpenSharedResource(shared,__uuidof(ID3D11Texture2D),(void**)&tb),"open texture")||!ok(ta->QueryInterface(__uuidof(IDXGIKeyedMutex),(void**)&ma),"mutex a")||!ok(tb->QueryInterface(__uuidof(IDXGIKeyedMutex),(void**)&mb),"mutex b")||!ok(a->CreateRenderTargetView(ta,nullptr,&rtv),"rtv"))return 1;
 d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;
 if(!ok(b->CreateTexture2D(&d,nullptr,&staging),"staging"))return 1;
 if(!ok(ma->AcquireSync(0,1000),"controlled owner acquire"))return 1;
 HRESULT timeoutResult=mb->AcquireSync(0,25);
 if(timeoutResult!=WAIT_TIMEOUT){printf("FAIL expected controlled timeout hr=%#lx\n",(unsigned long)timeoutResult);return 1;}
 std::thread releaser([&]{ok(ma->ReleaseSync(0),"cross-thread owner release");});releaser.join();
 if(!ok(mb->AcquireSync(0,1000),"acquire after timeout")||!ok(mb->ReleaseSync(0),"release after recovery"))return 1;
 puts("PASS controlled timeout and cross-thread release recovery");
 ID3D11Multithread *mt=nullptr;
 if(!ok(ac->QueryInterface(__uuidof(ID3D11Multithread),(void**)&mt),"multithread"))return 1;
 mt->SetMultithreadProtected(TRUE);
 if(!ok(ma->AcquireSync(0,1000),"contention owner acquire"))return 1;
 std::atomic<bool> entered{false};
 std::thread blocker([&]{mt->Enter();entered.store(true);Sleep(35);mt->Leave();});
 while(!entered.load()) std::this_thread::yield();
 if(!ok(ma->ReleaseSync(0),"contention owner release"))return 1;
 blocker.join();mt->Release();
 puts("PASS controlled 35ms context contention while keyed texture owned");
 // Real DXGI Present with the per-swapchain frame latency fence enabled.
 HWND window=CreateWindowExA(0,"STATIC","DXMT ownership presentation probe",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                            0,0,160,120,nullptr,nullptr,GetModuleHandleA(nullptr),nullptr);
 if(!window){puts("FAIL probe window");return 1;}
 IDXGIFactory *factory=nullptr;IDXGISwapChain *swap=nullptr;
 if(!ok(CreateDXGIFactory(__uuidof(IDXGIFactory),(void**)&factory),"factory"))return 1;
 DXGI_SWAP_CHAIN_DESC sd={};sd.BufferDesc.Width=64;sd.BufferDesc.Height=64;
 sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.SampleDesc.Count=1;
 sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=2;sd.OutputWindow=window;
 sd.Windowed=TRUE;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
 sd.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
 if(!ok(factory->CreateSwapChain(a,&sd,&swap),"swapchain"))return 1;
 if(!ok(ma->AcquireSync(0,1000),"presentation owner acquire"))return 1;
 for(int i=0;i<6;++i){
  ID3D11Texture2D *back=nullptr;ID3D11RenderTargetView *view=nullptr;
  if(!ok(swap->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&back),"backbuffer")||
     !ok(a->CreateRenderTargetView(back,nullptr,&view),"backbuffer view"))return 1;
  float color[]={0.2f,0.3f,0.4f,1.f};ac->ClearRenderTargetView(view,color);
  if(!ok(swap->Present(0,0),"present"))return 1;
  view->Release();back->Release();
 }
 Sleep(15);
 if(!ok(ma->ReleaseSync(0),"presentation owner release"))return 1;
 swap->Release();factory->Release();DestroyWindow(window);
 puts("PASS six DXGI presents during keyed ownership with swapchain pacing enabled");


 std::thread producer([&]{for(int i=0;i<200;++i){if(!ok(ma->AcquireSync(0,1000),"producer acquire"))return;float color[]={i%2?1.f:0.f,i%2?0.f:1.f,0.f,1.f};ac->ClearRenderTargetView(rtv,color);if(i==0){ac->Flush();Sleep(15);}if(!ok(ma->ReleaseSync(1),"producer release"))return;}});
 for(int i=0;i<200;++i){if(!ok(mb->AcquireSync(1,1000),"consumer acquire"))break;bc->CopyResource(staging,tb);D3D11_MAPPED_SUBRESOURCE map={};if(!ok(bc->Map(staging,0,D3D11_MAP_READ,0,&map),"readback")){mb->ReleaseSync(0);break;}unsigned pixel=*(unsigned*)map.pData;bc->Unmap(staging,0);if(i==0)Sleep(15);unsigned expected=i%2?0xff0000ff:0xff00ff00;if(pixel!=expected){printf("FAIL pixel round=%d actual=%08x expected=%08x\n",i,pixel,expected);++failures;}if(!ok(mb->ReleaseSync(0),"consumer release"))break;}
 producer.join();printf("%s keyed GPU handoff: 200 alternating frames, failures=%d\n",failures?"FAIL":"PASS",failures.load());
 rtv->Release();ma->Release();mb->Release();r->Release();staging->Release();tb->Release();ta->Release();ac->Release();bc->Release();a->Release();b->Release();return failures?1:0;
}

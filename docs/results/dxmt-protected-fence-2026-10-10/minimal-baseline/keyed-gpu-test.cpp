#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
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
 std::thread producer([&]{for(int i=0;i<200;++i){if(!ok(ma->AcquireSync(0,1000),"producer acquire"))return;float color[]={i%2?1.f:0.f,i%2?0.f:1.f,0.f,1.f};ac->ClearRenderTargetView(rtv,color);if(!ok(ma->ReleaseSync(1),"producer release"))return;}});
 for(int i=0;i<200;++i){if(!ok(mb->AcquireSync(1,1000),"consumer acquire"))break;bc->CopyResource(staging,tb);D3D11_MAPPED_SUBRESOURCE map={};if(!ok(bc->Map(staging,0,D3D11_MAP_READ,0,&map),"readback")){mb->ReleaseSync(0);break;}unsigned pixel=*(unsigned*)map.pData;bc->Unmap(staging,0);unsigned expected=i%2?0xff0000ff:0xff00ff00;if(pixel!=expected){printf("FAIL pixel round=%d actual=%08x expected=%08x\n",i,pixel,expected);++failures;}if(!ok(mb->ReleaseSync(0),"consumer release"))break;}
 producer.join();printf("%s keyed GPU handoff: 200 alternating frames, failures=%d\n",failures?"FAIL":"PASS",failures.load());
 rtv->Release();ma->Release();mb->Release();r->Release();staging->Release();tb->Release();ta->Release();ac->Release();bc->Release();a->Release();b->Release();return failures?1:0;
}

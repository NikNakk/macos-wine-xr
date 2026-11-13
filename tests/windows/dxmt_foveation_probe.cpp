// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>
#include "dxmt_native_interop.h"
#include "../in_process/foveation_probe.h"
#define STR_INNER(x) #x
#define STR(x) STR_INNER(x)
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static bool check_load_consumer(ID3D11Device *device,ID3D11DeviceContext *ctx,IDXMTNativeDevice2 *native,UINT64 texture) {
 D3D11_TEXTURE2D_DESC desc={};desc.Width=desc.Height=FOVEATION_PROBE_SIZE;desc.MipLevels=desc.ArraySize=1;
 desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
 ID3D11Texture2D *input=nullptr,*output=nullptr,*read=nullptr;
 if(FAILED(native->ImportMetalTexture(texture,&desc,&input)))return false;
 desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
 if(FAILED(device->CreateTexture2D(&desc,nullptr,&output)))return false;
 desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
 if(FAILED(device->CreateTexture2D(&desc,nullptr,&read)))return false;
 const char *shader="Texture2D<float4> src:register(t0);RWTexture2D<float4> dst:register(u0);"
  "[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){dst[p.xy]=src.Load(int3(p.xy,0));}";
 ID3DBlob *code=nullptr,*errors=nullptr;
 auto hr=D3DCompile(shader,std::strlen(shader),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&code,&errors);
 if(errors){std::fprintf(stderr,"%s",(char*)errors->GetBufferPointer());errors->Release();}if(FAILED(hr))return false;
 ID3D11ComputeShader *compute=nullptr;ID3D11ShaderResourceView *srv=nullptr;ID3D11UnorderedAccessView *uav=nullptr;
 if(FAILED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&compute))||
    FAILED(device->CreateShaderResourceView(input,nullptr,&srv))||FAILED(device->CreateUnorderedAccessView(output,nullptr,&uav)))return false;
 code->Release();ctx->CSSetShader(compute,nullptr,0);ctx->CSSetShaderResources(0,1,&srv);ctx->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
 ctx->Dispatch(FOVEATION_PROBE_SIZE/8,FOVEATION_PROBE_SIZE/8,1);
 ID3D11ShaderResourceView *no_srv=nullptr;ID3D11UnorderedAccessView *no_uav=nullptr;
 ctx->CSSetShaderResources(0,1,&no_srv);ctx->CSSetUnorderedAccessViews(0,1,&no_uav,nullptr);
 ctx->CopyResource(read,output);D3D11_MAPPED_SUBRESOURCE mapped={};if(FAILED(ctx->Map(read,0,D3D11_MAP_READ,0,&mapped)))return false;
 unsigned wrong=0;
 for(unsigned y=0;y<FOVEATION_PROBE_SIZE;++y)for(unsigned x=0;x<FOVEATION_PROBE_SIZE;++x){
  auto pixel=*((unsigned*)((char*)mapped.pData+y*mapped.RowPitch)+x);
  unsigned expected=(unsigned)std::lround((x+.5)*255/FOVEATION_PROBE_SIZE)|((unsigned)std::lround((y+.5)*255/FOVEATION_PROBE_SIZE)<<8)|0xff400000u;
  for(unsigned c=0;c<4;++c)if(std::abs(int((pixel>>(8*c))&255)-int((expected>>(8*c))&255))>3){
   if(wrong<4)std::fprintf(stderr,"Load pixel %u,%u got=%08x expected=%08x\n",x,y,pixel,expected);++wrong;break;
  }
 }
 ctx->Unmap(read,0);compute->Release();srv->Release();uav->Release();read->Release();output->Release();input->Release();
 std::printf("LOAD_CONSUMER pixels=%u wrong=%u\n",FOVEATION_PROBE_SIZE*FOVEATION_PROBE_SIZE,wrong);return !wrong;
}

int main(int argc, char **argv)
{
    bool negative_control = false, targeted = false, position_probe = false;
    bool unfoveated = false, reconstruct_probe = false, benchmark = false, load_consumer = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--negative-control")) negative_control = true;
        else if (!std::strcmp(argv[i], "--targeted")) targeted = true;
        else if (!std::strcmp(argv[i], "--position")) position_probe = true;
        else if (!std::strcmp(argv[i], "--unfoveated")) unfoveated = true;
        else if (!std::strcmp(argv[i], "--reconstruct")) reconstruct_probe = true;
        else if (!std::strcmp(argv[i], "--benchmark")) benchmark = true;
        else if (!std::strcmp(argv[i], "--load-consumer")) load_consumer = true;
        else REQUIRE(false);
    }
    HMODULE helper = LoadLibraryA("wine_foveation_probe.dll");
    REQUIRE(helper);
    auto probe = reinterpret_cast<LONG (WINAPI *)(UINT, void *)>(GetProcAddress(helper, "MWXRMetalProbe"));
    REQUIRE(probe);
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context)));
    std::fprintf(stderr, "probe: D3D11 device created\n");
    IDXMTNativeDevice2 *native = nullptr;
    IDXMTNativeContext1 *foveation = nullptr;
    IDXMTNativeContext2 *targets = nullptr;
    ID3D11DeviceContext4 *context4 = nullptr;
    REQUIRE(SUCCEEDED(device->QueryInterface(DXMT_IID_NATIVE_DEVICE2, reinterpret_cast<void **>(&native))));
    REQUIRE(SUCCEEDED(context->QueryInterface(DXMT_IID_NATIVE_CONTEXT1, reinterpret_cast<void **>(&foveation))));
    if (targeted) REQUIRE(SUCCEEDED(context->QueryInterface(DXMT_IID_NATIVE_CONTEXT2, reinterpret_cast<void **>(&targets))));
    REQUIRE(SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void **>(&context4))));
    UINT64 metal_device = 0;
    REQUIRE(SUCCEEDED(native->GetMetalDevice(&metal_device)) && metal_device);
    std::fprintf(stderr, "probe: native device/context interfaces acquired\n");

    const char *shader =
        "struct V { float4 p:SV_Position; float2 uv:TEXCOORD; };"
        "V vs(uint i:SV_VertexID) { V v; float2 p=float2((i==2)?3:-1,(i==1)?3:-1);"
        "v.p=float4(p,0,1); v.uv=float2((p.x+1)*0.5,(1-p.y)*0.5); return v; }"
        "float4 ps(V v):SV_Target { return float4(v.uv,0.25,1); }";
    const char *position_shader =
        "struct V { float4 p:SV_Position; float2 uv:TEXCOORD; };"
        "V vs(uint i:SV_VertexID) { V v; float2 p=float2((i==2)?3:-1,(i==1)?3:-1);"
        "v.p=float4(p,0,1); v.uv=float2((p.x+1)*0.5,(1-p.y)*0.5); return v; }"
        "float4 ps(V v):SV_Target { return float4(v.p.xy/PROBE_SIZE,0.25,1); }";
    if (position_probe) shader = position_shader;
    const char *work_shader =
        "struct V { float4 p:SV_Position; float2 uv:TEXCOORD; };"
        "V vs(uint i:SV_VertexID) { V v; float2 p=float2((i==2)?3:-1,(i==1)?3:-1);"
        "v.p=float4(p,0,1); v.uv=float2((p.x+1)*0.5,(1-p.y)*0.5); return v; }"
        "float4 ps(V v):SV_Target { float a=v.uv.x+.123; [loop] for(uint j=0;j<128;j++)"
        "a=sin(a*1.01+v.uv.y); return float4(v.uv+a*0.00001,0.25,1); }";
    const char *position_work_shader =
        "struct V { float4 p:SV_Position; float2 uv:TEXCOORD; };"
        "V vs(uint i:SV_VertexID) { V v; float2 p=float2((i==2)?3:-1,(i==1)?3:-1);"
        "v.p=float4(p,0,1);v.uv=float2((p.x+1)*.5,(1-p.y)*.5);return v;}"
        "float4 ps(V v):SV_Target {float2 uv=v.p.xy/PROBE_SIZE;float a=uv.x+.123;"
        "[loop]for(uint j=0;j<128;j++)a=sin(a*1.01+uv.y);return float4(uv+a*.00001,.25,1);}";
    if(benchmark)shader=position_probe?position_work_shader:work_shader;
    REQUIRE(!targeted || (!benchmark && !reconstruct_probe));
    REQUIRE(!load_consumer || reconstruct_probe);
    const D3D_SHADER_MACRO shader_macros[]={{"PROBE_SIZE",STR(FOVEATION_PROBE_SIZE)},{nullptr,nullptr}};
    ID3DBlob *vs = nullptr, *ps = nullptr, *errors = nullptr;
    REQUIRE(SUCCEEDED(D3DCompile(shader, std::strlen(shader), nullptr, shader_macros, nullptr,
        "vs", "vs_5_0", 0, 0, &vs, &errors)));
    if (errors) { errors->Release(); errors = nullptr; }
    REQUIRE(SUCCEEDED(D3DCompile(shader, std::strlen(shader), nullptr, shader_macros, nullptr,
        "ps", "ps_5_0", 0, 0, &ps, &errors)));
    if (errors) errors->Release();
    unsigned long long dxbc_hash=14695981039346656037ull;
    for(size_t i=0;i<ps->GetBufferSize();++i)dxbc_hash=(dxbc_hash^((const unsigned char*)ps->GetBufferPointer())[i])*1099511628211ull;
    std::printf("PIXEL_DXBC fnv1a64=%016llx bytes=%llu\n",dxbc_hash,(unsigned long long)ps->GetBufferSize());
    std::fprintf(stderr, "probe: HLSL compiled\n");
    ID3D11VertexShader *vertex = nullptr;
    ID3D11PixelShader *pixel = nullptr;
    REQUIRE(SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex)));
    REQUIRE(SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel)));
    vs->Release(); ps->Release();
    context->VSSetShader(vertex, nullptr, 0); context->PSSetShader(pixel, nullptr, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_VIEWPORT viewport = {0, 0, FOVEATION_PROBE_SIZE, FOVEATION_PROBE_SIZE, 0, 1};
    context->RSSetViewports(1, &viewport);
    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE; raster.DepthClipEnable = TRUE;
    ID3D11RasterizerState *rasterizer = nullptr;
    REQUIRE(SUCCEEDED(device->CreateRasterizerState(&raster, &rasterizer)));
    context->RSSetState(rasterizer);

    foveation_probe_params cases[4] = {};
    ID3D11Texture2D *textures[4] = {};
    ID3D11Fence *fences[4] = {};
    ID3D11RenderTargetView *rtvs[4] = {};
    const unsigned modes[] = {1, 2, 0, 1};
    constexpr unsigned timing_samples=24;
    ID3D11Query *disjoint=nullptr, *begin[4][timing_samples]={}, *end[4][timing_samples]={};
    LARGE_INTEGER cpu_frequency={}; QueryPerformanceFrequency(&cpu_frequency);
    double recording_cpu_ms[4]={}, recording_samples[4][timing_samples]={};
    if (benchmark) {
        D3D11_QUERY_DESC q={D3D11_QUERY_TIMESTAMP_DISJOINT,0};
        REQUIRE(SUCCEEDED(device->CreateQuery(&q,&disjoint)));
        q.Query=D3D11_QUERY_TIMESTAMP;
        for(unsigned i=0;i<4;++i) for(unsigned n=0;n<timing_samples;++n) {
            REQUIRE(SUCCEEDED(device->CreateQuery(&q,&begin[i][n])));
            REQUIRE(SUCCEEDED(device->CreateQuery(&q,&end[i][n])));
        }
        context->Begin(disjoint);
    }
    // Record all passes before flushing. Releasing the producer's map after
    // each bind exercises retention across the asynchronous command stream.
    for (unsigned i = 0; i < 4; ++i) {
        auto &p = cases[i]; p.device = metal_device; p.mode = unfoveated ? 0 : modes[i]; p.value = 1;
        std::fprintf(stderr, "probe: create/bind/draw case %u\n", i);
        REQUIRE(probe(FOVEATION_CREATE, &p) == 0 && p.status == 0);
        std::fprintf(stderr, "probe: created map=%llx physical=%ux%u\n",
            static_cast<unsigned long long>(p.map), p.physical_width, p.physical_height);
        if (p.mode) REQUIRE(p.physical_width < FOVEATION_PROBE_SIZE && p.physical_height < FOVEATION_PROBE_SIZE);
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = desc.Height = FOVEATION_PROBE_SIZE; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        REQUIRE(SUCCEEDED(native->ImportMetalTexture(p.texture, &desc, &textures[i])));
        REQUIRE(SUCCEEDED(native->ImportMetalSharedEvent(p.event, &fences[i])));
        ID3D11RenderTargetView *rtv = nullptr;
        REQUIRE(SUCCEEDED(device->CreateRenderTargetView(textures[i], nullptr, &rtv)));
        rtvs[i] = rtv;
        context->OMSetRenderTargets(1, &rtv, nullptr);
        const float clear[] = {0, 0, 0, 0};
        context->ClearRenderTargetView(rtv, clear);
        std::fprintf(stderr, "probe: binding rate map\n");
        if (targeted) {
            if (p.map) {
                ID3D11Texture2D *wrong_size = nullptr;
                D3D11_TEXTURE2D_DESC wrong_desc = desc; wrong_desc.Width = wrong_desc.Height = 64;
                REQUIRE(SUCCEEDED(device->CreateTexture2D(&wrong_desc, nullptr, &wrong_size)));
                REQUIRE(targets->SetRenderTargetRasterizationRateMap(wrong_size, p.map) == E_INVALIDARG);
                wrong_size->Release();
            }
            REQUIRE(SUCCEEDED(targets->SetRenderTargetRasterizationRateMap(textures[i], negative_control ? 0 : p.map)));
            REQUIRE(targets->SetRenderTargetRasterizationRateMap(textures[i], p.texture) == E_INVALIDARG);
        } else {
            REQUIRE(SUCCEEDED(foveation->SetRasterizationRateMap(negative_control ? 0 : p.map)));
            REQUIRE(foveation->SetRasterizationRateMap(p.texture) == E_INVALIDARG);
        }
        std::fprintf(stderr, "probe: rate map bound\n");
        if (!reconstruct_probe) REQUIRE(probe(FOVEATION_RELEASE_MAP, &p) == 0);
        if (!targeted) {
            // Timestamp boundaries prevent opaque overdraw elimination from
            // turning an N-draw average into a misleading single-pass cost.
            for(unsigned n=0;n<(benchmark ? timing_samples : 1u);++n) {
                if(benchmark) context->End(begin[i][n]);
                LARGE_INTEGER cpu_begin={},cpu_end={}; QueryPerformanceCounter(&cpu_begin);
                context->Draw(3,0);
                QueryPerformanceCounter(&cpu_end);
                recording_samples[i][n]=double(cpu_end.QuadPart-cpu_begin.QuadPart)*1000.0/cpu_frequency.QuadPart;
                if(benchmark) context->End(end[i][n]);
            }
        }
        std::fprintf(stderr, "probe: draw recorded\n");
    }
    if (targeted) {
        // All targets have been tagged already. Draw in a different order,
        // including an untagged control, to catch accidental global binding.
        const unsigned order[] = {1, 0, 2, 3};
        for (unsigned i : order) {
            context->OMSetRenderTargets(1, &rtvs[i], nullptr);
            context->Draw(3, 0);
        }
        for (auto *texture : textures) REQUIRE(SUCCEEDED(targets->SetRenderTargetRasterizationRateMap(texture, 0)));
    }
    for (auto *rtv : rtvs) rtv->Release();
    REQUIRE(SUCCEEDED(foveation->SetRasterizationRateMap(0)));
    if (benchmark) context->End(disjoint);
    // DXMT's Signal also flushes. Signal only after all four passes and map
    // changes have been recorded, so none can hide a stale state snapshot.
    for (unsigned i = 0; i < 4; ++i)
        REQUIRE(SUCCEEDED(context4->Signal(fences[i], cases[i].value)));
    context->Flush();
    std::fprintf(stderr, "probe: submitted; checking GPU results\n");
    double scene_ms[4]={};
    if (benchmark) {
        auto get=[&](ID3D11Query *q,void *data,UINT size) {
            HRESULT hr=S_FALSE;
            for(unsigned n=0;n<10000 && hr==S_FALSE;++n) {
                hr=context->GetData(q,data,size,0); if(hr==S_FALSE) Sleep(1);
            }
            return hr==S_OK;
        };
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT timing={};
        REQUIRE(get(disjoint,&timing,sizeof(timing)) && !timing.Disjoint && timing.Frequency);
        for(unsigned i=0;i<4;++i) {
            double gpu_samples[21], cpu_samples[21];
            for(unsigned n=0;n<timing_samples;++n) {
                UINT64 a=0,b=0;
                REQUIRE(get(begin[i][n],&a,sizeof(a)) && get(end[i][n],&b,sizeof(b)) && b>a);
                if(n>=3) { gpu_samples[n-3]=double(b-a)*1000.0/timing.Frequency;cpu_samples[n-3]=recording_samples[i][n]; }
                begin[i][n]->Release();end[i][n]->Release();
            }
            std::sort(gpu_samples,gpu_samples+21);std::sort(cpu_samples,cpu_samples+21);
            scene_ms[i]=gpu_samples[10];recording_cpu_ms[i]=cpu_samples[10];
        }
        disjoint->Release();
    }
    for (unsigned i = 0; i < 4; ++i) {
        auto &p = cases[i];
        if(reconstruct_probe && p.map) REQUIRE(probe(FOVEATION_RECONSTRUCT,&p)==0 && p.status==0);
        REQUIRE(probe(FOVEATION_VERIFY, &p) == 0 && p.status == 0 && p.checked == FOVEATION_PROBE_SAMPLES);
        std::printf("PASS case=%u mode=%u logical=%ux%u physical=%ux%u samples=%u\n",
            i, p.mode, FOVEATION_PROBE_SIZE, FOVEATION_PROBE_SIZE, p.physical_width, p.physical_height, p.checked);
        if(benchmark) std::printf("TIMING case=%u scene_gpu_ms=%.6f reconstruct_gpu_ms=%.6f net_gpu_ms=%.6f\n",
            i,scene_ms[i],p.reconstruction_gpu_ms,scene_ms[i]+p.reconstruction_gpu_ms);
        if(benchmark) std::printf("CPU case=%u draw_record_ms=%.6f reconstruct_encode_commit_ms=%.6f\n",
            i,recording_cpu_ms[i],p.reconstruction_cpu_ms);
        if(load_consumer) REQUIRE(check_load_consumer(device,context,native,p.texture));
        fences[i]->Release(); textures[i]->Release();
        REQUIRE(probe(FOVEATION_RELEASE, &p) == 0);
    }
    context->ClearState();
    rasterizer->Release(); vertex->Release(); pixel->Release();
    if (targets) targets->Release();
    context4->Release(); foveation->Release(); native->Release(); context->Release(); device->Release();
    FreeLibrary(helper);
    return 0;
}

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Stay-warped fixture: a target rendered with a Metal rate map is left in its
// physical layout and read by later passes through DXMT's read translation.
//   A (mapped)   scene = logical pixel position
//   B (mapped)   chain = Load / Sample / offset Load / point SampleLevel of scene
//   C (ordinary) Load of chain, D (ordinary) bilinear Sample of chain
// C and D are read back and compared with the analytic logical result.
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "dxmt_native_interop.h"
#include "../in_process/foveation_probe.h"
#define STR_INNER(x) #x
#define STR(x) STR_INNER(x)
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #x); return 1; } } while (0)

constexpr unsigned N = FOVEATION_PROBE_SIZE;

static const char *source =
    "Texture2D<float4> ordinary:register(t127);Texture2D<float4> src:register(t0);SamplerState lin:register(s0);SamplerState pt:register(s1);"
    "struct V{float4 p:SV_Position;float2 uv:TEXCOORD;};"
    "V vs(uint i:SV_VertexID){V v;float2 p=float2((i==2)?3:-1,(i==1)?3:-1);"
    "v.p=float4(p,0,1);v.uv=float2((p.x+1)*.5,(1-p.y)*.5);return v;}"
    "float4 ps_scene(V v):SV_Target{return float4(v.p.xy/PROBE_SIZE,.5,1);}"
    "float4 ps_chain(V v):SV_Target{int3 q=int3(v.p.xy,0);"
    "return float4(src.Load(q).r,src.Sample(lin,v.uv).g,src.Load(q,int2(7,0)).r,src.SampleLevel(pt,v.uv,0).g);}"
    "float4 ps_mixed(V v):SV_Target{int3 q=int3(v.p.xy,0);"
    "return float4(src.Load(q).r,ordinary.Sample(lin,v.uv).g,src.Load(q,int2(7,0)).r,ordinary.SampleLevel(pt,v.uv,0).g);}"
    "float4 ps_load(V v):SV_Target{return src.Load(int3(v.p.xy,0));}"
    "float4 ps_sample(V v):SV_Target{return src.Sample(lin,v.uv);}";

static ID3DBlob *compile(const char *entry, const char *profile)
{
    const D3D_SHADER_MACRO macros[] = {{"PROBE_SIZE", STR(FOVEATION_PROBE_SIZE) ".0"}, {nullptr, nullptr}};
    ID3DBlob *blob = nullptr, *errors = nullptr;
    HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, macros, nullptr, entry, profile, 0, 0, &blob, &errors);
    if (errors) { std::fprintf(stderr, "%s", (char *)errors->GetBufferPointer()); errors->Release(); }
    return SUCCEEDED(hr) ? blob : nullptr;
}

struct Error { double centre_max = 0, outer_max = 0, sum = 0; unsigned long long count = 0, over = 0; };

int main(int argc, char **argv)
{
    bool unfoveated = false, scissor = false, late_read = false, split_pass = false, reverse_read = false, mixed_reads = false, mixed_swap = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--unfoveated")) unfoveated = true;
        else if (!std::strcmp(argv[i], "--scissor")) scissor = true;
        else if (!std::strcmp(argv[i], "--late-read")) late_read = true;
        else if (!std::strcmp(argv[i], "--split-pass")) split_pass = true;
        else if (!std::strcmp(argv[i], "--reverse-read")) reverse_read = true;
        else if (!std::strcmp(argv[i], "--mixed-reads")) mixed_reads = true;
        else if (!std::strcmp(argv[i], "--mixed-swap")) mixed_reads = mixed_swap = true;
        else REQUIRE(false);
    }
    REQUIRE(!split_pass || (unfoveated && late_read));
    REQUIRE(!reverse_read || late_read);
    REQUIRE(!(late_read && mixed_reads));
    HMODULE helper = LoadLibraryA("wine_foveation_probe.dll");
    REQUIRE(helper);
    auto probe = reinterpret_cast<LONG (WINAPI *)(UINT, void *)>(GetProcAddress(helper, "MWXRMetalProbe"));
    REQUIRE(probe);
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context)));
    IDXMTNativeDevice2 *native = nullptr;
    IDXMTNativeContext1 *foveation = nullptr;
    REQUIRE(SUCCEEDED(device->QueryInterface(DXMT_IID_NATIVE_DEVICE2, reinterpret_cast<void **>(&native))));
    REQUIRE(SUCCEEDED(context->QueryInterface(DXMT_IID_NATIVE_CONTEXT1, reinterpret_cast<void **>(&foveation))));
    foveation_probe_params map = {};
    REQUIRE(SUCCEEDED(native->GetMetalDevice(&map.device)) && map.device);
    map.mode = unfoveated ? 0 : 1; map.value = 1;
    REQUIRE(probe(FOVEATION_CREATE, &map) == 0 && map.status == 0);
    if (!unfoveated) REQUIRE(map.map && map.physical_width < N && map.physical_height < N);

    ID3DBlob *vs = compile("vs", "vs_5_0");
    REQUIRE(vs);
    ID3D11VertexShader *vertex = nullptr;
    REQUIRE(SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex)));
    vs->Release();
    const char *entries[4] = {"ps_scene", mixed_reads ? "ps_mixed" : "ps_chain", "ps_load", "ps_sample"};
    ID3D11PixelShader *pixel[4] = {};
    for (unsigned i = 0; i < 4; ++i) {
        ID3DBlob *ps = compile(entries[i], "ps_5_0");
        REQUIRE(ps);
        REQUIRE(SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel[i])));
        ps->Release();
    }
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState *samplers[2] = {};
    REQUIRE(SUCCEEDED(device->CreateSamplerState(&sd, &samplers[0])));
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    REQUIRE(SUCCEEDED(device->CreateSamplerState(&sd, &samplers[1])));
    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE; raster.DepthClipEnable = TRUE;
    ID3D11RasterizerState *rasterizer = nullptr;
    REQUIRE(SUCCEEDED(device->CreateRasterizerState(&raster, &rasterizer)));
    // --scissor: the scene pass draws inside a logical rectangle only.
    raster.ScissorEnable = TRUE;
    ID3D11RasterizerState *scissored = nullptr;
    REQUIRE(SUCCEEDED(device->CreateRasterizerState(&raster, &scissored)));
    const D3D11_RECT rect = {LONG(N * 9 / 16), LONG(N * 3 / 16), LONG(N * 14 / 16), LONG(N * 13 / 16)};

    // scene, chain, ordinary Load result, ordinary Sample result
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = desc.Height = N; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_UNORM; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *textures[4] = {};
    ID3D11RenderTargetView *rtvs[4] = {};
    ID3D11ShaderResourceView *srvs[4] = {};
    for (unsigned i = 0; i < 4; ++i) {
        REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &textures[i])));
        REQUIRE(SUCCEEDED(device->CreateRenderTargetView(textures[i], nullptr, &rtvs[i])));
        REQUIRE(SUCCEEDED(device->CreateShaderResourceView(textures[i], nullptr, &srvs[i])));
    }
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging = nullptr;
    REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)));

    context->VSSetShader(vertex, nullptr, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_VIEWPORT viewport = {0, 0, float(N), float(N), 0, 1};
    context->RSSetViewports(1, &viewport);
    context->RSSetState(rasterizer);
    context->PSSetSamplers(0, 2, samplers);
    const float clear[] = {0, 0, 0, 0};
    ID3D11ShaderResourceView *no_srv = nullptr;
    auto pass = [&](unsigned target, unsigned shader, ID3D11ShaderResourceView *input) {
        context->PSSetShaderResources(0, 1, &no_srv);
        context->OMSetRenderTargets(1, &rtvs[target], nullptr);
        context->ClearRenderTargetView(rtvs[target], clear);
        context->PSSetShader(pixel[shader], nullptr, 0);
        context->PSSetShaderResources(0, 1, input ? &input : &no_srv);
        context->Draw(3, 0);
    };
    // --late-read: bind an ordinary source first, then a mapped one without
    // changing the pixel shader or render target. Selection must see the whole pass.
    if (late_read || mixed_reads) pass(2, 0, nullptr);
    // Everything is recorded before the first flush.
    REQUIRE(SUCCEEDED(foveation->SetRasterizationRateMap(map.map)));
    if (scissor) { context->RSSetState(scissored); context->RSSetScissorRects(1, &rect); }
    pass(0, 0, nullptr);   // A: mapped scene
    context->RSSetState(rasterizer);
    if (mixed_reads) {
        context->PSSetShaderResources(127, 1, &srvs[2]);
        pass(1, 1, srvs[0]);
        if (mixed_swap) {
            context->PSSetShaderResources(0, 1, &srvs[2]);
            context->PSSetShaderResources(127, 1, &srvs[0]);
            context->Draw(3, 0);
        }
        context->PSSetShaderResources(127, 1, &no_srv);
    } else if (late_read) {
        pass(1, 1, srvs[reverse_read ? 0 : 2]);
        if (split_pass) REQUIRE(SUCCEEDED(foveation->SetRasterizationRateMap(0)));
        context->PSSetShaderResources(0, 1, &srvs[reverse_read ? 2 : 0]);
        context->Draw(3, 0);
    } else {
        pass(1, 1, srvs[0]);   // B: mapped pass reading the mapped scene
    }
    REQUIRE(SUCCEEDED(foveation->SetRasterizationRateMap(0)));
    pass(2, 2, srvs[1]);   // C: ordinary pass, Load of the mapped chain
    pass(3, 3, srvs[1]);   // D: ordinary pass, bilinear Sample of the mapped chain
    context->PSSetShaderResources(0, 1, &no_srv);
    ID3D11RenderTargetView *no_rtv = nullptr;
    context->OMSetRenderTargets(1, &no_rtv, nullptr);

    // Full-rate cells of helper map mode 1, shrunk because rates blend between cells.
    const double cx0 = N * (2 / 8.0 + 1 / 16.0), cx1 = N * (4 / 8.0 - 1 / 16.0);
    const double cy0 = N * (3 / 8.0 + 1 / 16.0), cy1 = N * (5 / 8.0 - 1 / 16.0);
    const char *channel_names[4] = {"load_x", "sample_y", "offset_load_x", "point_sample_y"};
    bool ok = true;
    for (unsigned result = 2; result < 4; ++result) {
        context->CopyResource(staging, textures[result]);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        REQUIRE(SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)));
        Error errors[4];
        unsigned x0 = N, y0 = N, x1 = 0, y1 = 0;
        for (unsigned y = 0; y < N; ++y) {
            auto row = reinterpret_cast<const unsigned short *>(static_cast<const char *>(mapped.pData) + y * mapped.RowPitch);
            for (unsigned x = 0; x < N; ++x) {
                if (scissor) {
                    if (row[x * 4] || row[x * 4 + 1]) { x0 = std::min(x0, x); x1 = std::max(x1, x + 1); y0 = std::min(y0, y); y1 = std::max(y1, y + 1); }
                    continue;
                }
                const bool centre = !unfoveated && x + .5 > cx0 && x + .5 < cx1 && y + .5 > cy0 && y + .5 < cy1;
                const double expected[4] = {x + .5, y + .5, x + 7.5, y + .5};
                for (unsigned c = 0; c < 4; ++c) {
                    if (c == 2 && x + 16 >= N) continue; // offset read clamps at the edge
                    double got = row[x * 4 + c] * double(N) / 65535.0;
                    double e = std::fabs(got - expected[c]);
                    auto &s = errors[c];
                    (centre || unfoveated ? s.centre_max : s.outer_max) = std::max(centre || unfoveated ? s.centre_max : s.outer_max, e);
                    s.sum += e; ++s.count;
                    if (e > 8) {
                        if (s.over < 2) std::fprintf(stderr, "%s %s pixel %u,%u got=%.2f expected=%.2f\n",
                            result == 2 ? "load" : "sample", channel_names[c], x, y, got, expected[c]);
                        ++s.over;
                    }
                }
            }
        }
        context->Unmap(staging, 0);
        if (scissor) {
            const int worst = std::max(std::max(std::abs(int(x0) - int(rect.left)), std::abs(int(x1) - int(rect.right))),
                                       std::max(std::abs(int(y0) - int(rect.top)), std::abs(int(y1) - int(rect.bottom))));
            std::printf("WARP_SCISSOR final=%s expected=%ld,%ld-%ld,%ld drawn=%u,%u-%u,%u worst_edge_px=%d\n", result == 2 ? "load" : "sample",
                rect.left, rect.top, rect.right, rect.bottom, x0, y0, x1, y1, worst);
            ok = ok && worst <= 6;
            continue;
        }
        for (unsigned c = 0; c < 4; ++c) {
            auto &s = errors[c];
            // Logical pixels. One nearest lookup per stage costs up to half a
            // physical texel: 0.5 px at full rate, 2 px at rate 0.25.
            const double centre_limit = c == 2 ? 2.1 : 1.6, outer_limit = c == 2 ? 6.5 : 4.5;
            const bool pass_channel = s.centre_max <= centre_limit && s.outer_max <= outer_limit;
            ok = ok && pass_channel;
            std::printf("WARP_READ final=%s channel=%s centre_max_px=%.3f outer_max_px=%.3f mean_px=%.4f over_8px=%llu checked=%llu %s\n",
                result == 2 ? "load" : "sample", channel_names[c], s.centre_max, s.outer_max,
                s.sum / double(s.count), s.over, s.count, pass_channel ? "ok" : "WRONG");
        }
    }
    std::printf("%s mode=%u logical=%ux%u physical=%ux%u\n", ok ? "PASS" : "FAIL", map.mode, N, N,
        map.physical_width, map.physical_height);
    context->ClearState();
    for (unsigned i = 0; i < 4; ++i) { srvs[i]->Release(); rtvs[i]->Release(); textures[i]->Release(); pixel[i]->Release(); }
    scissored->Release(); staging->Release(); samplers[0]->Release(); samplers[1]->Release(); rasterizer->Release(); vertex->Release();
    REQUIRE(probe(FOVEATION_RELEASE, &map) == 0);
    foveation->Release(); native->Release(); context->Release(); device->Release();
    FreeLibrary(helper);
    return ok ? 0 : 1;
}

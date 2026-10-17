// GPU backend for lossy CinemaDNG: the curve lookup + DCT and the rate-control search run as Direct3D 11
// compute shaders, on any DX11 GPU (NVIDIA, AMD, Intel), with nothing to install. Shaders are compiled
// at startup with d3dcompiler_47.dll, which ships with Windows 10 and 11. The Huffman coding runs on
// the GPU too (count each block's bits, prefix-sum them per tile, then every block writes its codes at
// its own bit offset); the CPU only adds JPEG's 0xFF byte stuffing and the headers. The bytes are the
// same as lossy.hpp's EncodeTile would write from the same coefficients.
//
// One Device is shared by all worker threads behind a mutex; each worker owns a Backend with its own
// GPU buffers, so a frame's coefficients stay on the GPU between the DCT and the size search.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX   // d3d11.h pulls in windows.h; keep its min/max macros away from std::min/std::max
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include "lossy.hpp"
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace gpu_lossy
{
    using Microsoft::WRL::ComPtr;

    static const char kShader[] = R"HLSL(
cbuffer Params : register(b0)
{
    uint W, H, tile_w, jw;
    uint jh, bw, bh, blocks;
    uint shift, down, num_scales, cap_words;   // shift: to the 14-bit lut index (down is unused, 0); cap_words: stream space per tile
};
StructuredBuffer<uint>  cfa     : register(t0);   // two 16-bit samples per element
StructuredBuffer<uint>  enc_lut : register(t1);   // 14-bit linear -> curve code
StructuredBuffer<float> descale : register(t2);   // AAN output -> JPEG coefficient, natural order
StructuredBuffer<float> inv_q   : register(t3);   // num_scales x 64, zigzag order
StructuredBuffer<uint>  dc_len  : register(t4);
StructuredBuffer<uint>  ac_len  : register(t5);
StructuredBuffer<uint>  dc_code : register(t6);
StructuredBuffer<uint>  ac_code : register(t7);
RWStructuredBuffer<uint> coefs  : register(u0);   // blocks x 32, two int16 coefficients each, zigzag order
RWStructuredBuffer<uint> totals : register(u1);   // bits per scale; bits per tile after scan_main
RWStructuredBuffer<uint> blk_bits : register(u2); // bits per block, then (scan_main) its bit offset in its tile
RWStructuredBuffer<uint> stream : register(u3);   // tiles x cap_words: the entropy-coded bytes in file order

static const uint zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };

uint sample_at(uint x, uint y)
{
    uint i = y * W + x;
    uint w = cfa[i >> 1];
    return (i & 1) ? (w >> 16) : (w & 0xFFFF);
}

// AAN float forward DCT on 8 values at d[o], d[o+s], ..., as in lossy.hpp FdctFloat
#define FDCT8(d, o, s)                                                                  \
{                                                                                       \
    precise float t0 = d[o] + d[o + 7 * s], t7 = d[o] - d[o + 7 * s];                   \
    precise float t1 = d[o + s] + d[o + 6 * s], t6 = d[o + s] - d[o + 6 * s];           \
    precise float t2 = d[o + 2 * s] + d[o + 5 * s], t5 = d[o + 2 * s] - d[o + 5 * s];   \
    precise float t3 = d[o + 3 * s] + d[o + 4 * s], t4 = d[o + 3 * s] - d[o + 4 * s];   \
    precise float t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;           \
    d[o] = t10 + t11;                                                                   \
    d[o + 4 * s] = t10 - t11;                                                           \
    precise float z1 = (t12 + t13) * 0.707106781f;                                      \
    d[o + 2 * s] = t13 + z1;                                                            \
    d[o + 6 * s] = t13 - z1;                                                            \
    t10 = t4 + t5; t11 = t5 + t6; t12 = t6 + t7;                                        \
    precise float z5 = (t10 - t12) * 0.382683433f;                                      \
    precise float z2 = 0.541196100f * t10 + z5;                                         \
    precise float z4 = 1.306562965f * t12 + z5;                                         \
    precise float z3 = t11 * 0.707106781f;                                              \
    precise float z11 = t7 + z3, z13 = t7 - z3;                                         \
    d[o + 5 * s] = z13 + z2;                                                            \
    d[o + 3 * s] = z13 - z2;                                                            \
    d[o + s] = z11 + z4;                                                                \
    d[o + 7 * s] = z11 - z4;                                                            \
}

int round_half_away(float x) { return x >= 0 ? (int)floor(x + 0.5f) : -(int)floor(-x + 0.5f); }   // = lround

[numthreads(64, 1, 1)]
void dct_main(uint3 id : SV_DispatchThreadID)
{
    uint b = id.x;
    if (b >= blocks) return;
    uint per_tile = bw * bh, t = b / per_tile, r = b % per_tile, by = r / bw, bx = r % bw;
    precise float d[64];
    [unroll] for (uint y = 0; y < 8; ++y)
    [unroll] for (uint x = 0; x < 8; ++x)
    {
        uint jy = min(by * 8 + y, jh - 1), jx = min(bx * 8 + x, jw - 1);
        uint right = jx >= tile_w ? 1 : 0;                 // JPEG row jy = image rows 2jy | 2jy+1
        uint sx = t * tile_w + jx - right * tile_w, sy = 2 * jy + right;
        if (sx >= W) sx = W - 2 + (sx & 1);                // padding past the right edge (lossy::TileWidth)
        uint v = sample_at(sx, sy) << shift;
        d[y * 8 + x] = (float)((int)enc_lut[min(v, 16383u)] - 2048);
    }
    [unroll] for (uint row = 0; row < 8; ++row) FDCT8(d, row * 8, 1)
    [unroll] for (uint col = 0; col < 8; ++col) FDCT8(d, col, 8)
    [unroll] for (uint k = 0; k < 64; k += 2)
    {
        int c0 = round_half_away(d[zigzag[k]] * descale[zigzag[k]]);
        int c1 = round_half_away(d[zigzag[k + 1]] * descale[zigzag[k + 1]]);
        coefs[b * 32 + k / 2] = ((uint)c0 & 0xFFFF) | ((uint)c1 << 16);
    }
}

int coef(uint b, uint k)
{
    uint w = coefs[b * 32 + (k >> 1)];
    return (k & 1) ? ((int)w >> 16) : (((int)(w << 16)) >> 16);
}
int quant(uint b, uint k, uint s) { return clamp((int)round((float)coef(b, k) * inv_q[s * 64 + k]), -16383, 16383); }   // round = to nearest even, as the CPU
uint bit_size(int v) { return v == 0 ? 0 : firstbithigh((uint)abs(v)) + 1; }

// Entropy-coded bits of block b at scale s, as lossy.hpp EncodeTile writes them
uint block_bits(uint b, uint s)
{
    uint per_tile = bw * bh;
    int dc = quant(b, 0, s), prev = (b % per_tile) ? quant(b - 1, 0, s) : 0;   // DC is coded as a difference within the tile
    uint cat = min(bit_size(clamp(dc - prev, -32767, 32767)), 15u);
    uint bits = dc_len[cat] + cat;
    uint last = 0;
    for (uint k = 1; k < 64; ++k)
    {
        int v = quant(b, k, s);
        if (v == 0) continue;
        uint run = k - last - 1;
        for (; run > 15; run -= 16) bits += ac_len[0xF0];
        uint sz = bit_size(v);
        bits += ac_len[(run << 4) | sz] + sz;
        last = k;
    }
    if (last != 63) bits += ac_len[0];
    return bits;
}

groupshared uint group_bits;

[numthreads(64, 1, 1)]
void count_main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    uint s = gid.y, b = gid.x * 64 + gtid.x;
    if (gtid.x == 0) group_bits = 0;
    GroupMemoryBarrierWithGroupSync();
    uint bits = b < blocks ? block_bits(b, s) : 0;
    InterlockedAdd(group_bits, bits);
    GroupMemoryBarrierWithGroupSync();
    if (gtid.x == 0) InterlockedAdd(totals[s], group_bits);
}

// ---- Huffman coding at scale 0 (inv_q's first table): bits_main, scan_main, emit_main in that order

[numthreads(64, 1, 1)]
void bits_main(uint3 id : SV_DispatchThreadID)
{
    if (id.x < blocks) blk_bits[id.x] = block_bits(id.x, 0);
}

// One group per tile: block bit counts -> each block's bit offset in the tile, and the tile's total
groupshared uint part[1024];

[numthreads(1024, 1, 1)]
void scan_main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    uint per_tile = bw * bh, base = gid.x * per_tile;
    uint chunk = (per_tile + 1023) / 1024, i0 = min(gtid.x * chunk, per_tile), i1 = min(i0 + chunk, per_tile);
    uint sum = 0;
    for (uint i = i0; i < i1; ++i) sum += blk_bits[base + i];
    part[gtid.x] = sum;
    GroupMemoryBarrierWithGroupSync();
    if (gtid.x == 0)
    {
        uint run = 0;
        for (uint k = 0; k < 1024; ++k) { uint v = part[k]; part[k] = run; run += v; }
        totals[gid.x] = run;
    }
    GroupMemoryBarrierWithGroupSync();
    uint off = part[gtid.x];
    for (uint j = i0; j < i1; ++j) { uint v = blk_bits[base + j]; blk_bits[base + j] = off; off += v; }
}

uint bswap(uint x) { return (x >> 24) | ((x >> 8) & 0xFF00) | ((x << 8) & 0xFF0000) | (x << 24); }

// Appends len bits of v (len <= 31) at bit `pos` of the tile stream, MSB first. `acc` holds the bits
// of the current word; whole words are ORed in (blocks share their first and last words).
void put(inout uint pos, inout uint acc, uint base, uint v, uint len)
{
    uint room = 32 - (pos & 31);
    if (len < room) acc |= v << (room - len);
    else
    {
        uint spill = len - room;
        acc |= v >> spill;
        uint wi = pos >> 5;
        if (wi < cap_words) InterlockedOr(stream[base + wi], bswap(acc));   // bytes in file order
        acc = spill ? v << (32 - spill) : 0;
    }
    pos += len;
}

[numthreads(64, 1, 1)]
void emit_main(uint3 id : SV_DispatchThreadID)
{
    uint b = id.x;
    if (b >= blocks) return;
    uint per_tile = bw * bh, base = (b / per_tile) * cap_words, pos = blk_bits[b], acc = 0;
    int dc = quant(b, 0, 0), prev = (b % per_tile) ? quant(b - 1, 0, 0) : 0;
    int diff = clamp(dc - prev, -32767, 32767);
    uint cat = min(bit_size(diff), 15u);
    put(pos, acc, base, (dc_code[cat] << cat) | ((uint)(diff < 0 ? diff - 1 : diff) & ((1u << cat) - 1)), dc_len[cat] + cat);
    uint last = 0;
    for (uint k = 1; k < 64; ++k)
    {
        int v = quant(b, k, 0);
        if (v == 0) continue;
        uint run = k - last - 1;
        for (; run > 15; run -= 16) put(pos, acc, base, ac_code[0xF0], ac_len[0xF0]);
        uint sz = bit_size(v), sym = (run << 4) | sz;
        put(pos, acc, base, (ac_code[sym] << sz) | ((uint)(v < 0 ? v - 1 : v) & ((1u << sz) - 1)), ac_len[sym] + sz);
        last = k;
    }
    if (last != 63) put(pos, acc, base, ac_code[0], ac_len[0]);
    if (pos & 31) { uint wi = pos >> 5; if (wi < cap_words) InterlockedOr(stream[base + wi], bswap(acc)); }
}
)HLSL";

    static const int kScales = 16;   // scales measured per rate-control dispatch

    struct Params { uint32_t W, H, tile_w, jw, jh, bw, bh, blocks, shift, down, num_scales, cap_words; };

    inline bool make_buffer(ID3D11Device* dev, UINT bytes, UINT bind, D3D11_USAGE usage, UINT cpu, UINT stride, const void* init, ComPtr<ID3D11Buffer>& out)
    {
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = bytes;
        d.Usage = usage;
        d.BindFlags = bind;
        d.CPUAccessFlags = cpu;
        if (stride) { d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; d.StructureByteStride = stride; }
        D3D11_SUBRESOURCE_DATA s = {init, 0, 0};
        return SUCCEEDED(dev->CreateBuffer(&d, init ? &s : nullptr, out.GetAddressOf()));
    }
    inline bool make_srv(ID3D11Device* dev, ID3D11Buffer* b, ComPtr<ID3D11ShaderResourceView>& out) { return SUCCEEDED(dev->CreateShaderResourceView(b, nullptr, out.GetAddressOf())); }
    inline bool make_uav(ID3D11Device* dev, ID3D11Buffer* b, ComPtr<ID3D11UnorderedAccessView>& out) { return SUCCEEDED(dev->CreateUnorderedAccessView(b, nullptr, out.GetAddressOf())); }

    // The GPU, its compiled shaders and the constant tables, shared by every worker.
    class Device
    {
    public:
        ComPtr<ID3D11Device> dev;
        ComPtr<ID3D11DeviceContext> ctx;
        ComPtr<ID3D11DeviceContext4> ctx4;   // fence signalling (D3D11.3)
        ComPtr<ID3D11Fence> fence;
        uint64_t fence_value = 0;             // last value signalled (under lock)
        ComPtr<ID3D11ComputeShader> dct, count, bits, scan, emit;
        ComPtr<ID3D11Buffer> lut, descale, dc_len, ac_len, dc_code, ac_code;
        ComPtr<ID3D11ShaderResourceView> lut_srv, descale_srv, dc_len_srv, ac_len_srv, dc_code_srv, ac_code_srv;
        std::mutex lock;
        std::string name, error;

        // The hardware GPUs, fastest first (DXGI's high-performance order): index 0 is the default.
        // ZRAWDNG_SOFTWARE_GPU=1 adds Windows' software rasterizer (WARP) at the end, which lets a
        // one-GPU machine test the multi-GPU path.
        static std::vector<ComPtr<IDXGIAdapter1>> Adapters()
        {
            std::vector<ComPtr<IDXGIAdapter1>> list, soft;
            ComPtr<IDXGIFactory1> f1;
            ComPtr<IDXGIFactory6> f6;
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f1)))) return list;
            f1.As(&f6);
            for (UINT i = 0;; ++i)
            {
                ComPtr<IDXGIAdapter1> a;
                if (f6 ? FAILED(f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a))) : f1->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_ADAPTER_DESC1 d;
                a->GetDesc1(&d);
                (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE ? soft : list).push_back(a);
            }
            const char* s = getenv("ZRAWDNG_SOFTWARE_GPU");
            if (s && *s == '1') list.insert(list.end(), soft.begin(), soft.end());
            return list;
        }

        static std::string Name(IDXGIAdapter1* a)
        {
            DXGI_ADAPTER_DESC1 desc;
            a->GetDesc1(&desc);
            char nm[128];
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nm, sizeof nm, nullptr, nullptr);
            return nm;
        }

        // The device on GPU `index` of Adapters(), created once. nullptr when it cannot be used, with
        // the reason in *why; `error` on a returned object is never set.
        static Device* Get(int index, std::string* why = nullptr)
        {
            static std::mutex m;
            static std::map<int, std::unique_ptr<Device>> made;
            static std::map<int, std::string> failures;
            std::lock_guard<std::mutex> l(m);
            if (!made.count(index) && !failures.count(index))
            {
                auto x = std::make_unique<Device>();
                if (x->init(index)) made[index] = std::move(x); else failures[index] = x->error;
            }
            if (made.count(index)) return made[index].get();
            if (why) *why = failures[index];
            return nullptr;
        }

        static Device* Get(std::string* why = nullptr) { return Get(0, why); }   // the fastest GPU

    private:
        bool fail(const std::string& e) { error = e; return false; }

        bool init(int index)
        {
            auto adapters = Adapters();
            if (adapters.empty()) return fail("no hardware GPU");
            if (index < 0 || index >= (int)adapters.size()) return fail("there is no GPU " + std::to_string(index));
            ComPtr<IDXGIAdapter1> adapter = adapters[index];
            name = Name(adapter.Get());

            D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
            if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &want, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
                return fail("D3D11 device creation failed on " + name);
            {
                ComPtr<ID3D11Device5> dev5;
                if (FAILED(dev.As(&dev5)) || FAILED(ctx.As(&ctx4)) || FAILED(dev5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
                    return fail("D3D11.3 fences not available on " + name);
            }

            auto compile = [&](const char* entry, ComPtr<ID3D11ComputeShader>& cs) {
                ComPtr<ID3DBlob> code, errs;
                if (FAILED(D3DCompile(kShader, sizeof kShader - 1, "gpu_lossy.hlsl", nullptr, nullptr, entry, "cs_5_0",
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errs)))
                    return fail(std::string("shader compile failed: ") + (errs ? (const char*)errs->GetBufferPointer() : entry));
                return SUCCEEDED(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs)) || fail("CreateComputeShader failed");
            };
            if (!compile("dct_main", dct) || !compile("count_main", count) || !compile("bits_main", bits) ||
                !compile("scan_main", scan) || !compile("emit_main", emit)) return false;

            const lossy::Curve& cv = lossy::curve();
            std::vector<uint32_t> lut32(cv.encode, cv.encode + 16384);
            const lossy::Tables& tb = lossy::tables();
            uint32_t dcl[16], acl[256], dcc[16], acc[256];
            for (int i = 0; i < 16; ++i) { dcl[i] = tb.dc.len[i]; dcc[i] = tb.dc.code[i]; }
            for (int i = 0; i < 256; ++i) { acl[i] = tb.ac.len[i]; acc[i] = tb.ac.code[i]; }
            const UINT srv = D3D11_BIND_SHADER_RESOURCE;
            bool ok = make_buffer(dev.Get(), (UINT)(lut32.size() * 4), srv, D3D11_USAGE_IMMUTABLE, 0, 4, lut32.data(), lut) && make_srv(dev.Get(), lut.Get(), lut_srv) &&
                      make_buffer(dev.Get(), sizeof(float) * 64, srv, D3D11_USAGE_IMMUTABLE, 0, 4, lossy::aan().f, descale) && make_srv(dev.Get(), descale.Get(), descale_srv) &&
                      make_buffer(dev.Get(), sizeof dcl, srv, D3D11_USAGE_IMMUTABLE, 0, 4, dcl, dc_len) && make_srv(dev.Get(), dc_len.Get(), dc_len_srv) &&
                      make_buffer(dev.Get(), sizeof acl, srv, D3D11_USAGE_IMMUTABLE, 0, 4, acl, ac_len) && make_srv(dev.Get(), ac_len.Get(), ac_len_srv) &&
                      make_buffer(dev.Get(), sizeof dcc, srv, D3D11_USAGE_IMMUTABLE, 0, 4, dcc, dc_code) && make_srv(dev.Get(), dc_code.Get(), dc_code_srv) &&
                      make_buffer(dev.Get(), sizeof acc, srv, D3D11_USAGE_IMMUTABLE, 0, 4, acc, ac_code) && make_srv(dev.Get(), ac_code.Get(), ac_code_srv);
            return ok || fail("GPU table upload failed");
        }
    };

    // Per worker thread: its own buffers on the shared device.
    // A worker holds the device lock only while issuing commands. It waits for completion on a D3D11.3
    // fence event outside the lock (no polling, no CPU), and the CFA upload and coefficient copy-out
    // also happen outside the lock, so the GPU pipelines several workers' frames.
    class Backend : public lossy::Backend
    {
        Device& g;
        ComPtr<ID3D11Buffer> params, cfa, cfa_up, coefs, inv_q, totals, totals_stage, blk_bits, stream, stream_stage;
        ComPtr<ID3D11ShaderResourceView> cfa_srv, inv_q_srv;
        ComPtr<ID3D11UnorderedAccessView> coefs_uav, totals_uav, blk_bits_uav, stream_uav;
        size_t cfa_words = 0, coef_words = 0;
        Params p = {};
        HANDLE ev;

        bool ensure(size_t words, size_t cwords)
        {
            ID3D11Device* dev = g.dev.Get();
            const UINT srv = D3D11_BIND_SHADER_RESOURCE, uav = D3D11_BIND_UNORDERED_ACCESS;
            if (!params && !make_buffer(dev, sizeof(Params), D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DEFAULT, 0, 0, nullptr, params)) return false;
            if (!inv_q && !(make_buffer(dev, kScales * 64 * 4, srv, D3D11_USAGE_DEFAULT, 0, 4, nullptr, inv_q) && make_srv(dev, inv_q.Get(), inv_q_srv))) return false;
            if (!totals && !(make_buffer(dev, kScales * 4, uav, D3D11_USAGE_DEFAULT, 0, 4, nullptr, totals) && make_uav(dev, totals.Get(), totals_uav) &&
                             make_buffer(dev, kScales * 4, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, 0, nullptr, totals_stage))) return false;
            if (words != cfa_words)
            {
                cfa.Reset(); cfa_srv.Reset(); cfa_up.Reset();
                if (!(make_buffer(dev, (UINT)(words * 4), srv, D3D11_USAGE_DEFAULT, 0, 4, nullptr, cfa) && make_srv(dev, cfa.Get(), cfa_srv) &&
                      make_buffer(dev, (UINT)(words * 4), 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_WRITE, 0, nullptr, cfa_up))) return false;
                cfa_words = words;
            }
            if (cwords != coef_words)
            {
                // Stream space per tile (p.cap_words): the tile as 12-bit raw. A frame that needs more is encoded on the CPU.
                const size_t sw = (size_t)p.cap_words * lossy::Coefficients().tiles;
                coefs.Reset(); coefs_uav.Reset(); blk_bits.Reset(); blk_bits_uav.Reset(); stream.Reset(); stream_uav.Reset(); stream_stage.Reset();
                if (!(make_buffer(dev, (UINT)(cwords * 4), uav, D3D11_USAGE_DEFAULT, 0, 4, nullptr, coefs) && make_uav(dev, coefs.Get(), coefs_uav) &&
                      make_buffer(dev, (UINT)(cwords / 32 * 4), uav, D3D11_USAGE_DEFAULT, 0, 4, nullptr, blk_bits) && make_uav(dev, blk_bits.Get(), blk_bits_uav) &&
                      make_buffer(dev, (UINT)(sw * 4), uav, D3D11_USAGE_DEFAULT, 0, 4, nullptr, stream) && make_uav(dev, stream.Get(), stream_uav) &&
                      make_buffer(dev, (UINT)(sw * 4), 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, 0, nullptr, stream_stage))) return false;
                coef_words = cwords;
            }
            return true;
        }

        void bind(ID3D11ComputeShader* cs)
        {
            ID3D11DeviceContext* c = g.ctx.Get();
            ID3D11ShaderResourceView* srvs[8] = {cfa_srv.Get(), g.lut_srv.Get(), g.descale_srv.Get(), inv_q_srv.Get(), g.dc_len_srv.Get(), g.ac_len_srv.Get(),
                                                 g.dc_code_srv.Get(), g.ac_code_srv.Get()};
            ID3D11UnorderedAccessView* uavs[4] = {coefs_uav.Get(), totals_uav.Get(), blk_bits_uav.Get(), stream_uav.Get()};
            c->CSSetShader(cs, nullptr, 0);
            c->CSSetShaderResources(0, 8, srvs);
            c->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
            c->CSSetConstantBuffers(0, 1, params.GetAddressOf());
        }

        std::unique_lock<std::mutex> take() { return std::unique_lock<std::mutex>(g.lock); }

        // Under the lock: signals the fence after the queued work and arms this worker's event.
        uint64_t signal()
        {
            uint64_t v = ++g.fence_value;
            g.ctx4->Signal(g.fence.Get(), v);
            g.fence->SetEventOnCompletion(v, ev);
            g.ctx->Flush();
            return v;
        }
        void wait() { WaitForSingleObject(ev, INFINITE); }

        // Exact bits for each of `n` scales (n <= kScales) of the frame whose coefficients are on the GPU.
        bool count(const double* scales, int n, uint64_t* bits)
        {
            float inv[kScales * 64];
            for (int i = 0; i < kScales; ++i)
            {
                uint16_t q[64];
                lossy::QuantTable(scales[std::min(i, n - 1)], q);
                lossy::InverseTable(q, inv + 64 * i);
            }
            {
                auto l = take();
                ID3D11DeviceContext* c = g.ctx.Get();
                p.num_scales = n;
                c->UpdateSubresource(params.Get(), 0, nullptr, &p, 0, 0);
                c->UpdateSubresource(inv_q.Get(), 0, nullptr, inv, 0, 0);
                UINT zero[4] = {0, 0, 0, 0};
                c->ClearUnorderedAccessViewUint(totals_uav.Get(), zero);
                bind(g.count.Get());
                c->Dispatch((p.blocks + 63) / 64, n, 1);
                c->CopyResource(totals_stage.Get(), totals.Get());
                signal();
            }
            wait();
            {
                auto l = take();
                D3D11_MAPPED_SUBRESOURCE m;
                if (FAILED(g.ctx->Map(totals_stage.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
                for (int i = 0; i < n; ++i) bits[i] = ((const uint32_t*)m.pData)[i];
                g.ctx->Unmap(totals_stage.Get(), 0);
            }
            return true;
        }

    public:
        explicit Backend(Device& d) : g(d), ev(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
        ~Backend() { CloseHandle(ev); }

        bool Transform(const uint16_t* cfa_in, int W, int H, int bits, lossy::Coefficients& co) override
        {
            co.tile_w = lossy::TileWidth(W);
            co.jw = co.tile_w * 2;
            co.jh = H / 2;
            co.bw = (co.jw + 7) / 8;
            co.bh = (co.jh + 7) / 8;
            const size_t blocks = (size_t)co.tiles * co.bw * co.bh;
            p = {(uint32_t)W, (uint32_t)H, (uint32_t)co.tile_w, (uint32_t)co.jw, (uint32_t)co.jh, (uint32_t)co.bw, (uint32_t)co.bh, (uint32_t)blocks,
                 (uint32_t)lossy::CurveShift(bits), 0, 0,
                 (uint32_t)((size_t)co.tile_w * H * 12 / 32)};
            ID3D11DeviceContext* c = g.ctx.Get();
            D3D11_MAPPED_SUBRESOURCE m;
            {
                auto l = take();
                if (!ensure((size_t)W * H / 2, blocks * 32)) return false;
                if (FAILED(c->Map(cfa_up.Get(), 0, D3D11_MAP_WRITE, 0, &m))) return false;   // this worker's staging buffer: idle since its last frame
            }
            memcpy(m.pData, cfa_in, (size_t)W * H * 2);   // outside the lock
            {
                auto l = take();
                c->Unmap(cfa_up.Get(), 0);
                c->CopyResource(cfa.Get(), cfa_up.Get());
                c->UpdateSubresource(params.Get(), 0, nullptr, &p, 0, 0);
                bind(g.dct.Get());
                c->Dispatch((UINT)((blocks + 63) / 64), 1, 1);   // the coefficients stay on the GPU for Search and EncodeTiles
            }
            return true;
        }

        // Measures 16 scales spread around the start in one dispatch, then 16 between the two that
        // bracket the target, and picks the one closest to it (preferring the smaller file).
        double Search(lossy::Coefficients&, double target_bytes, double start, double stuffing, int& passes) override
        {
            auto bytes = [&](uint64_t b) { return (double)b / 8.0 * stuffing + 4 * 660; };
            double s[kScales];
            uint64_t b[kScales];
            double lo = start / 3, hi = start * 3;   // geometric spread, widened if the target lies outside
            for (int round = 0; round < 6; ++round)
            {
                for (int i = 0; i < kScales; ++i) s[i] = std::max(0.5, lo * std::pow(hi / lo, i / double(kScales - 1)));
                if (!count(s, kScales, b)) return start;
                ++passes;
                if (bytes(b[0]) < target_bytes && s[0] <= 0.5) return 0.5;          // small even at the finest step
                if (bytes(b[0]) < target_bytes) { hi = lo; lo /= 9; continue; }     // target needs a finer scale
                if (bytes(b[kScales - 1]) > target_bytes) { lo = hi; hi *= 9; continue; }
                int i = 0;
                while (bytes(b[i + 1]) > target_bytes) ++i;   // b[i] > target >= b[i+1]
                if (round == 0 || s[i + 1] / s[i] > 1.002) { lo = s[i]; hi = s[i + 1]; if (round == 0) continue; }
                // closest of the measured scales, ties to the smaller file
                int best = i + 1;
                if (std::fabs(bytes(b[i]) - target_bytes) < std::fabs(bytes(b[i + 1]) - target_bytes) && bytes(b[i]) / target_bytes - 1 < 0.01) best = i;
                return s[best];
            }
            return hi;
        }

        bool EncodeTiles(lossy::Coefficients& co, const uint16_t* q, std::vector<uint8_t>* tiles, uint64_t& counted_bits) override
        {
            float inv[64];
            lossy::InverseTable(q, inv);
            const size_t cap = p.cap_words;
            {
                auto l = take();
                ID3D11DeviceContext* c = g.ctx.Get();
                p.num_scales = 1;
                c->UpdateSubresource(params.Get(), 0, nullptr, &p, 0, 0);
                D3D11_BOX box = {0, 0, 0, sizeof inv, 1, 1};
                c->UpdateSubresource(inv_q.Get(), 0, &box, inv, 0, 0);
                UINT zero[4] = {0, 0, 0, 0};
                c->ClearUnorderedAccessViewUint(stream_uav.Get(), zero);
                const UINT groups = (p.blocks + 63) / 64;
                bind(g.bits.Get()); c->Dispatch(groups, 1, 1);
                bind(g.scan.Get()); c->Dispatch((UINT)co.tiles, 1, 1);
                bind(g.emit.Get()); c->Dispatch(groups, 1, 1);
                c->CopyResource(totals_stage.Get(), totals.Get());
                c->CopyResource(stream_stage.Get(), stream.Get());
                signal();
            }
            wait();
            D3D11_MAPPED_SUBRESOURCE mt, ms;
            {
                auto l = take();
                if (FAILED(g.ctx->Map(totals_stage.Get(), 0, D3D11_MAP_READ, 0, &mt))) return false;
                if (FAILED(g.ctx->Map(stream_stage.Get(), 0, D3D11_MAP_READ, 0, &ms))) { g.ctx->Unmap(totals_stage.Get(), 0); return false; }
            }
            // Outside the lock: headers, byte stuffing, the last byte padded with 1 bits, EOI
            bool ok = true;
            for (int t = 0; t < co.tiles; ++t)
            {
                const uint32_t tb = ((const uint32_t*)mt.pData)[t];
                if (tb > cap * 32) { ok = false; break; }   // did not fit: the caller redoes the frame on the CPU
                const uint8_t* s = (const uint8_t*)ms.pData + (size_t)t * cap * 4;
                lossy::TileHeader(co, q, tiles[t]);
                lossy::AppendStuffed(tiles[t], s, tb / 8);
                if (tb % 8) { uint8_t last = (uint8_t)(s[tb / 8] | (0xFF >> (tb % 8))); lossy::AppendStuffed(tiles[t], &last, 1); }
                tiles[t].insert(tiles[t].end(), {0xFF, 0xD9});
                counted_bits += tb;
            }
            {
                auto l = take();
                g.ctx->Unmap(stream_stage.Get(), 0);
                g.ctx->Unmap(totals_stage.Get(), 0);
            }
            return ok;
        }
    };
}

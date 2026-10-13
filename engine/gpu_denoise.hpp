// Temporal raw denoise on the GPU: the HDR+ merge (Hasinoff et al. 2016) on the four Bayer planes,
// in a variance-stabilized domain built from the measured E2 noise model.
//
// Each decoded frame is uploaded once into a slot of a Store on the GPU. A merge reads the reference
// frame and its neighbors from their slots. Per 16x16 tile (stride 8, raised-cosine window) and
// color plane, it:
//   1. applies the generalized Anscombe transform, which makes the noise unit-variance;
//   2. takes a 2D DFT of each frame's tile;
//   3. pulls every neighbor toward the reference per frequency by |D|^2 / (|D|^2 + temporal x noise),
//      so where a neighbor differs from the reference by more than noise (it moved), the reference wins;
//   4. averages the frames;
//   5. applies a light Wiener shrink that removes part of the remaining noise (the tile mean never
//      changes);
//   6. writes the inverse DFT to a tile buffer.
// A second pass adds the four tiles over each photosite, undoes the transform and writes the CFA.
// Alignment is one integer offset per neighbor (whole frame), found on the CPU (Align below).
#pragma once
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include "gpu_lossy.hpp"

namespace gpu_denoise
{
    using Microsoft::WRL::ComPtr;
    using gpu_lossy::make_buffer;
    using gpu_lossy::make_srv;
    using gpu_lossy::make_uav;

    static const char kShader[] = R"HLSL(
cbuffer Params : register(b0)
{
    uint W, H, pw, ph;            // frame and plane size
    uint nfr, slot_words, tiles_x, tiles_y;
    float black, a, f2, temporal;
    float spatial, noise_var, pad0, pad1;
};
StructuredBuffer<uint> frames : register(t0);   // Store: slots of W x H 16-bit samples, two per uint
StructuredBuffer<int4> info   : register(t1);   // per window frame: slot, dy, dx, is_reference
RWStructuredBuffer<float> tiles : register(u0); // 4 planes x tiles_y x tiles_x x 256
RWStructuredBuffer<uint> outcfa : register(u1); // W x H / 2

static const float PI = 3.14159265358979;
// cos and sin of 2 pi k / 16, the DFT's twiddles, in group shared memory (filled once per tile):
// computing them per step was the slowest part, and a constant array indexed per thread is worse
groupshared float CS[16], SN[16];
groupshared float sre[256], sim[256];

int reflect(int i, int n) { i = i < 0 ? -i - 1 : i; return i >= n ? 2 * n - 1 - i : i; }

float sample_cfa(uint slot, int y, int x)
{
    uint i = slot * slot_words * 2 + (uint)y * W + (uint)x;
    uint w = frames[i >> 1];
    return (float)((i & 1) ? (w >> 16) : (w & 0xFFFF));
}

float vst(float v) { return 2.0 / a * sqrt(max(a * v + 0.375 * a * a + f2, 0.0)); }
float win1(uint i) { return 0.5 - 0.5 * cos(2.0 * PI * (i + 0.5) / 16.0); }

float2 cmul(float2 a, float2 b) { return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }
float2 twiddle(uint t, float sign) { return float2(CS[t & 15], sign * SN[t & 15]); }   // exp(sign i 2 pi t / 16)

// One 16-point DFT along a line of sre/sim (elements base + j * stride), radix 4 in two stages; the
// thread at position i of the line gets output i. Every thread of the group calls it (barriers).
float2 fft16(uint base, uint stride, uint i, float sign, float2 v)
{
    sre[base + i * stride] = v.x; sim[base + i * stride] = v.y;
    GroupMemoryBarrierWithGroupSync();
    // stage 1: n = 4 n1 + n2, this thread makes Y[n2][k1] with n2 = i / 4, k1 = i % 4
    uint n2 = i >> 2, k1 = i & 3;
    float2 acc = 0;
    [unroll] for (uint n1 = 0; n1 < 4; ++n1)
    {
        uint e = base + (4 * n1 + n2) * stride;
        acc += cmul(float2(sre[e], sim[e]), twiddle(4 * n1 * k1, sign));
    }
    acc = cmul(acc, twiddle(n2 * k1, sign));
    GroupMemoryBarrierWithGroupSync();
    sre[base + i * stride] = acc.x; sim[base + i * stride] = acc.y;   // stored at n2 * 4 + k1 = i
    GroupMemoryBarrierWithGroupSync();
    // stage 2: output k = k1 + 4 k2 with k1 = i % 4, k2 = i / 4
    uint q1 = i & 3, k2 = i >> 2;
    float2 res = 0;
    [unroll] for (uint m2 = 0; m2 < 4; ++m2)
    {
        uint e = base + (m2 * 4 + q1) * stride;
        res += cmul(float2(sre[e], sim[e]), twiddle(4 * m2 * k2, sign));
    }
    GroupMemoryBarrierWithGroupSync();
    return res;
}

// 16x16 DFT, thread (x, y) owning element [y][x]; sign -1 forward, +1 inverse (unscaled)
void dft(uint x, uint y, float sign, inout float re, inout float im)
{
    float2 v = fft16(y * 16, 1, x, sign, float2(re, im));   // along x
    v = fft16(x, 16, y, sign, v);                            // along y
    re = v.x; im = v.y;
}

groupshared float zr[256], zi[256];

// Splits Z, the spectrum of (a + i b) with a and b both real, into the spectra of a and of b
void split(uint x, uint y, float r, float i, out float2 A, out float2 B)
{
    zr[y * 16 + x] = r; zi[y * 16 + x] = i;
    GroupMemoryBarrierWithGroupSync();
    uint m = ((16 - y) & 15) * 16 + ((16 - x) & 15);   // the mirrored frequency
    float mr = zr[m], mi = zi[m];
    GroupMemoryBarrierWithGroupSync();
    A = float2(r + mr, i - mi) * 0.5;
    B = float2(i + mi, mr - r) * 0.5;
}

// One tile of two planes of one frame (row parity `pair`, both columns), transformed together as
// one complex tile (plane 2 pair as the real part, 2 pair + 1 as the imaginary part) and split
void tile_spectra(uint x, uint y, uint pair, uint slot, int py, int px, float wgt, out float2 A, out float2 B)
{
    int sy = reflect(py, (int)ph), sx = reflect(px, (int)pw);
    float re = vst(sample_cfa(slot, 2 * sy + (int)pair, 2 * sx) - black) * wgt;
    float im = vst(sample_cfa(slot, 2 * sy + (int)pair, 2 * sx + 1) - black) * wgt;
    dft(x, y, -1.0, re, im);
    split(x, y, re, im, A, B);
}

float2 pull(float2 R, float2 F)   // the alternate frame's coefficient pulled toward the reference's
{
    float2 d = R - F;
    float d2 = dot(d, d);
    return F + d * (d2 / (d2 + temporal * noise_var));
}

[numthreads(16, 16, 1)]
void merge_main(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID)
{
    const uint pair = g.z, x = t.x, y = t.y;
    if (y == 0) sincos(2.0 * PI * x / 16.0, SN[x], CS[x]);
    GroupMemoryBarrierWithGroupSync();
    const int py = (int)g.y * 8 - 8 + (int)y, px = (int)g.x * 8 - 8 + (int)x;   // tiles start half a tile before the plane
    const float wgt = win1(x) * win1(y);
    float2 RA = 0, RB = 0, accA = 0, accB = 0;
    uint n = 0;
    for (uint f = 0; f < nfr; ++f)   // the reference first
    {
        int4 fi = info[f];
        if (fi.w == 0) continue;
        tile_spectra(x, y, pair, (uint)fi.x, py, px, wgt, RA, RB);
        accA = RA; accB = RB; n = 1;
    }
    for (uint j = 0; j < nfr; ++j)
    {
        int4 fi = info[j];
        if (fi.w != 0) continue;
        float2 A, B;
        tile_spectra(x, y, pair, (uint)fi.x, py + fi.y, px + fi.z, wgt, A, B);
        accA += pull(RA, A); accB += pull(RB, B);
        n += 1;
    }
    accA /= n; accB /= n;
    if (spatial > 0 && (x | y) != 0)   // a light Wiener shrink of what noise is left; never the tile mean
    {
        float mA = dot(accA, accA), mB = dot(accB, accB), left = spatial * noise_var / n;
        accA *= mA / (mA + left); accB *= mB / (mB + left);
    }
    float re = accA.x - accB.y, im = accA.y + accB.x;   // A + i B, back to one complex tile
    dft(x, y, 1.0, re, im);
    tiles[(((2 * pair) * tiles_y + g.y) * tiles_x + g.x) * 256 + y * 16 + x] = re / 256.0;
    tiles[(((2 * pair + 1) * tiles_y + g.y) * tiles_x + g.x) * 256 + y * 16 + x] = im / 256.0;
}

float plane_value(uint plane, int py, int px)
{
    // the four tiles over this plane pixel: tiles start at ty * 8 - 8
    int ty0 = py >> 3, tx0 = px >> 3;
    int ly0 = py - ty0 * 8 + 8, lx0 = px - tx0 * 8 + 8;   // local position in tile ty0 / tx0 (8..15)
    float s = 0;
    [unroll] for (int j = 0; j < 2; ++j)
    [unroll] for (int i = 0; i < 2; ++i)
    {
        uint ty = (uint)(ty0 + j), tx = (uint)(tx0 + i);
        uint ly = (uint)(ly0 - 8 * j), lx = (uint)(lx0 - 8 * i);
        s += tiles[((plane * tiles_y + ty) * tiles_x + tx) * 256 + ly * 16 + lx];
    }
    return s;
}

float ivst(float yv) { float q = a * yv * 0.5; return (q * q - 0.375 * a * a - f2) / a; }

uint ref_slot() { for (uint j = 0; j < nfr; ++j) if (info[j].w != 0) return (uint)info[j].x; return 0; }

// A photosite clipped in the frame being denoised keeps its value. Its real level is unknown, and a
// merged value under the sensor's clip (4094, the DNGs' WhiteLevel) would hide it from highlight
// recovery in the raw developer.
uint keep_clipped(float merged, float original) { return original >= 4094.0 ? (uint)original : (uint)clamp(round(merged), 0.0, 4095.0); }

[numthreads(64, 1, 1)]
void compose_main(uint3 id : SV_DispatchThreadID)
{
    uint i = id.x;                       // one output uint = two horizontally adjacent photosites
    if (i >= W * H / 2) return;
    uint y = (i * 2) / W, x = (i * 2) % W;
    uint row = y & 1, rs = ref_slot();
    float v0 = ivst(plane_value(row * 2 + 0, (int)(y >> 1), (int)(x >> 1))) + black;
    float v1 = ivst(plane_value(row * 2 + 1, (int)(y >> 1), (int)(x >> 1))) + black;
    uint a0 = keep_clipped(v0, sample_cfa(rs, (int)y, (int)x)), a1 = keep_clipped(v1, sample_cfa(rs, (int)y, (int)x + 1));
    outcfa[i] = a0 | (a1 << 16);
}
)HLSL";

    struct Params { uint32_t W, H, pw, ph, nfr, slot_words, tiles_x, tiles_y; float black, a, f2, temporal, spatial, noise_var, pad0, pad1; };

    struct Settings { int radius = 2; float temporal = 8, spatial = 0.5f; };

    // Frame slots on the GPU, shared by every merge of a clip; plus the compiled kernels.
    class Store
    {
    public:
        gpu_lossy::Device& g;
        ComPtr<ID3D11ComputeShader> merge, compose;
        ComPtr<ID3D11Buffer> frames;
        ComPtr<ID3D11ShaderResourceView> frames_srv;
        uint32_t W = 0, H = 0, slots = 0;
        std::string error;

        Store(gpu_lossy::Device& d, uint32_t w, uint32_t h, uint32_t nslots) : g(d), W(w), H(h), slots(nslots)
        {
            std::lock_guard<std::mutex> l(g.lock);
            auto compile = [&](const char* entry, ComPtr<ID3D11ComputeShader>& cs) {
                ComPtr<ID3DBlob> code, errs;
                if (FAILED(D3DCompile(kShader, sizeof kShader - 1, "gpu_denoise.hlsl", nullptr, nullptr, entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errs)))
                { error = std::string("denoise shader: ") + (errs ? (const char*)errs->GetBufferPointer() : entry); return false; }
                return SUCCEEDED(g.dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs));
            };
            if (!compile("merge_main", merge) || !compile("compose_main", compose)) return;
            if (!(make_buffer(g.dev.Get(), (UINT)((size_t)W * H / 2 * slots * 4), D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, 4, nullptr, frames) &&
                  make_srv(g.dev.Get(), frames.Get(), frames_srv)))
                error = "not enough GPU memory for the denoise frame store";
        }
        bool ok() const { return error.empty(); }

        // Copies a decoded frame (W x H 16-bit samples) into a slot
        void Upload(uint32_t slot, const uint16_t* cfa)
        {
            std::lock_guard<std::mutex> l(g.lock);
            const UINT words = W * H / 2;
            D3D11_BOX box = {slot * words * 4, 0, 0, (slot + 1) * words * 4, 1, 1};
            g.ctx->UpdateSubresource(frames.Get(), 0, &box, cfa, 0, 0);
        }
    };

    // One merge context: its own tile and output buffers (about 150 MB for UHD). A few are shared by
    // all the workers (see convert_clip in main.cpp).
    class Merger
    {
        Store& st;
        ComPtr<ID3D11Buffer> params, info, tiles, out, out_stage;
        ComPtr<ID3D11ShaderResourceView> info_srv;
        ComPtr<ID3D11UnorderedAccessView> tiles_uav, out_uav;
        HANDLE ev;
        uint32_t tx, ty;
    public:
        explicit Merger(Store& s) : st(s), ev(CreateEventW(nullptr, FALSE, FALSE, nullptr))
        {
            ID3D11Device* dev = st.g.dev.Get();
            tx = st.W / 2 / 8 + 1; ty = st.H / 2 / 8 + 1;
            const UINT srv = D3D11_BIND_SHADER_RESOURCE, uav = D3D11_BIND_UNORDERED_ACCESS;
            ok_ = make_buffer(dev, sizeof(Params), D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DEFAULT, 0, 0, nullptr, params) &&
                  make_buffer(dev, 8 * 16, srv, D3D11_USAGE_DEFAULT, 0, 16, nullptr, info) && make_srv(dev, info.Get(), info_srv) &&
                  make_buffer(dev, (UINT)((size_t)4 * tx * ty * 256 * 4), uav, D3D11_USAGE_DEFAULT, 0, 4, nullptr, tiles) && make_uav(dev, tiles.Get(), tiles_uav) &&
                  make_buffer(dev, st.W * st.H / 2 * 4, uav, D3D11_USAGE_DEFAULT, 0, 4, nullptr, out) && make_uav(dev, out.Get(), out_uav) &&
                  make_buffer(dev, st.W * st.H / 2 * 4, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, 0, nullptr, out_stage);
        }
        ~Merger() { CloseHandle(ev); }
        bool ok() const { return ok_; }

        // slots[i] and offs[i] (dy, dx in plane pixels) describe each window frame; ref is the reference's index.
        bool Run(const std::vector<uint32_t>& slots, const std::vector<std::pair<int, int>>& offs, int ref, double black, double a, double f2,
                 const Settings& s, uint16_t* cfa_out)
        {
            int32_t fi[8][4] = {};
            for (size_t i = 0; i < slots.size() && i < 8; ++i) { fi[i][0] = (int32_t)slots[i]; fi[i][1] = offs[i].first; fi[i][2] = offs[i].second; fi[i][3] = (int)i == ref; }
            Params p = {st.W, st.H, st.W / 2, st.H / 2, (uint32_t)slots.size(), st.W * st.H / 2, tx, ty,
                        (float)black, (float)a, (float)f2, s.temporal, s.spatial, 36.0f, 0, 0};   // noise_var: (sum of the 1D window squared = 6)^2
            auto& g = st.g;
            {
                std::lock_guard<std::mutex> l(g.lock);
                ID3D11DeviceContext* c = g.ctx.Get();
                c->UpdateSubresource(params.Get(), 0, nullptr, &p, 0, 0);
                c->UpdateSubresource(info.Get(), 0, nullptr, fi, 0, 0);
                ID3D11ShaderResourceView* srvs[2] = {st.frames_srv.Get(), info_srv.Get()};
                ID3D11UnorderedAccessView* uavs[2] = {tiles_uav.Get(), out_uav.Get()};
                c->CSSetShaderResources(0, 2, srvs);
                c->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
                c->CSSetConstantBuffers(0, 1, params.GetAddressOf());
                c->CSSetShader(st.merge.Get(), nullptr, 0);
                c->Dispatch(tx, ty, 2);   // two planes per group
                c->CSSetShader(st.compose.Get(), nullptr, 0);
                c->Dispatch((st.W * st.H / 2 + 63) / 64, 1, 1);
                ID3D11UnorderedAccessView* none[2] = {};
                c->CSSetUnorderedAccessViews(0, 2, none, nullptr);   // the lossy backend binds its own buffers next
                c->CopyResource(out_stage.Get(), out.Get());
                uint64_t v = ++g.fence_value;
                g.ctx4->Signal(g.fence.Get(), v);
                g.fence->SetEventOnCompletion(v, ev);
                g.ctx->Flush();
            }
            WaitForSingleObject(ev, INFINITE);
            D3D11_MAPPED_SUBRESOURCE m;
            {
                std::lock_guard<std::mutex> l(g.lock);
                if (FAILED(g.ctx->Map(out_stage.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
            }
            memcpy(cfa_out, m.pData, (size_t)st.W * st.H * 2);
            {
                std::lock_guard<std::mutex> l(g.lock);
                g.ctx->Unmap(out_stage.Get(), 0);
            }
            return true;
        }
    private:
        bool ok_ = false;
    };

    // Whole-frame integer offset (in plane pixels, within +-r) of `alt` against `ref`: least squares on
    // one green plane, every 8th plane pixel each way (a whole-frame shift needs no more, and every 4th
    // would make alignment the slowest part of the CPU side). Both are full CFA frames.
    inline std::pair<int, int> Align(const uint16_t* ref, const uint16_t* alt, int W, int H, int r = 3)
    {
        const int pw = W / 2, ph = H / 2, m = 64;
        int64_t best = INT64_MAX;
        std::pair<int, int> out(0, 0);
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx)
            {
                int64_t s = 0;
                for (int y = m; y < ph - m; y += 8)
                {
                    const uint16_t* a = ref + (size_t)(2 * y) * W + 1;
                    const uint16_t* b = alt + (size_t)(2 * (y + dy)) * W + 1 + 2 * dx;
                    for (int x = m; x < pw - m; x += 8) { int d = (int)a[2 * x] - b[2 * x]; s += d * d; }
                }
                if (s < best) { best = s; out = {dy, dx}; }
            }
        return out;
    }
}

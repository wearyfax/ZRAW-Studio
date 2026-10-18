// Lossy CinemaDNG in Blackmagic's 3:1 / 4:1 format, the lossy raw flavor DaVinci Resolve reads.
// The layout is copied from Blackmagic Pocket Cinema Camera 4K files (raw.pixls.us samples):
//   - Compression 7, CFA, 12 bits, a LinearizationTable holding Blackmagic's log curve (zrawdng
//     defaults to a noise-matched curve instead, see Curve).
//   - 4 tiles, each a quarter of the width and the full height.
//   - Each tile is one 12-bit extended-DCT JPEG (SOF1), one component, with no restart markers.
//     Two image rows sit side by side in one JPEG row: a W/4 x H tile is coded as (W/2) x (H/2).
//   - Segment order SOI, COM(99 99 99 99), DQT (16-bit), Blackmagic's two Huffman tables, SOF1, SOS.
// The compression ratio is the 12-bit packed raw size over the tile data size, as Blackmagic counts it.
// The encoder searches for a per-frame scale on Blackmagic's quantization table that brings the frame to that size.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "portable.hpp"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cstdio>
#endif
#include "zraw.hpp"
#include "camera_color.hpp"
#include "movparse.hpp"
#include "bitwriter.hpp"

namespace lossy
{
    // ---- Blackmagic's 12-bit log curve: codes 0..1024 linear, then each 512 codes double the value.
    inline uint32_t CurveToLinear(int c)
    {
        if (c <= 1024) return (uint32_t)c;
        int k = (c - 1024) / 512;
        return (1024u << k) + (uint32_t)(c - 1024 - 512 * k) * (2u << k);
    }

    struct Curve
    {
        uint16_t to_linear[4096];   // LinearizationTable: code -> 16-bit linear
        uint16_t encode[16384];     // 14-bit linear sample -> nearest code (12-bit samples index it times 4)
        Curve()
        {
            for (int c = 0; c < 4096; ++c) to_linear[c] = (uint16_t)CurveToLinear(c);
            fill_encode();
        }
        // Noise-matched curve, the default: codes follow sqrt(signal + 4 DN) above black and run on
        // straight down to 48 DN under it, so the sensor's noise spans about the same number of codes
        // at every level (shot noise grows as the square root; 4 DN is where read noise takes over
        // across the E2's ISOs). With Blackmagic's log curve the noise near black covers 3 to 5x fewer
        // codes than in the midtones, so the one quantization table per tile flattens shadows into
        // blocks first; that curve also spends half its codes below black. The curve travels in each
        // DNG's LinearizationTable, so readers need nothing new.
        explicit Curve(double black)
        {
            const double s0 = 4, lo = -48, r0 = std::sqrt(s0);
            auto u = [&](double s) { return s >= 0 ? std::sqrt(s + s0) - r0 : s / (2 * r0); };
            auto inv = [&](double v) { return v >= 0 ? (v + r0) * (v + r0) - s0 : v * 2 * r0; };
            const double k = 4095 / (u(4095 - black) - u(lo));
            for (int c = 0; c < 4096; ++c)
                to_linear[c] = (uint16_t)std::clamp(std::lround(16 * (black + inv(c / k + u(lo)))), 0L, 65535L);
            fill_encode();
        }
        void fill_encode()
        {
            for (int x = 0; x < 16384; ++x)
            {
                uint32_t v = (uint32_t)x * 4;   // 14-bit linear into the curve's 16-bit domain
                int c = (int)(std::lower_bound(to_linear, to_linear + 4096, v) - to_linear);
                if (c == 4096) c = 4095;
                else if (c > 0 && v - to_linear[c - 1] <= (uint32_t)to_linear[c] - v) --c;
                encode[x] = (uint16_t)c;
            }
        }
    };
    // One global curve built for the E2's black level (200 at 12 bits, 796 to 800 at 14); another
    // black only makes the shadows' code spacing a little less even, as long as it is above 48.
    constexpr double kCurveBlack = 200;
    inline const Curve*& active_curve() { static const Curve* c = new Curve(kCurveBlack); return c; }
    inline const Curve& curve() { return *active_curve(); }
    // Switches every later encode to Blackmagic's curve (--curve bmd). Call before any GPU device is
    // made: its lookup table is built once.
    inline void UseBlackmagicCurve() { active_curve() = new Curve(); }

    // ---- Tables copied from Blackmagic's files

    // Huffman tables, both DHT segments verbatim (DC table 0, then AC table 0). The DC table repeats
    // symbol 15 as filler; decoders accept that and the encoder below uses each symbol's first code.
    static const uint8_t kDht[494] = {
        0xFF, 0xC4, 0x00, 0xF5, 0x00, 0x00, 0x02, 0x02, 0x01, 0x03, 0x02, 0x04, 0x03, 0x03, 0x07, 0x06, 0x04, 0x00, 0x00, 0x02, 0xBB, 0x01, 0x02, 0x03,
        0x0F, 0x04, 0x05, 0x0F, 0x0F, 0x06, 0x0F, 0x00, 0x0F, 0x0F, 0x0F, 0x07, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x08, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
        0x09, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
        0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0A, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
        0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
        0x0F, 0x0F, 0x0F, 0x0B, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0C, 0x0D, 0x0E, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
        0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
        0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
        0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
        0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
        0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0xFF, 0xC4, 0x00, 0xF5, 0x10, 0x00, 0x02, 0x02, 0x01, 0x03, 0x02, 0x04, 0x03, 0x03, 0x07, 0x06, 0x04,
        0x00, 0x00, 0x02, 0xBB, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x12, 0x21, 0x06, 0x31, 0x00, 0x13, 0x22, 0x41, 0x07, 0x14, 0x51, 0x15, 0x32, 0x61,
        0x08, 0x16, 0x23, 0x24, 0x25, 0x42, 0x71, 0x09, 0x17, 0x33, 0x52, 0x81, 0x91, 0x26, 0x34, 0x35, 0x43, 0x45, 0xA1, 0xB1, 0xF0, 0x27, 0x36, 0x37,
        0x44, 0x62, 0xC1, 0xD1, 0x46, 0x47, 0x53, 0x54, 0x55, 0x72, 0x82, 0xE1, 0xF1, 0x0A, 0x56, 0x57, 0x63, 0x64, 0x65, 0x67, 0x73, 0x75, 0x83, 0x85,
        0x66, 0x74, 0x77, 0x84, 0x92, 0x18, 0x76, 0x87, 0x94, 0x97, 0xA2, 0xB2, 0xC2, 0xC4, 0xC5, 0x86, 0x93, 0x95, 0xA3, 0xA4, 0xA5, 0xA7, 0xB3, 0xB4,
        0xB5, 0xC3, 0xC7, 0xD2, 0x96, 0xB7, 0xC6, 0xA6, 0xB6, 0xE2, 0x0B, 0xF2, 0xD3, 0x19, 0x28, 0xD4, 0x0C, 0x0D, 0x0E, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E,
        0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x58, 0x59, 0x5A, 0x5B,
        0x5C, 0x5D, 0x5E, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E,
        0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xC8, 0xC9, 0xCA,
        0xCB, 0xCC, 0xCD, 0xCE, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC,
        0xED, 0xEE, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE,
    };

    // Quantization table of the 3:1 sample, in zigzag order. Its shape is the base the rate control scales.
    static const uint16_t kQuantShape[64] = {
        10, 12, 13, 12, 12, 11, 13, 11, 12, 12, 12, 12, 12, 12, 10, 14, 11, 12, 11, 12, 12, 12, 12, 12, 12, 11, 12, 10, 14, 11, 12, 12,
        12, 12, 12, 12, 12, 12, 12, 11, 12, 11, 12, 12, 12, 12, 11, 12, 12, 11, 12, 12, 11, 12, 12, 11, 12, 12, 12, 12, 12, 12, 12, 10,
    };

    // Natural (row-major) index of each zigzag position
    static const uint8_t kZigzag[64] = {
        0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
        35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
    };

    struct HuffCodes { uint16_t code[256]; uint8_t len[256]; };
    struct Tables
    {
        HuffCodes dc, ac;
        Tables()
        {
            memset(&dc, 0, sizeof dc);
            memset(&ac, 0, sizeof ac);
            for (size_t p = 0; p < sizeof kDht;)
            {
                const uint8_t* seg = kDht + p + 4;            // after FF C4 and the length
                size_t seglen = ((size_t)kDht[p + 2] << 8) | kDht[p + 3];
                HuffCodes& h = (seg[0] >> 4) ? ac : dc;
                const uint8_t* counts = seg + 1;
                const uint8_t* sym = seg + 17;
                uint32_t code = 0;
                for (int len = 1, k = 0; len <= 16; ++len, code <<= 1)
                    for (int i = 0; i < counts[len - 1]; ++i, ++k, ++code)
                        if (!h.len[sym[k]]) { h.code[sym[k]] = (uint16_t)code; h.len[sym[k]] = (uint8_t)len; }   // first code wins
                p += 2 + seglen;
            }
        }
    };
    inline const Tables& tables() { static const Tables t; return t; }

    // ---- Forward DCT: AAN float algorithm (as in IJG jfdctflt.c); outputs are scaled by kAan below.
    inline void FdctFloat(float* d)
    {
        for (int pass = 0; pass < 2; ++pass)
            for (int i = 0; i < 8; ++i)
            {
                const int step = pass ? 8 : 1;
                float* p = pass ? d + i : d + 8 * i;
                float t0 = p[0] + p[7 * step], t7 = p[0] - p[7 * step];
                float t1 = p[step] + p[6 * step], t6 = p[step] - p[6 * step];
                float t2 = p[2 * step] + p[5 * step], t5 = p[2 * step] - p[5 * step];
                float t3 = p[3 * step] + p[4 * step], t4 = p[3 * step] - p[4 * step];
                float t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
                p[0] = t10 + t11;
                p[4 * step] = t10 - t11;
                float z1 = (t12 + t13) * 0.707106781f;
                p[2 * step] = t13 + z1;
                p[6 * step] = t13 - z1;
                t10 = t4 + t5; t11 = t5 + t6; t12 = t6 + t7;
                float z5 = (t10 - t12) * 0.382683433f;
                float z2 = 0.541196100f * t10 + z5;
                float z4 = 1.306562965f * t12 + z5;
                float z3 = t11 * 0.707106781f;
                float z11 = t7 + z3, z13 = t7 - z3;
                p[5 * step] = z13 + z2;
                p[3 * step] = z13 - z2;
                p[step] = z11 + z4;
                p[7 * step] = z11 - z4;
            }
    }

    // 1 / (aanscale[u] * aanscale[v] * 8): turns AAN outputs into true JPEG DCT coefficients
    struct AanDescale
    {
        float f[64];
        AanDescale()
        {
            static const double s[8] = {1.0, 1.387039845, 1.306562965, 1.175875602, 1.0, 0.785694958, 0.541196100, 0.275899379};
            for (int v = 0; v < 8; ++v)
                for (int u = 0; u < 8; ++u) f[v * 8 + u] = (float)(1.0 / (s[v] * s[u] * 8.0));
        }
    };
    inline const AanDescale& aan() { static const AanDescale a; return a; }

    inline int bit_size(int v) { return v ? 32 - (int)__lzcnt((unsigned)(v < 0 ? -v : v)) : 0; }

    // Quantization table (zigzag order) for scale s: Blackmagic's shape, divided by 12 so s = 1 is
    // about a flat step of 1, clamped to what 16-bit DQT entries and 12-bit coefficients allow.
    // Each entry rounds at its own fixed offset, so as s grows the entries step up one at a time and
    // the frame size changes smoothly instead of jumping when all 64 cross a whole number together.
    inline void QuantTable(double s, uint16_t* q)
    {
        for (int k = 0; k < 64; ++k)
        {
            double offset = ((k * 29) % 64 + 0.5) / 64.0;   // a fixed permutation of 0..1
            q[k] = (uint16_t)std::clamp((int)std::floor(kQuantShape[k] * s / 12.0 + offset), 2, 65535);
        }
    }

    // One frame's DCT coefficients, per tile, zigzag order, 64 per block, blocks row by row.
    struct Coefficients
    {
        int tiles = 4, tile_w = 0, jw = 0, jh = 0, bw = 0, bh = 0;   // JPEG geometry per tile
        std::vector<int16_t> c;                                      // tiles * bw * bh * 64
        int16_t* block(int t, int b) { return c.data() + ((size_t)t * bw * bh + b) * 64; }
    };

    // ---- AVX2 version of FdctFloat: v[k] holds sample k of eight independent 1-D DCTs (one per lane).
    // Same operations in the same order as the scalar code, so the results are identical.
    inline void Dct1dVec(__m256* v)
    {
        const __m256 c707 = _mm256_set1_ps(0.707106781f), c382 = _mm256_set1_ps(0.382683433f), c541 = _mm256_set1_ps(0.541196100f), c1306 = _mm256_set1_ps(1.306562965f);
        __m256 t0 = _mm256_add_ps(v[0], v[7]), t7 = _mm256_sub_ps(v[0], v[7]);
        __m256 t1 = _mm256_add_ps(v[1], v[6]), t6 = _mm256_sub_ps(v[1], v[6]);
        __m256 t2 = _mm256_add_ps(v[2], v[5]), t5 = _mm256_sub_ps(v[2], v[5]);
        __m256 t3 = _mm256_add_ps(v[3], v[4]), t4 = _mm256_sub_ps(v[3], v[4]);
        __m256 t10 = _mm256_add_ps(t0, t3), t13 = _mm256_sub_ps(t0, t3), t11 = _mm256_add_ps(t1, t2), t12 = _mm256_sub_ps(t1, t2);
        v[0] = _mm256_add_ps(t10, t11);
        v[4] = _mm256_sub_ps(t10, t11);
        __m256 z1 = _mm256_mul_ps(_mm256_add_ps(t12, t13), c707);
        v[2] = _mm256_add_ps(t13, z1);
        v[6] = _mm256_sub_ps(t13, z1);
        t10 = _mm256_add_ps(t4, t5); t11 = _mm256_add_ps(t5, t6); t12 = _mm256_add_ps(t6, t7);
        __m256 z5 = _mm256_mul_ps(_mm256_sub_ps(t10, t12), c382);
        __m256 z2 = _mm256_add_ps(_mm256_mul_ps(c541, t10), z5);
        __m256 z4 = _mm256_add_ps(_mm256_mul_ps(c1306, t12), z5);
        __m256 z3 = _mm256_mul_ps(t11, c707);
        __m256 z11 = _mm256_add_ps(t7, z3), z13 = _mm256_sub_ps(t7, z3);
        v[5] = _mm256_add_ps(z13, z2);
        v[3] = _mm256_sub_ps(z13, z2);
        v[1] = _mm256_add_ps(z11, z4);
        v[7] = _mm256_sub_ps(z11, z4);
    }

    // 8x8 transpose of eight row vectors
    inline void Transpose8(__m256* r)
    {
        __m256 t[8], u[8];
        for (int i = 0; i < 8; i += 2) { t[i] = _mm256_unpacklo_ps(r[i], r[i + 1]); t[i + 1] = _mm256_unpackhi_ps(r[i], r[i + 1]); }
        u[0] = _mm256_shuffle_ps(t[0], t[2], 0x44); u[1] = _mm256_shuffle_ps(t[0], t[2], 0xEE);
        u[2] = _mm256_shuffle_ps(t[1], t[3], 0x44); u[3] = _mm256_shuffle_ps(t[1], t[3], 0xEE);
        u[4] = _mm256_shuffle_ps(t[4], t[6], 0x44); u[5] = _mm256_shuffle_ps(t[4], t[6], 0xEE);
        u[6] = _mm256_shuffle_ps(t[5], t[7], 0x44); u[7] = _mm256_shuffle_ps(t[5], t[7], 0xEE);
        for (int i = 0; i < 4; ++i) { r[i] = _mm256_permute2f128_ps(u[i], u[i + 4], 0x20); r[i + 4] = _mm256_permute2f128_ps(u[i], u[i + 4], 0x31); }
    }

    // std::lround for eight floats: nearest, ties away from zero (the SSE rounding is ties to even).
    inline __m256i LroundVec(__m256 x)
    {
        const __m256 sign = _mm256_set1_ps(-0.0f);
        __m256 r = _mm256_round_ps(x, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
        __m256 t = _mm256_round_ps(x, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC);
        __m256 d = _mm256_andnot_ps(sign, _mm256_sub_ps(x, t));   // |x - trunc(x)|, exact
        __m256 half = _mm256_cmp_ps(d, _mm256_set1_ps(0.5f), _CMP_EQ_OQ);
        __m256 away = _mm256_add_ps(t, _mm256_or_ps(_mm256_set1_ps(1.0f), _mm256_and_ps(x, sign)));
        return _mm256_cvtps_epi32(_mm256_blendv_ps(r, away, half));
    }

    // Tile width: a quarter of the image, rounded up to the multiple of 16 TIFF requires of tiles. The
    // last tile then runs past the right edge (3696-wide footage: 4 x 928 = 3712) and readers crop it.
    inline int TileWidth(int W) { return (W / 4 + 15) & ~15; }

    // Curve encoder index for a sample of `bits` bits: the 14-bit scale the encode table uses
    inline int CurveShift(int bits) { return 14 - bits; }   // ponytail: E2 footage is 12 or 14 bits; deeper data would need a right shift

    // Curve-encodes the CFA and runs the DCT for every block of every tile (AVX2).
    inline void Transform(const uint16_t* cfa, int W, int H, int bits, Coefficients& out)
    {
        const Curve& cv = curve();
        const AanDescale& ds = aan();
        out.tile_w = TileWidth(W);
        out.jw = out.tile_w * 2;
        out.jh = H / 2;
        out.bw = (out.jw + 7) / 8;
        out.bh = (out.jh + 7) / 8;
        out.c.resize((size_t)out.tiles * out.bw * out.bh * 64);
        const int shift = CurveShift(bits);
        alignas(32) float blk[64];
        alignas(32) int32_t q[64];
        for (int t = 0; t < out.tiles; ++t)
            for (int by = 0; by < out.bh; ++by)
                for (int bx = 0; bx < out.bw; ++bx)
                {
                    for (int y = 0; y < 8; ++y)
                    {
                        int jy = std::min(by * 8 + y, out.jh - 1);
                        for (int x = 0; x < 8; ++x)
                        {
                            int jx = std::min(bx * 8 + x, out.jw - 1);
                            int right = jx >= out.tile_w;
                            int sx = t * out.tile_w + jx - right * out.tile_w, sy = 2 * jy + right;
                            if (sx >= W) sx = W - 2 + (sx & 1);   // padding past the right edge: the last column of the same color
                            int v = (int)cfa[(size_t)sy * W + sx] << shift;
                            blk[y * 8 + x] = (float)((int)cv.encode[std::min(v, 16383)] - 2048);
                        }
                    }
                    __m256 v[8];
                    for (int i = 0; i < 8; ++i) v[i] = _mm256_load_ps(blk + 8 * i);   // v[y], lanes = x
                    Transpose8(v);      // v[x], lanes = y: sample x of each row's DCT
                    Dct1dVec(v);        // v[k], lanes = y: row outputs
                    Transpose8(v);      // v[y], lanes = k: sample y of each column's DCT
                    Dct1dVec(v);        // v[k], lanes = u: final row k
                    for (int k = 0; k < 8; ++k) _mm256_store_si256((__m256i*)(q + 8 * k), LroundVec(_mm256_mul_ps(v[k], _mm256_load_ps(ds.f + 8 * k))));
                    int16_t* o = out.block(t, by * out.bw + bx);
                    for (int k = 0; k < 64; ++k) o[k] = (int16_t)q[kZigzag[k]];
                }
    }

    // Quantizes one block (zigzag order), 16 coefficients per AVX2 step, clamped to what the tables can
    // code. Returns a bitmask of the nonzero coefficients so the entropy pass can skip the zeros.
    inline uint64_t QuantizeBlock(const int16_t* c, const float* inv_q, int16_t* zz)
    {
        const __m256i lim = _mm256_set1_epi16(16383), nlim = _mm256_set1_epi16(-16383), zero = _mm256_setzero_si256();
        __m256i q[4];
        for (int k = 0; k < 64; k += 16)
        {
            __m256i v = _mm256_loadu_si256((const __m256i*)(c + k));
            __m256i lo = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(_mm256_castsi256_si128(v))), _mm256_loadu_ps(inv_q + k)));
            __m256i hi = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(_mm256_extracti128_si256(v, 1))), _mm256_loadu_ps(inv_q + k + 8)));
            __m256i r = _mm256_permute4x64_epi64(_mm256_packs_epi32(lo, hi), 0xD8);   // packs works per 128-bit lane
            r = _mm256_min_epi16(_mm256_max_epi16(r, nlim), lim);
            _mm256_storeu_si256((__m256i*)(zz + k), r);
            q[k / 16] = r;
        }
        // Saturating pack to bytes keeps nonzero values nonzero; one mask bit per coefficient
        uint64_t mask = 0;
        for (int h = 0; h < 2; ++h)
        {
            __m256i b = _mm256_permute4x64_epi64(_mm256_packs_epi16(q[2 * h], q[2 * h + 1]), 0xD8);
            mask |= (uint64_t)(uint32_t)~_mm256_movemask_epi8(_mm256_cmpeq_epi8(b, zero)) << (32 * h);
        }
        return mask;
    }

    inline void InverseTable(const uint16_t* q_zigzag, float* inv)
    {
        for (int k = 0; k < 64; ++k) inv[k] = 1.0f / q_zigzag[k];
    }

    // Exact entropy-coded size in bits of all tiles at scale s (byte stuffing not included).
    inline uint64_t CountBits(Coefficients& co, double s)
    {
        const Tables& tb = tables();
        uint16_t q[64];
        alignas(32) float inv[64];
        QuantTable(s, q);
        InverseTable(q, inv);
        uint64_t bits = 0;
        alignas(32) int16_t zz[64];
        for (int t = 0; t < co.tiles; ++t)
        {
            int prev_dc = 0;
            for (int b = 0; b < co.bw * co.bh; ++b)
            {
                uint64_t m = QuantizeBlock(co.block(t, b), inv, zz) & ~1ull;
                int diff = std::clamp(zz[0] - prev_dc, -32767, 32767);
                prev_dc = zz[0];
                int cat = std::min(bit_size(diff), 15);
                bits += tb.dc.len[cat] + cat;
                int last = 0;
                while (m)
                {
                    int k = (int)_tzcnt_u64(m);
                    m &= m - 1;
                    int run = k - last - 1;
                    for (; run > 15; run -= 16) bits += tb.ac.len[0xF0];
                    int sz = bit_size(zz[k]);
                    bits += tb.ac.len[(run << 4) | sz] + sz;
                    last = k;
                }
                if (last != 63) bits += tb.ac.len[0x00];
            }
        }
        return bits;
    }

    // A tile's JPEG header up to and including SOS, laid out exactly like Blackmagic's.
    inline void TileHeader(const Coefficients& co, const uint16_t* q, std::vector<uint8_t>& out)
    {
        auto put16 = [&](int v) { out.push_back((uint8_t)(v >> 8)); out.push_back((uint8_t)v); };
        out.insert(out.end(), {0xFF, 0xD8, 0xFF, 0xFE, 0x00, 0x06, 0x99, 0x99, 0x99, 0x99});   // SOI, COM
        out.insert(out.end(), {0xFF, 0xDB, 0x00, 0x83, 0x10});                                 // DQT, 16-bit, table 0
        for (int k = 0; k < 64; ++k) put16(q[k]);
        out.insert(out.end(), kDht, kDht + sizeof kDht);
        out.insert(out.end(), {0xFF, 0xC1, 0x00, 0x0B, 0x0C});                                 // SOF1, 12-bit
        put16(co.jh); put16(co.jw);
        out.insert(out.end(), {0x01, 0x01, 0x11, 0x00});                                       // 1 component, 1x1, table 0
        out.insert(out.end(), {0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00});   // SOS
    }

    // Appends entropy-coded bytes with JPEG's 0x00 after every 0xFF.
    inline void AppendStuffed(std::vector<uint8_t>& out, const uint8_t* p, size_t n)
    {
        const uint8_t* end = p + n;
        while (p < end)
        {
            const uint8_t* ff = (const uint8_t*)memchr(p, 0xFF, end - p);
            if (!ff) { out.insert(out.end(), p, end); break; }
            out.insert(out.end(), p, ff + 1);
            out.push_back(0);
            p = ff + 1;
        }
    }

    // Writes one tile as a complete JPEG. Returns the entropy-coded payload bits (before byte stuffing).
    inline uint64_t EncodeTile(Coefficients& co, int t, const uint16_t* q, std::vector<uint8_t>& out)
    {
        const Tables& tb = tables();
        TileHeader(co, q, out);

        // Worst case per block: 64 codes of at most 30 bits, every byte stuffed
        thread_local std::vector<uint8_t> scan;
        const size_t blocks = (size_t)co.bw * co.bh;
        if (scan.size() < blocks * 512 + 64) scan.resize(blocks * 512 + 64);

        alignas(32) float inv[64];
        InverseTable(q, inv);
        JpegBitWriter bw{scan.data()};
        alignas(32) int16_t zz[64];
        int prev_dc = 0;
        for (size_t b = 0; b < blocks; ++b)
        {
            uint64_t m = QuantizeBlock(co.block(t, (int)b), inv, zz) & ~1ull;
            int diff = std::clamp(zz[0] - prev_dc, -32767, 32767);
            prev_dc = zz[0];
            int cat = std::min(bit_size(diff), 15);
            bw.put(((uint32_t)tb.dc.code[cat] << cat) | ((uint32_t)(diff < 0 ? diff - 1 : diff) & ((1u << cat) - 1)), tb.dc.len[cat] + cat);
            int last = 0;
            while (m)
            {
                int k = (int)_tzcnt_u64(m);
                m &= m - 1;
                int run = k - last - 1;
                for (; run > 15; run -= 16) bw.put(tb.ac.code[0xF0], tb.ac.len[0xF0]);
                int v = zz[k], sz = bit_size(v), sym = (run << 4) | sz;
                bw.put(((uint32_t)tb.ac.code[sym] << sz) | ((uint32_t)(v < 0 ? v - 1 : v) & ((1u << sz) - 1)), tb.ac.len[sym] + sz);
                last = k;
            }
            if (last != 63) bw.put(tb.ac.code[0x00], tb.ac.len[0x00]);
        }
        bw.flush(true);   // pad with 1 bits
        out.insert(out.end(), scan.data(), bw.w);
        out.insert(out.end(), {0xFF, 0xD9});
        return bw.bits;
    }

    struct Result { std::vector<uint8_t> tile[4]; double scale = 0; int passes = 0; };

    // Carried from frame to frame of a clip: the last quantization scale, and how much JPEG's 0xFF
    // byte stuffing adds on top of the counted bits (0.2% at 3:1, about 5% at 7:1 on Z CAM footage).
    struct RateState { double scale = 0; double stuffing = 1.004; };

    // Searches the scale whose estimated size is within 1% of target_bytes (sizes come in integer
    // quantization steps, so 1% is about as close as it gets). Returns the scale and pass count.
    inline double SearchScale(Coefficients& co, double target_bytes, double start, double stuffing, int& passes)
    {
        auto bytes_at = [&](double s) { return (double)CountBits(co, s) / 8.0 * stuffing + 4 * 660; };   // + headers
        double s = start;
        double lo = 0, hi = 0, f_lo = 0, f_hi = 0;   // bracket: size(lo) > target >= size(hi)
        for (int i = 0; i < 10; ++i, ++passes)
        {
            double b = bytes_at(s);
            if (std::fabs(b / target_bytes - 1) < 0.01) return s;
            if (b > target_bytes) { lo = s; f_lo = b; } else { hi = s; f_hi = b; }
            if (s <= 0.5 && b < target_bytes) return s;   // frame is small even at the finest step
            if (lo > 0 && hi > 0)
            {
                if (hi / lo < 1.002) break;   // no scale between the two gives a different table
                double k = std::log(f_lo / f_hi) / std::log(hi / lo);   // size ~ a * s^-k
                s = lo * std::pow(f_lo / target_bytes, 1.0 / std::max(k, 0.1));
                if (s <= lo || s >= hi) s = std::sqrt(lo * hi);
            }
            else s = std::max(0.5, s * std::pow(b / target_bytes, 1.0 / 0.9));   // size falls roughly as s^-0.9
        }
        return hi > 0 ? hi : s;   // the smaller of the two bracketing sizes
    }

    // ---- Fixed quality: coding error measured against the sensor's own noise.
    // Noise in 12-bit DN above black, measured on the E2:
    // sigma^2 = 1.24e-4 x ISO x signal + floor^2. The floor depends on the gain mode (ISO 2500 and
    // up is the high-gain one) and is interpolated in log ISO within it. Unknown ISO (iso <= 0): ISO
    // 500's model, the lowest noise at every level, so the quality is never lower than asked.
    // 14-bit footage is recorded in LN (low noise) mode: same gain and shot noise, lower floor, 0.51 DN
    // against 0.76 at ISO 500 on lens-cap frames (2026-10-17, UHD and 3696 x 2772 alike), so its floors
    // are two thirds of these. LN mode is in neither the clip metadata nor the frame parameters, so
    // the bit depth stands in for it; 12-bit LN footage keeps the normal floor.
    // ponytail: measured at ISO 500 only; an LN ISO series would replace the factor with a table.
    struct Noise { double a, floor2; };
    inline Noise E2Noise(double iso, int bits = 12)
    {
        static const double low[][2] = {{500, 0.76}, {800, 1.00}, {1250, 1.36}, {2000, 1.92}};
        static const double high[][2] = {{2500, 0.82}, {3200, 0.95}, {5000, 1.28}, {6400, 1.53}, {32000, 5.57}};
        if (iso <= 0) iso = 500;
        const double (*t)[2] = iso < 2500 ? low : high;
        const int n = iso < 2500 ? 4 : 5;
        double f = iso <= t[0][0] ? t[0][1] : t[n - 1][1];
        for (int i = 0; i + 1 < n; ++i)
            if (iso > t[i][0] && iso <= t[i + 1][0])
                f = t[i][1] * std::pow(t[i + 1][1] / t[i][1], std::log(iso / t[i][0]) / std::log(t[i + 1][0] / t[i][0]));
        if (bits >= 14) f *= 0.67;
        return {1.24e-4 * iso, f * f};
    }

    // The "error x noise" quality measure: RMS coding error over RMS noise, both in DN. Frame-wide it
    // hides the shadows: with one quantization table per tile the error is about even in curve codes,
    // while the noise in codes is several times lower near black than in the midtones, so a clean
    // shot's shadows can break into flat blocks while the frame still scores well.
    // Hence the measure is also taken per brightness band, one stop of signal above black each (band 0
    // below 8 DN).
    // (A block-by-block measure fails the other way: near-black blocks, where neither shows even
    // pushed 2 stops, would set the scale for the whole frame.)
    // Per block: w = (DN per curve code)^2 / 64, so squared coefficient errors x w summed over a set
    // of blocks, over that set's summed noise variance, is the measure squared. The JPEG DCT is
    // orthonormal, so coefficient and pixel errors carry the same energy; the curve's slope at the
    // block's mean turns codes into DN. var_scale: 1/4 per downscale.
    constexpr int kBands = 10;
    struct NoiseMap
    {
        std::vector<float> w;
        std::vector<uint8_t> band;
        double var = 0, band_var[kBands] = {};
        int band_n[kBands] = {};
    };
    inline int BandOf(double signal) { return signal < 8 ? 0 : std::min(kBands - 1, (int)std::log2(signal) - 2); }

    // The frame's mean black level in 12-bit DN, the unit of the noise model and of the curve (to_linear / 16).
    // 14-bit footage is the same signal on a 4x finer scale (black 796 to 800 against 200).
    inline double Black12(const ZrawFrame& f) { return std::ldexp((f.black[0] + f.black[1] + f.black[2] + f.black[3]) / 4.0, 12 - f.bits()); }

    inline void NoiseWeights(const Coefficients& co, const Noise& nz, double black, double var_scale, NoiseMap& m)
    {
        const Curve& cv = curve();
        const size_t n = (size_t)co.tiles * co.bw * co.bh;
        m = NoiseMap();
        m.w.resize(n);
        m.band.resize(n);
        for (size_t b = 0; b < n; ++b)
        {
            const int c = std::clamp((int)std::lround(co.c[b * 64] / 8.0 + 2048), 1, 4094);   // DC = 8 x mean, level-shifted
            const double slope = (cv.to_linear[c + 1] - cv.to_linear[c - 1]) / 32.0;          // DN per code
            const double signal = cv.to_linear[c] / 16.0 - black;
            const double var = (nz.a * std::max(signal, 0.0) + nz.floor2) * var_scale;
            m.w[b] = (float)(slope * slope / 64.0);
            m.band[b] = (uint8_t)BandOf(signal);
            m.var += var;
            m.band_var[m.band[b]] += var;
            m.band_n[m.band[b]]++;
        }
    }

    // Frame-wide measure at scale s; per band too when `bands` is given (0 for an empty band).
    inline double NoiseRatio(const Coefficients& co, const NoiseMap& m, double s, double* bands = nullptr)
    {
        uint16_t q[64];
        alignas(32) float inv[64];
        alignas(32) int16_t zz[64];
        QuantTable(s, q);
        InverseTable(q, inv);
        double sum = 0, band_sum[kBands] = {};
        for (size_t b = 0; b < m.w.size(); ++b)
        {
            const int16_t* c = co.c.data() + b * 64;
            QuantizeBlock(c, inv, zz);
            double e = 0;
            for (int k = 0; k < 64; ++k) { double d = c[k] - (double)zz[k] * q[k]; e += d * d; }
            sum += e * m.w[b];
            band_sum[m.band[b]] += e * m.w[b];
        }
        if (bands)
            for (int i = 0; i < kBands; ++i) bands[i] = m.band_var[i] > 0 ? std::sqrt(band_sum[i] / m.band_var[i]) : 0;
        return std::sqrt(sum / m.var);
    }

    // The fixed-quality measure: the worst band that covers at least 1% of the frame, so no level of
    // brightness that shows gets more than the target, shadows included.
    inline double WorstBand(const Coefficients& co, const NoiseMap& m, double s)
    {
        double bands[kBands], worst = 0;
        NoiseRatio(co, m, s, bands);
        for (int i = 0; i < kBands; ++i)
            if (m.band_n[i] >= 0.01 * m.w.size()) worst = std::max(worst, bands[i]);
        return worst;
    }

    // The coarsest scale whose worst band stays within `target` x noise (error grows with the scale).
    inline double ScaleForQuality(const Coefficients& co, const NoiseMap& m, double target)
    {
        double lo = 0.5, hi = 2000;   // the table entries saturate well before 2000
        if (WorstBand(co, m, lo) > target) return lo;
        if (WorstBand(co, m, hi) <= target) return hi;
        while (hi / lo > 1.002) { double x = std::sqrt(lo * hi); (WorstBand(co, m, x) <= target ? lo : hi) = x; }
        return lo;
    }

    // Where the DCT and the rate-control search run. The CPU backend is the code above; gpu_lossy.hpp
    // runs both on the GPU. The entropy coding and the file are the same either way.
    struct Backend
    {
        virtual ~Backend() = default;
        virtual bool Transform(const uint16_t* cfa, int W, int H, int bits, Coefficients& co) { lossy::Transform(cfa, W, H, bits, co); return true; }
        virtual double Search(Coefficients& co, double target_bytes, double start, double stuffing, int& passes)
        {
            return SearchScale(co, target_bytes, start, stuffing, passes);
        }
        // All tiles as complete JPEGs at quantization table q; adds their payload bits to counted_bits.
        virtual bool EncodeTiles(Coefficients& co, const uint16_t* q, std::vector<uint8_t>* tiles, uint64_t& counted_bits)
        {
            for (int t = 0; t < co.tiles; ++t) counted_bits += EncodeTile(co, t, q, tiles[t]);
            return true;
        }
    };

    // Encodes the frame at the scale that brings its tile data to target_bytes. If the real size
    // misses by more than 1% because stuffing differed from the estimate, corrects and encodes once more.
    inline bool Encode(Coefficients& co, double target_bytes, RateState& st, Result& r, Backend& be)
    {
        r.passes = 0;
        bool speculative = st.scale > 0;   // try the last frame's scale first; search only if it misses the 1% window
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            if (speculative) r.scale = st.scale;
            else r.scale = be.Search(co, target_bytes, st.scale > 0 ? st.scale : 9.7, st.stuffing, r.passes);
            uint16_t q[64];
            QuantTable(r.scale, q);
            size_t total = 0;
            uint64_t counted_bits = 0;
            for (int t = 0; t < co.tiles; ++t)
            {
                r.tile[t].clear();
                r.tile[t].reserve((size_t)(target_bytes / 4 * 1.1));
            }
            if (!be.EncodeTiles(co, q, r.tile, counted_bits)) return false;
            for (int t = 0; t < co.tiles; ++t) total += r.tile[t].size();
            double counted = counted_bits / 8.0;
            double predicted = counted * st.stuffing + 4 * 660;
            st.stuffing = std::clamp((total - 4 * 660.0) / counted, 1.0, 1.2);
            st.scale = r.scale;
            // Encode again only if the stuffing estimate was off, not when the step size is the limit
            if (std::fabs(total / target_bytes - 1) < 0.01 || (r.scale <= 0.5 && total < target_bytes)) break;
            if (speculative) { speculative = false; continue; }   // missed: search properly (stuffing now measured)
            if (std::fabs(total / predicted - 1) < 0.005 || r.scale <= 0.5) break;
        }
        return true;
    }

    // Encodes at a given scale, no rate control (fixed quality). Returns the tiles' total bytes, 0 on failure.
    inline size_t EncodeAt(Coefficients& co, double s, Result& r, Backend& be)
    {
        r.scale = s;
        r.passes = 0;
        uint16_t q[64];
        QuantTable(s, q);
        for (int t = 0; t < co.tiles; ++t) r.tile[t].clear();
        uint64_t bits = 0;
        if (!be.EncodeTiles(co, q, r.tile, bits)) return 0;
        size_t total = 0;
        for (int t = 0; t < co.tiles; ++t) total += r.tile[t].size();
        return total;
    }

    // ---- Minimal little-endian TIFF/DNG writer with tiles (tinydngwriter only does strips).
    class TiffWriter
    {
        struct Entry { uint16_t tag, type; uint32_t count; std::vector<uint8_t> data; };
        std::vector<Entry> entries;
        static size_t type_size(uint16_t t) { return t == 3 ? 2 : (t == 4 || t == 9) ? 4 : (t == 5 || t == 10) ? 8 : 1; }
    public:
        void add(uint16_t tag, uint16_t type, uint32_t count, const void* p)
        {
            Entry e{tag, type, count, std::vector<uint8_t>((const uint8_t*)p, (const uint8_t*)p + type_size(type) * count)};
            entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry& x) { return x.tag == tag; }), entries.end());
            entries.push_back(std::move(e));
        }
        void shorts(uint16_t tag, std::initializer_list<uint16_t> v) { std::vector<uint16_t> a(v); add(tag, 3, (uint32_t)a.size(), a.data()); }
        void longs(uint16_t tag, std::initializer_list<uint32_t> v) { std::vector<uint32_t> a(v); add(tag, 4, (uint32_t)a.size(), a.data()); }
        void bytes(uint16_t tag, std::initializer_list<uint8_t> v) { std::vector<uint8_t> a(v); add(tag, 1, (uint32_t)a.size(), a.data()); }
        void ascii(uint16_t tag, const char* s) { add(tag, 2, (uint32_t)strlen(s) + 1, s); }
        void rationals(uint16_t tag, bool is_signed, std::initializer_list<double> v) { rationals(tag, is_signed, std::vector<double>(v)); }
        void rationals(uint16_t tag, bool is_signed, const std::vector<double>& v)
        {
            std::vector<int32_t> a;
            for (double x : v) { a.push_back((int32_t)std::lround(x * 10000)); a.push_back(10000); }
            add(tag, is_signed ? 10 : 5, (uint32_t)v.size(), a.data());
        }

        // Header, IFD, tag data, then the tiles. Fills TileOffsets/TileByteCounts and returns the file bytes.
        bool write(const std::string& path, const std::vector<uint8_t>* tiles, int ntiles, uint64_t* written, std::string& err)
        {
            std::vector<uint32_t> offs(ntiles), counts(ntiles);
            for (int t = 0; t < ntiles; ++t) counts[t] = (uint32_t)tiles[t].size();
            add(324, 4, ntiles, offs.data());      // placeholder, same size as the final values
            add(325, 4, ntiles, counts.data());
            std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.tag < b.tag; });

            const size_t ifd_size = 2 + 12 * entries.size() + 4;
            size_t pos = 8 + ifd_size;
            std::vector<size_t> ext(entries.size(), 0);
            for (size_t i = 0; i < entries.size(); ++i)
                if (entries[i].data.size() > 4) { pos = (pos + 1) & ~(size_t)1; ext[i] = pos; pos += entries[i].data.size(); }
            size_t data_start = (pos + 15) & ~(size_t)15;
            for (int t = 0, p = (int)data_start; t < ntiles; p += counts[t], ++t) offs[t] = (uint32_t)p;
            for (auto& e : entries) if (e.tag == 324) memcpy(e.data.data(), offs.data(), 4 * ntiles);

            std::vector<uint8_t> head(data_start, 0);
            uint8_t* h = head.data();
            memcpy(h, "II\x2A\x00\x08\x00\x00\x00", 8);   // little endian, IFD at 8
            uint16_t n = (uint16_t)entries.size();
            memcpy(h + 8, &n, 2);
            for (size_t i = 0; i < entries.size(); ++i)
            {
                uint8_t* p = h + 10 + 12 * i;
                memcpy(p, &entries[i].tag, 2);
                memcpy(p + 2, &entries[i].type, 2);
                memcpy(p + 4, &entries[i].count, 4);
                if (ext[i]) { uint32_t o = (uint32_t)ext[i]; memcpy(p + 8, &o, 4); memcpy(h + ext[i], entries[i].data.data(), entries[i].data.size()); }
                else memcpy(p + 8, entries[i].data.data(), entries[i].data.size());
            }   // next-IFD offset stays 0

#ifdef _WIN32
            HANDLE f = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) { err = "cannot create " + path; return false; }
            auto put = [&](const void* p, size_t sz) { DWORD done = 0; return sz == 0 || (WriteFile(f, p, (DWORD)sz, &done, nullptr) && done == sz); };
#else
            FILE* f = fopen(path.c_str(), "wb");
            if (!f) { err = "cannot create " + path; return false; }
            auto put = [&](const void* p, size_t sz) { return sz == 0 || fwrite(p, 1, sz, f) == sz; };
#endif
            bool ok = put(head.data(), head.size());
            uint64_t total = head.size();
            for (int t = 0; ok && t < ntiles; ++t) { ok = put(tiles[t].data(), tiles[t].size()); total += tiles[t].size(); }
#ifdef _WIN32
            CloseHandle(f);
#else
            if (fclose(f)) ok = false;
#endif
            if (!ok) { err = "write failed: " + path; return false; }
            if (written) *written = total;
            return true;
        }
    };

    // WhiteLevel (16-bit linear). The DCT's ringing at the edges of clipped areas makes clipped
    // photosites decode under the top code, and Resolve's highlight recovery only rebuilds what reaches
    // WhiteLevel. The undershoot follows the frame's quantization scale: about 1.1 x the scale in curve
    // codes (0.9 to 1.4), measured on two daylight E2 clips (about 23,000 clipped photosites each) at
    // ratios from 1.5 to 20:1. The code uses 1.6 x for margin. Capped at 20% under the top: past that
    // (about 14:1 on detailed daylight) a lower white would cost more highlight range than the few
    // edge photosites it leaves unrecovered. The white level follows the actual scale, not the
    // nominal ratio, because fixed-quality frames can be coded far above the nominal ratio.
    inline uint32_t WhiteLevel(const Curve& cv, double scale)
    {
        const int c = std::max(0, 4095 - (int)std::ceil(1.6 * scale));
        return std::max<uint32_t>(cv.to_linear[c], (uint32_t)(cv.to_linear[4095] * 0.8));
    }

    struct Options { double ratio = 3.0; uint32_t fps_num = 0, fps_den = 0; const DngColor* color = nullptr;   // color: camera_color.hpp
                     const ClipTimecode* tc = nullptr; uint32_t frame = 0;                       // timecode of the clip's first frame, this frame's number
                     const ClipShot* shot = nullptr;                                             // lens, f-number, ISO, shutter (movparse.hpp)
                     double quality_scale = 0, min_ratio = 2, max_ratio = 8; };   // quality_scale > 0: fixed quality, within the ratio caps

    // Encodes one frame and writes it as a lossy CinemaDNG. `state` carries the quantization
    // scale between frames of a clip (read and updated), which usually saves rate-control passes.
    inline bool WriteLossyDng(const std::string& path, const ZrawFrame& f, const uint16_t* cfa, const Options& o,
                              RateState& state, std::string& err, uint64_t* written = nullptr, Result* result = nullptr,
                              Backend* backend = nullptr)
    {
        if (f.width % 4 || f.height % 2 || f.bits() > 14) { err = "lossy DNG needs a width divisible by 4, an even height and at most 14 bits"; return false; }
        thread_local Coefficients co;
        thread_local Result local;
        Result& r = result ? *result : local;
        static Backend cpu;
        Backend& be = backend ? *backend : cpu;
        if (!be.Transform(cfa, (int)f.width, (int)f.height, f.bits(), co)) { err = "GPU transform failed"; return false; }
        const double raw_bytes = (double)f.width * f.height * 12 / 8;   // Blackmagic counts against 12-bit packed raw
        if (o.quality_scale > 0)
        {
            // Fixed quality: the clip's scale, unless this frame would land outside the ratio caps
            const size_t total = EncodeAt(co, o.quality_scale, r, be);
            if (!total) { err = "GPU encode failed"; return false; }
            const double most = raw_bytes / o.min_ratio, least = raw_bytes / o.max_ratio;
            if (state.scale <= 0) state.scale = o.quality_scale;
            if ((total > most || total < least) && !Encode(co, total > most ? most : least, state, r, be)) { err = "GPU encode failed"; return false; }
        }
        else
        {
            // First frame of a clip: on Z CAM footage 3:1 lands near s = 9.7, 4:1 near 20, 5:1 near 34 (noise-matched curve; about 0.77x that on Blackmagic's)
            if (state.scale <= 0) state.scale = 9.7 * std::pow(o.ratio / 3.0, 2.6);
            if (!Encode(co, raw_bytes / o.ratio, state, r, be)) { err = "GPU encode failed"; return false; }
        }

        const Curve& cv = curve();
        const uint32_t tw = (uint32_t)TileWidth((int)f.width);
        TiffWriter w;
        w.longs(254, {0});                                   // NewSubfileType: main image
        w.longs(256, {f.width});
        w.longs(257, {f.height});
        w.shorts(258, {12});
        w.shorts(259, {7});                                  // JPEG
        w.shorts(262, {32803});                              // CFA
        w.ascii(270, "[ZRAW STUDIO] ZRAW -> DNG converter generated image");
        w.shorts(274, {1});
        w.shorts(277, {1});
        w.rationals(282, false, {300.0});
        w.rationals(283, false, {300.0});
        w.shorts(284, {1});
        w.shorts(296, {1});
        w.longs(322, {tw});
        w.longs(323, {f.height});
        w.shorts(33421, {2, 2});
        w.bytes(33422, {0, 1, 1, 2});
        w.bytes(50706, {1, 2, 0, 0});                        // DNGVersion, as Blackmagic writes it
        w.ascii(50708, "Z CAM E2");
        w.add(50712, 3, 4096, cv.to_linear);                 // LinearizationTable: the coding curve
        w.shorts(50713, {2, 2});
        // Black and white levels apply after linearization, in the curve's 16-bit domain (12-bit x 16, 14-bit x 4)
        const int up = 16 - f.bits();
        w.shorts(50714, {(uint16_t)(f.black[0] << up), (uint16_t)(f.black[1] << up), (uint16_t)(f.black[2] << up), (uint16_t)(f.black[3] << up)});
        w.longs(50717, {WhiteLevel(cv, r.scale)});
        const DngColor* c = o.color;   // the camera's own calibration, else zraw-parser's fixed matrices
        if (c) w.rationals(50721, true, std::vector<double>(c->color_matrix, c->color_matrix + 9));
        else
        {
            w.rationals(50721, true, {0.9784, -0.4995, 0.003, -0.3625, 1.1454, 0.2475, -0.0961, 0.2097, 0.6377});
            w.rationals(50722, true, {0.6770, -0.1895, -0.0744, -0.5232, 1.3145, 0.2303, -0.1664, 0.2691, 0.5703});
        }
        w.rationals(50727, false, {1.0, 1.0, 1.0});
        double rg = f.awb_r ? (double)f.awb_g / f.awb_r : 1.0, bg = f.awb_b ? (double)f.awb_g / f.awb_b : 1.0;
        w.rationals(50728, false, {rg, 1.0, bg});            // AsShotNeutral, from the frame's AWB gains
        w.shorts(50778, {(uint16_t)(c ? 0 : 17)});
        if (!c) w.shorts(50779, {21});
        if (c) w.rationals(50964, true, std::vector<double>(c->forward_matrix, c->forward_matrix + 9));   // ForwardMatrix1
        if (o.fps_num && o.fps_den) { int32_t fr[2] = {(int32_t)o.fps_num, (int32_t)o.fps_den}; w.add(51044, 10, 1, fr); }   // FrameRate
        if (o.shot) for (auto& t : ShotTags(*o.shot)) w.add(t.tag, t.type, t.count, t.data.data());
        if (o.tc && o.tc->ok)
        {
            uint8_t code[8];
            TimecodeBytes(*o.tc, o.frame, code);
            w.add(51043, 1, 8, code);                                              // TimeCodes
            if (!o.tc->reel.empty()) w.ascii(51081, o.tc->reel.c_str());           // ReelName
        }
        return w.write(path, r.tile, 4, written, err);
    }
}

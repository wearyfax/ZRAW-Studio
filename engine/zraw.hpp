// ZRAW (pre-0.94 firmware, "true RAW") frame parser and decoder.
// A port of storyboardcreativity/zraw-decoder-lib (GPL-2) that produces the same pixels bit for bit,
// rewritten around flat buffers and pointer bit reads. The codec is HiSilicon's line-predictive
// Bayer compression: two interleaved components per line, each predicted from its left neighbor
// and from the previous line after that line has been smoothed and truncated.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <intrin.h>
#include <immintrin.h>

struct ZrawFrame
{
    uint32_t version = 0;
    uint32_t bayer_mode = 0, cmp_mode = 0, is_lossless = 0, part_cmp_en = 0;
    uint32_t width = 0, height = 0, bitdepth_code = 0;  // real bit depth = 8 + 2 * bitdepth_code
    uint32_t stride_en = 0, stride = 0, align_mode = 0;
    uint32_t nl1_num_thr = 0, nl2_num_thr = 0, noise_grad_thr = 0;
    uint32_t awb_r = 0, awb_g = 0, awb_b = 0;
    uint16_t black[4] = {};
    std::vector<const uint8_t*> lines;  // one compressed line each, pointing into the caller's buffer
    const uint8_t* dpc = nullptr;       // defective-pixel tables, when the frame has them (frame 0 on the E2)
    size_t dpc_size = 0;
    const uint8_t* ccm = nullptr;       // the camera's color matrices, AES encrypted (camera_color.hpp reads them)
    size_t ccm_size = 0;

    int bits() const { return 8 + 2 * (int)bitdepth_code; }
};

namespace zraw_detail
{
    inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
    inline uint32_t le32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
    inline uint16_t le16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }

    // LSB-first reader, same bit order as the original BitReader. Reads 8 bytes at a time, so the
    // buffer needs 8 readable bytes past the last line (ParseZrawFrame's caller provides padding).
    struct Bits
    {
        const uint8_t* p;
        uint64_t pos;
        uint64_t peek() const { uint64_t v; memcpy(&v, p + (pos >> 3), 8); return v >> (pos & 7); }  // >= 57 valid bits
        uint32_t get(int n) { if (!n) return 0; uint32_t v = (uint32_t)(peek() & ((1ull << n) - 1)); pos += n; return v; }
    };

    inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
    inline int absi(int v) { return v < 0 ? -v : v; }

    // Number of bits of g minus one, capped at 6 (original: _getValueBitSizeMinus1_ButMax6).
    inline int lsb_size(int g)
    {
        unsigned long msb; _BitScanReverse(&msb, (unsigned long)(g | 1));   // g is never negative
        return msb > 6 ? 6 : (int)msb;
    }

    // Prefix code for the high part of a residual (original: _readHuffmanValue).
    inline uint32_t msb_code_slow(uint32_t d, uint32_t& len)
    {
        uint32_t tz = _tzcnt_u32(d | 0x200);   // trailing zeros, at most 9
        switch (tz)
        {
        case 5: len = 7; return ((d >> 5) & 3) == 1 ? 5 : 6;
        case 6: len = 8; return ((d >> 6) & 3) == 1 ? 7 : 8;
        case 7: len = 9; return ((d >> 7) & 3) == 1 ? 11 : 12;
        case 8: len = 9; return 10;
        case 9: len = 9; return 9;
        default: len = tz + 1; return tz;     // 0..4
        }
    }

    // The code is decided by the next 9 bits, so a 512-entry table replaces the branches.
    struct MsbTable
    {
        uint8_t value[512], len[512];
        MsbTable() { for (uint32_t i = 0; i < 512; ++i) { uint32_t l; value[i] = (uint8_t)msb_code_slow(i, l); len[i] = (uint8_t)l; } }
    };
    inline const MsbTable& msb_table() { static const MsbTable t; return t; }

    // Median-style predictor from left, up and up-left (original: _fixPrediction).
    // Written as selects rather than branches: the choice depends on pixel data and mispredicts often.
    inline int predict(int left, int up, int upleft)
    {
        int a = left <= up ? left : up, b = left <= up ? up : left;
        int r = upleft > a ? a + b - upleft : b;
        return upleft >= b ? a : r;
    }

    // Copies the smoothed line into the reference line with its low `shift` bits cleared
    // (original: __post_process_truncate, applied right after smoothing).
    inline void store_truncated(uint16_t* L, const uint16_t* tmp, int n, int shift)
    {
        const __m256i keep = _mm256_set1_epi16((short)(0xFFFF << shift));
        int i = 0;
        for (; i + 16 <= n; i += 16)
            _mm256_storeu_si256((__m256i*)(L + i), _mm256_and_si256(_mm256_loadu_si256((const __m256i*)(tmp + i)), keep));
        for (; i < n; ++i) L[i] = (uint16_t)(tmp[i] >> shift << shift);
    }

    inline __m256i ld(const uint16_t* p) { return _mm256_loadu_si256((const __m256i*)p); }

    // Smooths the previous component-A line before it is used for prediction (original: __post_process_a).
    // Indices are clamped to the line. The AVX2 path covers the interior when sums fit 16 bits (<= 12-bit data).
    inline void smooth_a(const uint16_t* L, uint16_t* dst, uint16_t* tmp, int n, int field, uint32_t noise, int shift, bool simd)
    {
        auto at = [&](int i) { return (int)L[clampi(i, 0, n - 1)]; };
        auto edge = [&](int i) {
            int f = i + field;
            return noise == 2 ? (3 * at(f + 1) + 5 * at(f) + 5 * at(f - 1) + 3 * at(f - 2) + 8) / 16 : (8 * at(f) + 8 * at(f - 1) + 8) / 16;
        };
        const int lo = 2, hi = n - 2;   // i + field - 2 >= 0 and i + field + 1 <= n - 1
        for (int i = 0; i < lo; ++i) tmp[i] = (uint16_t)edge(i);
        const uint16_t* p = L + field;
        int i = lo;
        if (simd && noise == 2)
        {
            const __m256i k3 = _mm256_set1_epi16(3), k5 = _mm256_set1_epi16(5), k8 = _mm256_set1_epi16(8);
            for (; i + 16 <= hi; i += 16)
            {
                __m256i outer = _mm256_mullo_epi16(_mm256_add_epi16(ld(p + i + 1), ld(p + i - 2)), k3);
                __m256i inner = _mm256_mullo_epi16(_mm256_add_epi16(ld(p + i), ld(p + i - 1)), k5);
                _mm256_storeu_si256((__m256i*)(tmp + i), _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(outer, inner), k8), 4));
            }
        }
        else if (simd)
            for (; i + 16 <= hi; i += 16)   // (8a + 8b + 8) / 16 == (a + b + 1) / 2, the rounded average
                _mm256_storeu_si256((__m256i*)(tmp + i), _mm256_avg_epu16(ld(p + i), ld(p + i - 1)));
        if (noise == 2) for (; i < hi; ++i) tmp[i] = (uint16_t)((3 * p[i + 1] + 5 * p[i] + 5 * p[i - 1] + 3 * p[i - 2] + 8) >> 4);
        else            for (; i < hi; ++i) tmp[i] = (uint16_t)((8 * p[i] + 8 * p[i - 1] + 8) >> 4);
        for (i = hi; i < n; ++i) tmp[i] = (uint16_t)edge(i);
        store_truncated(dst, tmp, n, shift);
    }

    // Same for the component-B/C line (original: __post_process_b). Noise level 0 moves each value toward
    // the local average by at most half the block step, which is a clamp.
    inline void smooth_b(const uint16_t* L, uint16_t* dst, uint16_t* tmp, int n, const uint8_t* hdr, uint32_t noise, int shift, bool simd)
    {
        auto at = [&](int i) { return (int)L[clampi(i, 0, n - 1)]; };
        auto one = [&](int i) {
            int m2 = at(i - 2), m1 = at(i - 1), c = at(i), p1 = at(i + 1), p2 = at(i + 2);
            if (noise == 1) return (m1 * 4 + c * 8 + p1 * 4 + 8) >> 4;
            if (noise == 2) return (p2 * 2 + p1 * 4 + c * 4 + m1 * 4 + m2 * 2 + 8) >> 4;
            int off = (1 << hdr[i / 32]) >> 1;
            return c + clampi(((p1 + 2 * c + m1 + 2) >> 2) - c, -off, off);
        };
        // Vector chunks start 16-aligned so each sits inside one 32-value block (one header value)
        int i = 0, vend = 0;
        if (simd && n >= 36)
        {
            for (; i < 16; ++i) tmp[i] = (uint16_t)one(i);
            vend = 16 + (n - 2 - 16) / 16 * 16;
            const __m256i k2 = _mm256_set1_epi16(2), k8 = _mm256_set1_epi16(8);
            for (; i < vend; i += 16)
            {
                __m256i m1 = ld(L + i - 1), c = ld(L + i), p1 = ld(L + i + 1);
                __m256i r;
                if (noise == 1)
                    r = _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(m1, p1), _mm256_add_epi16(_mm256_add_epi16(c, c), k2)), 2);
                else if (noise == 2)
                {
                    __m256i near3 = _mm256_slli_epi16(_mm256_add_epi16(_mm256_add_epi16(m1, p1), c), 2);
                    __m256i far2 = _mm256_slli_epi16(_mm256_add_epi16(ld(L + i - 2), ld(L + i + 2)), 1);
                    r = _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(near3, far2), k8), 4);
                }
                else
                {
                    __m256i avg = _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(m1, p1), _mm256_add_epi16(_mm256_add_epi16(c, c), k2)), 2);
                    __m256i off = _mm256_set1_epi16((short)((1 << hdr[i / 32]) >> 1));
                    __m256i delta = _mm256_sub_epi16(avg, c);
                    delta = _mm256_min_epi16(_mm256_max_epi16(delta, _mm256_sub_epi16(_mm256_setzero_si256(), off)), off);
                    r = _mm256_add_epi16(c, delta);
                }
                _mm256_storeu_si256((__m256i*)(tmp + i), r);
            }
        }
        for (; i < n; ++i) tmp[i] = (uint16_t)one(i);
        store_truncated(dst, tmp, n, shift);
    }
}

// Parses one frame's container. `data` must stay alive while the frame is decoded and must have at
// least 8 readable bytes after `size`.
inline bool ParseZrawFrame(const uint8_t* data, size_t size, ZrawFrame& f, std::string& err)
{
    using namespace zraw_detail;
    if (size < 8 || memcmp(data + 4, "zraw", 4) != 0) { err = "not a ZRAW frame"; return false; }
    size_t end = be32(data);
    if (end > size) { err = "truncated frame"; return false; }
    bool have_params = false;
    for (size_t pos = 8; pos + 8 <= end;)
    {
        uint32_t bsize = be32(data + pos);
        if (bsize < 8 || pos + bsize > end) { err = "bad block size"; return false; }
        const uint8_t* b = data + pos + 8;
        size_t blen = bsize - 8;
        if (!memcmp(data + pos + 4, "ver ", 4) && blen >= 4) f.version = be32(b);
        else if (!memcmp(data + pos + 4, "pram", 4) && blen >= 176)
        {
            f.bayer_mode = le32(b + 0);  f.cmp_mode = le32(b + 4);    f.is_lossless = le32(b + 8);
            f.width = le32(b + 12);      f.height = le32(b + 16);     f.bitdepth_code = le32(b + 20);
            f.part_cmp_en = le32(b + 24); f.stride_en = le32(b + 36); f.stride = le32(b + 40);
            f.align_mode = le32(b + 44);
            f.nl2_num_thr = le32(b + 164); f.nl1_num_thr = le32(b + 168); f.noise_grad_thr = le32(b + 172);
            have_params = true;
        }
        else if (!memcmp(data + pos + 4, "awb ", 4) && blen >= 12) { f.awb_r = le32(b); f.awb_g = le32(b + 4); f.awb_b = le32(b + 8); }
        else if (!memcmp(data + pos + 4, "dpc ", 4)) { f.dpc = b; f.dpc_size = blen; }
        else if (!memcmp(data + pos + 4, "ccm ", 4)) { f.ccm = b; f.ccm_size = blen; }
        else if (!memcmp(data + pos + 4, "blc ", 4) && blen >= 8) for (int i = 0; i < 4; ++i) f.black[i] = le16(b + 2 * i);
        else if (!memcmp(data + pos + 4, "data", 4))
        {
            if (!have_params) { err = "data block before parameters"; return false; }
            f.lines.clear();
            size_t q = 0;
            for (uint32_t y = 0; y < f.height; ++y)
            {
                if (q + 2 > blen) { err = "truncated line data"; return false; }
                uint32_t count = le16(b + q);
                if (f.stride < count + 1) { err = "line longer than stride"; return false; }
                f.lines.push_back(b + q + 2);  // the 16-bit length is not part of the bitstream
                q += 16 * (size_t)(count + 1);
            }
            if (q > blen) { err = "truncated line data"; return false; }
        }
        pos += bsize;
    }
    if (!have_params || f.lines.size() != f.height) { err = "missing parameters or image data"; return false; }
    if (f.cmp_mode != 0) { err = "uncompressed ZRAW frames are not supported"; return false; }
    if (f.width % 4 || f.height % 2 || f.bits() > 16) { err = "unexpected frame geometry"; return false; }
    return true;
}

#ifdef ZRAW_PROFILE
inline uint64_t g_prof[4];   // main loop, output copy, smoothing, bayer reorder (cycles)
inline uint64_t g_cov[8];    // lines at noise 0/1/2, raw blocks, VL blocks, blocks with diff > 0, lossless frames, frames
#define PROF_MARK(i) do { uint64_t _t = __rdtsc(); g_prof[i] += _t - _pt; _pt = _t; } while (0)
#else
#define PROF_MARK(i) do {} while (0)
#endif

// Decodes into `cfa`, width * height values, row-major, in the original decoder's Bayer layout.
inline void DecodeZrawFrame(const ZrawFrame& f, uint16_t* cfa)
{
    using namespace zraw_detail;
    const int W = (int)f.width, H = (int)f.height, n = W / 2;
    const int bd = f.bits(), maxpix = (1 << bd) - 1, def = 1 << (bd - 1);
    const bool lossless = !(f.is_lossless == 0 && f.part_cmp_en == 0);
    const int dist = (int)f.noise_grad_thr, trunc = bd > 10 ? bd - 10 : 0;
    const int blocks = (n + 31) / 32;

    std::vector<uint16_t> cur_a(n), cur_b(n), prev_a(n, (uint16_t)def), prev_b(n, (uint16_t)def), prev_c(n, (uint16_t)def), tmp(n);
    std::vector<uint8_t> hdr(blocks);
    uint32_t history[8] = {}, noise = 0;
    const MsbTable& msb = msb_table();
    const bool simd = bd <= 12;   // 16-bit lanes hold the smoothing sums only up to 12-bit data
#ifdef ZRAW_PROFILE
    ++g_cov[7]; if (lossless) ++g_cov[6];
#endif

#ifdef ZRAW_PROFILE
    uint64_t _pt = __rdtsc();
#endif
    for (int y = 0; y < H; ++y)
    {
        Bits br{f.lines[y], 0};
        const bool upper = (y & 1) == 0;
        const uint16_t* up_a = prev_a.data();
        const uint16_t* up_b = upper ? prev_b.data() : prev_c.data();

        int g_a = 4, g_b = 4;
        int left_a = def, left_b = def, ul_a = def, ul_b = def;  // contexts reset every line
        int diff = 0;
        uint32_t quiet = 0;   // pixels whose local gradient is under noise_grad_thr

        for (int blk = 0, j = 0; blk < blocks; ++blk)
        {
            // Block header: bit-depth reduction (relative after the first block) and raw/VL mode
            if (lossless) diff = 0;
            else if (j > 0) { if (br.get(1)) { static const int step[4] = {-2, -1, 1, 2}; diff += step[br.get(2)]; } }
            else diff = (int)br.get(4);
            const bool raw = br.get(1) != 0;
            hdr[blk] = (uint8_t)diff;
#ifdef ZRAW_PROFILE
            ++g_cov[raw ? 3 : 4]; if (diff > 0) ++g_cov[5];
#endif

            const int scale = 1 << diff;
            const int range = (scale >> 1) - 1 < 0 ? 0 : (scale >> 1) - 1;
            const int steps = ((2 * range + maxpix) >> diff) + 1;
            const int esc_bits = bd - diff;

            const int jend = j + 32 < n ? j + 32 : n;
            for (; j < jend; ++j)
            {
                const int u_a = up_a[j], u_b = up_b[j];   // reference lines start filled with `def`
                int v_a, v_b;
                if (raw)
                {
                    v_a = (int)br.get(esc_bits) << diff;
                    v_b = (int)br.get(esc_bits) << diff;
                }
                else
                {
                    // Two prefix codes, then two low-bit fields sized by each component's context
                    const int sz_a = lsb_size(g_a), sz_b = lsb_size(g_b);
                    uint64_t d = br.peek();
                    const uint32_t msb_a = msb.value[d & 511], len_a = msb.len[d & 511]; d >>= len_a;
                    const uint32_t msb_b = msb.value[d & 511], len_b = msb.len[d & 511]; d >>= len_b;
                    const int la = msb_a == 12 ? esc_bits : sz_a;
                    const uint32_t lsb_a = (uint32_t)(d & ((1ull << la) - 1)); d >>= la;
                    const int lb = msb_b == 12 ? esc_bits : sz_b;
                    const uint32_t lsb_b = (uint32_t)(d & ((1ull << lb) - 1));
                    br.pos += len_a + len_b + la + lb;

                    int code_a = msb_a == 12 ? (int)lsb_a + 1 : (int)(msb_a << la | lsb_a);
                    int code_b = msb_b == 12 ? (int)lsb_b + 1 : (int)(msb_b << lb | lsb_b);

                    // Zig-zag residual times the block step, plus prediction, wrapped back into range
                    auto reconstruct = [&](int code, int pred) {
                        int body = (code + 1) >> 1;
                        int v = scale * ((code & 1) ? -body : body) + pred;
                        if (v < -range) v += scale * steps;
                        else if (range + maxpix < v) v -= scale * steps;
                        return clampi(v, 0, maxpix);
                    };
                    v_a = reconstruct(code_a, predict(left_a, u_a, ul_a));
                    v_b = reconstruct(code_b, predict(left_b, u_b, ul_b));

                    code_a -= (code_a >> sz_a) > 11;
                    code_b -= (code_b >> sz_b) > 11;
                    g_a = (2 * clampi(code_a, 0, 255) + 2 * g_a + 2) >> 2;
                    g_b = (2 * clampi(code_b, 0, 255) + 2 * g_b + 2) >> 2;
                }

                // Noise statistics use component A only
                int m = absi(v_a - left_a), t = absi(ul_a - v_a), s = absi(u_a - v_a);
                if (t < m) m = t;
                if (s < m) m = s;
                quiet += m < dist;

                left_a = v_a; left_b = v_b; ul_a = u_a; ul_b = u_b;
                cur_a[j] = (uint16_t)v_a; cur_b[j] = (uint16_t)v_b;
            }
        }

        PROF_MARK(0);
        // Straight into the CFA layout: the original's per-2x2 reorder (_post_process_correct_bayer)
        // amounts to swapping the two components on even rows for bayer modes 0 and 3, on odd rows
        // for modes 1 and 2.
        {
            uint16_t* row = cfa + (size_t)y * W;
            const bool swap = upper ? (f.bayer_mode == 0 || f.bayer_mode == 3) : (f.bayer_mode == 1 || f.bayer_mode == 2);
            const uint16_t* first = swap ? cur_b.data() : cur_a.data();
            const uint16_t* second = swap ? cur_a.data() : cur_b.data();
            int i = 0;
            for (; i + 16 <= n; i += 16)
            {
                __m256i a = ld(first + i), b = ld(second + i);
                __m256i lo = _mm256_unpacklo_epi16(a, b), hi = _mm256_unpackhi_epi16(a, b);
                _mm256_storeu_si256((__m256i*)(row + 2 * i), _mm256_permute2x128_si256(lo, hi, 0x20));
                _mm256_storeu_si256((__m256i*)(row + 2 * i + 16), _mm256_permute2x128_si256(lo, hi, 0x31));
            }
            for (; i < n; ++i) { row[2 * i] = first[i]; row[2 * i + 1] = second[i]; }
        }
        PROF_MARK(1);

        // The decoded line becomes the (smoothed, truncated) reference for the next lines
        int field_flag = f.bayer_mode != 3 ? (f.bayer_mode == 0) : 1;
        int field = ((y & 1) != field_flag) ? 0 : 1;
        uint16_t* ref_b = upper ? prev_b.data() : prev_c.data();
#ifdef ZRAW_PROFILE
        ++g_cov[noise];
#endif
        smooth_a(cur_a.data(), prev_a.data(), tmp.data(), n, field, noise, trunc, simd);
        smooth_b(cur_b.data(), ref_b, tmp.data(), n, hdr.data(), noise, trunc, simd);

        memmove(history + 1, history, 7 * sizeof(uint32_t));
        history[0] = quiet >= f.nl2_num_thr ? (quiet < f.nl1_num_thr ? 1u : 0u) : 2u;
        int c0 = 0, c1 = 0, c2 = 0;
        for (int i = 0; i < 8; ++i) { c0 += history[i] == 0; c1 += history[i] == 1; c2 += history[i] == 2; }
        noise = (c0 >= c1 && c0 >= c2) ? 0 : (c1 >= c0 && c1 >= c2) ? 1 : 2;
        PROF_MARK(2);
    }

    PROF_MARK(3);
}

// Lossless JPEG (predictor 6) encoder producing the same bytes as tinydngwriter's lj92_encode,
// which zraw-parser uses. The Huffman table comes from the original createEncodeTable; residuals
// are computed once, and whole codes go through JpegBitWriter's 64-bit accumulator.
#pragma once
#include "bitwriter.hpp"
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include <intrin.h>
// Needs tiny_dng_writer.h included first (dng.hpp does it); its implementation section has no include guard.

// Encodes `image` (width x height, row-major). On success `out`/`out_len` point at a per-thread
// buffer that stays valid until this thread encodes again.
inline bool FastLj92Encode(const uint16_t* image, int width, int height, int bitdepth, const uint8_t*& out, size_t& out_len)
{
    thread_local std::vector<int16_t> res;   // residuals, reused across frames
    res.resize((size_t)width * height);
    tinydngwriter::lje self = {};
    self.width = width;
    self.height = height;
    self.bitdepth = bitdepth;

    // Predictor 6 residuals: row 0 predicts from the left, column 0 from above. AVX2 on 16-bit lanes:
    // every intermediate fits int16 for data up to 15 bits, and the arithmetic shift matches C's >> on int.
    const int maxval = 1 << bitdepth;
    __m256i top_v = _mm256_setzero_si256();
    for (int y = 0; y < height; ++y)
    {
        const uint16_t* row = image + (size_t)y * width;
        int16_t* r = res.data() + (size_t)y * width;
        int x = 1;
        uint16_t top_tail = row[0];
        if (y == 0)
        {
            r[0] = (int16_t)(row[0] - (1 << (bitdepth - 1)));
            for (; x + 16 <= width; x += 16)
            {
                __m256i c = _mm256_loadu_si256((const __m256i*)(row + x));
                top_v = _mm256_or_si256(top_v, c);
                _mm256_storeu_si256((__m256i*)(r + x), _mm256_sub_epi16(c, _mm256_loadu_si256((const __m256i*)(row + x - 1))));
            }
            for (; x < width; ++x) { top_tail |= row[x]; r[x] = (int16_t)(row[x] - row[x - 1]); }
        }
        else
        {
            const uint16_t* up = row - width;
            r[0] = (int16_t)(row[0] - up[0]);
            for (; x + 16 <= width; x += 16)
            {
                __m256i c = _mm256_loadu_si256((const __m256i*)(row + x)), u = _mm256_loadu_si256((const __m256i*)(up + x));
                __m256i cl = _mm256_loadu_si256((const __m256i*)(row + x - 1)), ul = _mm256_loadu_si256((const __m256i*)(up + x - 1));
                top_v = _mm256_or_si256(top_v, c);
                __m256i pred = _mm256_add_epi16(u, _mm256_srai_epi16(_mm256_sub_epi16(cl, ul), 1));
                _mm256_storeu_si256((__m256i*)(r + x), _mm256_sub_epi16(c, pred));
            }
            for (; x < width; ++x) { top_tail |= row[x]; r[x] = (int16_t)(row[x] - (up[x] + ((row[x - 1] - up[x - 1]) >> 1))); }
        }
        if (top_tail >= maxval) return false;
    }
    {
        alignas(32) uint16_t t[16];
        _mm256_store_si256((__m256i*)t, top_v);
        uint16_t top = 0;
        for (int k = 0; k < 16; ++k) top |= t[k];
        if (top >= maxval) return false;
    }
    auto ssss = [](int d) { return 32 - (int)__lzcnt((unsigned)(d < 0 ? -d : d)); };
    {
        int h[4][17] = {};
        const int16_t* q = res.data();
        size_t i = 0, nres = res.size();
        for (; i + 4 <= nres; i += 4) { ++h[0][ssss(q[i])]; ++h[1][ssss(q[i + 1])]; ++h[2][ssss(q[i + 2])]; ++h[3][ssss(q[i + 3])]; }
        for (; i < nres; ++i) ++h[0][ssss(q[i])];
        for (int s = 0; s < 17; ++s) self.hist[s] += h[0][s] + h[1][s] + h[2][s] + h[3][s];
    }
    tinydngwriter::createEncodeTable(&self);

    uint8_t header[128];
    self.encoded = header;
    self.encodedWritten = 0;
    tinydngwriter::writeHeader(&self);

    // Worst-case size, same bound as the original. Allocated once per thread, never zero-filled.
    thread_local std::unique_ptr<uint8_t[]> buf;
    thread_local size_t cap = 0;
    const size_t need = (size_t)width * height * 3 + 256;
    if (cap < need) { buf.reset(new uint8_t[need]); cap = need; }
    memcpy(buf.get(), header, self.encodedWritten);
    uint8_t* w = buf.get() + self.encodedWritten;

    // Per size class: Huffman code shifted above the magnitude bits, and total length
    uint32_t code[17], len[17];
    for (int s = 0; s < 17; ++s) { int h = self.huffsym[s]; code[s] = (uint32_t)self.huffenc[h] << s; len[s] = self.huffbits[h] + s; }

    JpegBitWriter bw{w};
    for (size_t i = 0; i < res.size(); ++i)
    {
        int d = res[i];
        int s = ssss(d);
        uint32_t mag = (uint32_t)(d - (d < 0)) & ((1u << s) - 1);   // negatives as d + 2^s - 1, JPEG style
        bw.put(code[s] | mag, len[s]);
    }
    bw.flush(false);   // pad with zero bits, as the original does
    w = bw.w;
    *w++ = 0xFF;
    *w++ = 0xD9;   // EOI
    out = buf.get();
    out_len = (size_t)(w - buf.get());
    return true;
}

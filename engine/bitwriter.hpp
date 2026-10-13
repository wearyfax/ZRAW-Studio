// JPEG entropy-coded bit writer: MSB first, a 0x00 stuffed after every 0xFF byte. Codes go into a
// 64-bit accumulator and leave 32 bits at a time; only words containing 0xFF take the byte path.
// (A variant that stored 8 bytes on every put measured slower: 34.5 vs 30.6 ms per UHD LJ92 frame.)
#pragma once
#include <cstdint>
#include <cstring>
#include <intrin.h>

struct JpegBitWriter
{
    uint8_t* w;
    uint64_t acc = 0;
    int n = 0;
    uint64_t bits = 0;   // payload bits written, before stuffing

    void byte(uint8_t b) { *w++ = b; if (b == 0xFF) *w++ = 0; }

    void put(uint32_t v, int len)   // v holds exactly len bits, len <= 32
    {
        acc = (acc << len) | v;
        n += len;
        bits += len;
        if (n >= 32)
        {
            n -= 32;
            uint32_t word = (uint32_t)(acc >> n), inv = ~word;   // a 0xFF byte in word is a zero byte in inv
            if (((inv - 0x01010101u) & ~inv & 0x80808080u) == 0) { word = _byteswap_ulong(word); memcpy(w, &word, 4); w += 4; }
            else for (int k = 24; k >= 0; k -= 8) byte((uint8_t)(word >> k));
        }
    }

    // Pads the last byte with 1 bits if `ones`, else with 0 bits.
    void flush(bool ones)
    {
        while (n >= 8) { n -= 8; byte((uint8_t)(acc >> n)); }
        if (n) byte((uint8_t)((acc << (8 - n)) | (ones ? 0xFF >> n : 0)));
        n = 0;
    }
};

// Hot and dead pixel removal for the previews. DNG conversion never uses this: its output must stay
// byte-identical to zraw-parser's.
//
// Two sources, merged:
//  - The camera's defect list: the 'dpc' block in frame 0. Entries are little-endian (y << 16 | x) in
//    final CFA coordinates. On the E2 it is a factory map (one table, tagged 100) whose pixels are only
//    slightly warm at high ISO, so on its own it misses the hot pixels that show.
//  - Stuck pixels found in the footage: frames spread through the clip are checked, and a photosite
//    that sits well above (hot) or below (dead) every same-color neighbor in most of them is stuck.
//    Noise and fine moving detail don't repeat at one spot, so they are left alone.
// Each defective photosite is replaced by the median of its four nearest same-color neighbors.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <thread>
#include <vector>
#include "clip.hpp"

// Camera list from a frame's 'dpc' block (all tables, bright and dark), as linear CFA indices
inline void AddCameraDefects(const ZrawFrame& f, int W, int H, std::vector<uint32_t>& out)
{
    const uint8_t* p = f.dpc;
    size_t n = f.dpc_size, q = 0;
    auto be32 = [&](uint32_t& v) { if (!p || q + 4 > n) return false; v = (uint32_t)p[q] << 24 | (uint32_t)p[q + 1] << 16 | (uint32_t)p[q + 2] << 8 | p[q + 3]; q += 4; return true; };
    auto entry = [&](void) {
        if (q + 4 > n) return;
        uint32_t le = (uint32_t)p[q] | (uint32_t)p[q + 1] << 8 | (uint32_t)p[q + 2] << 16 | (uint32_t)p[q + 3] << 24;
        q += 4;
        uint32_t x = le & 0xFFFF, y = le >> 16;
        if ((int)x < W && (int)y < H) out.push_back(y * (uint32_t)W + x);
    };
    uint32_t tables, iso, count;
    if (!be32(tables)) return;
    for (uint32_t t = 0; t < tables && t < 32; ++t)
    {
        if (!be32(iso)) return;
        for (int kind = 0; kind < 2; ++kind)   // bright, then dark
        {
            if (!be32(count)) return;
            for (uint32_t i = 0; i < count; ++i) entry();
        }
    }
}

// Photosites more than `threshold` above every same-color neighbor (or below every one) in at
// least three quarters of `samples` frames spread through the clip. Decodes the samples in parallel.
inline std::vector<uint32_t> FindStuckPixels(const ZrawClip& clip, int samples = 8, int threshold = 32)
{
    const int W = clip.width, H = clip.height;
    std::vector<int> picks;
    for (int i = 0; i < samples; ++i)
    {
        int f = samples > 1 ? (int)((int64_t)i * (clip.frames - 1) / (samples - 1)) : 0;
        if (picks.empty() || picks.back() != f) picks.push_back(f);
    }
    std::vector<std::vector<uint32_t>> found(picks.size());
    std::vector<std::thread> pool;
    for (size_t k = 0; k < picks.size(); ++k)
        pool.emplace_back([&, k] {
            std::vector<uint8_t> data;
            std::vector<uint16_t> cfa;
            ZrawFrame f;
            std::string err;
            if (!clip.Decode(picks[k], data, f, cfa, err)) return;
            for (int y = 2; y < H - 2; ++y)
            {
                const uint16_t* r = cfa.data() + (size_t)y * W;
                for (int x = 2; x < W - 2; ++x)
                {
                    const int v = r[x];
                    const uint16_t nb[8] = {r[x - 2], r[x + 2], r[x - 2 * W], r[x + 2 * W], r[x - 2 * W - 2], r[x - 2 * W + 2], r[x + 2 * W - 2], r[x + 2 * W + 2]};
                    int hi = nb[0], lo = nb[0];
                    for (int i = 1; i < 8; ++i) { hi = std::max(hi, (int)nb[i]); lo = std::min(lo, (int)nb[i]); }
                    if (v - hi > threshold || lo - v > threshold) found[k].push_back((uint32_t)y * W + x);
                }
            }
        });
    for (auto& t : pool) t.join();

    std::vector<uint32_t> all;
    for (auto& v : found) all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());
    const size_t need = (picks.size() * 3 + 3) / 4;
    std::vector<uint32_t> stuck;
    for (size_t i = 0; i < all.size();)
    {
        size_t j = i;
        while (j < all.size() && all[j] == all[i]) ++j;
        if (j - i >= need) stuck.push_back(all[i]);
        i = j;
    }
    return stuck;
}

// Camera list plus stuck pixels, sorted and without duplicates
// Setting the environment variable ZRAW_NO_DEFECTS turns repair off, for before/after comparisons.
inline std::vector<uint32_t> BuildDefectMap(const ZrawClip& clip)
{
    if (getenv("ZRAW_NO_DEFECTS")) return {};
    std::vector<uint32_t> map = FindStuckPixels(clip);
    std::vector<uint8_t> data;
    ZrawFrame f0;
    std::string err;
    if (clip.Read(0, data) && ParseZrawFrame(data.data(), data.size() - 16, f0, err)) AddCameraDefects(f0, clip.width, clip.height, map);
    std::sort(map.begin(), map.end());
    map.erase(std::unique(map.begin(), map.end()), map.end());
    return map;
}

// Replaces each listed photosite with the median of its four nearest same-color neighbors
inline void RepairDefects(uint16_t* cfa, int W, int H, const std::vector<uint32_t>& map)
{
    for (uint32_t i : map)
    {
        const int x = (int)(i % (uint32_t)W), y = (int)(i / (uint32_t)W);
        int v[4], n = 0;
        if (x >= 2) v[n++] = cfa[i - 2];
        if (x < W - 2) v[n++] = cfa[i + 2];
        if (y >= 2) v[n++] = cfa[i - 2 * (size_t)W];
        if (y < H - 2) v[n++] = cfa[i + 2 * (size_t)W];
        if (!n) continue;
        std::sort(v, v + n);
        cfa[i] = (uint16_t)(n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2] + 1) / 2);
    }
}

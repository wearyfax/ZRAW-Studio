// Decoded preview frames for mpv: FrameServer decodes ahead of the reader and caches frames, and
// FillPreviewStream exposes a clip to mpv as a custom stream of those frames laid end to end.
// Shared by the embedded player (player.hpp) and the mpv plugin (mpv_plugin.cpp).
#pragma once
#include <algorithm>
#include <atomic>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <mpv/stream_cb.h>
#include "clip.hpp"
#include "preview.hpp"
#include "gpu_debayer.hpp"
#include "defects.hpp"
#include "hevczraw.hpp"

// Decodes preview frames ahead of the reader and caches them. Half-resolution frames are finished
// rgb24 pictures; full-resolution frames are packed Bayer data for the GPU shader (gpu_debayer.hpp).
class FrameServer
{
    const ZrawClip& clip_;
    const bool full_;
    std::vector<uint32_t> defects_;   // hot and dead photosites, repaired in every frame (defects.hpp)
    // One matrix per clip, from the first frame's color temperature; a clip whose white
    // balance changes mid-shot keeps the first frame's matrix
    std::vector<uint8_t> first_data_;
    ZrawFrame first_;
    bool first_ok_ = false;
    int matrix_source_ = -1;
    float matrix_[9];
    std::mutex m_;
    std::condition_variable cv_;
    std::shared_ptr<const PreviewLook> look_;
    uint64_t gen_ = 0;   // bumped on every settings change; older frames are discarded
    struct Entry { uint64_t gen; std::shared_ptr<const std::vector<uint8_t>> rgb; };
    std::map<int, Entry> cache_;
    std::set<int> busy_;
    int want_ = 0;
    bool quit_ = false;
    std::vector<std::thread> workers_;
    int ahead_, capacity_;   // frames decoded ahead of the reader, frames kept in total

    bool cached(int f) const { auto it = cache_.find(f); return it != cache_.end() && it->second.gen == gen_; }

    void set_matrix(int source)
    {
        matrix_source_ = source;
        if (first_ok_) PreviewMatrix(first_, source, matrix_); else FixedPreviewMatrix(matrix_);
    }

    void evict()
    {
        while ((int)cache_.size() > capacity_)
        {
            auto worst = cache_.begin();
            int worst_d = -1;
            for (auto it = cache_.begin(); it != cache_.end(); ++it)
            {
                int d = it->first >= want_ ? it->first - want_ : (want_ - it->first) * 3;   // keep fewer frames behind
                if (it->second.gen != gen_) d = INT_MAX;
                if (d > worst_d) { worst_d = d; worst = it; }
            }
            cache_.erase(worst);
        }
    }

    void work()
    {
        std::vector<uint8_t> data;
        std::vector<uint16_t> cfa;
        std::unique_lock<std::mutex> l(m_);
        while (!quit_)
        {
            int target = -1;
            for (int f = want_; f < std::min(want_ + ahead_, clip_.frames); ++f)
                if (!cached(f) && !busy_.count(f)) { target = f; break; }
            if (target < 0) { cv_.wait(l); continue; }
            busy_.insert(target);
            auto look = look_;
            uint64_t gen = gen_;
            l.unlock();

            auto rgb = std::make_shared<std::vector<uint8_t>>(FrameBytes(), (uint8_t)0);   // black if decoding fails
            ZrawFrame f;
            std::string err;
            if (clip_.Decode(target, data, f, cfa, err))
            {
                RepairDefects(cfa.data(), (int)f.width, (int)f.height, defects_);
                if (full_) PackFullResFrame(f, cfa.data(), rgb->data());
                else look->Render(f, cfa.data(), rgb->data());
            }

            l.lock();
            busy_.erase(target);
            if (gen == gen_) { cache_[target] = {gen, rgb}; evict(); }
            cv_.notify_all();
        }
    }

public:
    FrameServer(const ZrawClip& clip, const PreviewSettings& s, int threads)
        : clip_(clip), full_(s.full_res && FullResSupported(clip.bits))
    {
        std::string err;
        first_ok_ = clip.Read(0, first_data_) && ParseZrawFrame(first_data_.data(), first_data_.size() - 16, first_, err);
        set_matrix(s.matrix);
        look_ = std::make_shared<PreviewLook>(s, matrix_);
        // About 400 MB of cache: 24 frames ahead at half resolution, 16 at 4K (16.6 MB a frame)
        const int budget = (int)(400.0e6 / FrameBytes());
        ahead_ = std::max(6, std::min(24, budget * 7 / 10));
        capacity_ = ahead_ + std::max(2, std::min(8, budget / 5)) + 4;
        defects_ = BuildDefectMap(clip);   // about 0.1 s: samples 8 frames in parallel
        for (int i = 0; i < threads; ++i) workers_.emplace_back([this] { work(); });
    }
    ~FrameServer()
    {
        { std::lock_guard<std::mutex> l(m_); quit_ = true; }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }

    // Camera -> linear sRGB for this clip and the chosen source, row-major (the shader's zm0..zm8)
    const float* Matrix() const { return matrix_; }
    bool FullRes() const { return full_; }
    size_t DefectCount() const { return defects_.size(); }
    const char* Format() const { return full_ ? "rgba64" : "rgb24"; }
    int Width() const { return full_ ? clip_.width / 4 : clip_.width / 2; }
    int Height() const { return full_ ? clip_.height : clip_.height / 2; }
    size_t FrameBytes() const { return (size_t)Width() * Height() * (full_ ? 8 : 3); }

    // Blocks until frame f is ready; null if `cancelled` is set meanwhile. Also moves the read-ahead window to f.
    std::shared_ptr<const std::vector<uint8_t>> Get(int f, const std::atomic<bool>& cancelled)
    {
        std::unique_lock<std::mutex> l(m_);
        if (want_ != f) { want_ = f; evict(); cv_.notify_all(); }
        cv_.wait(l, [&] { return cancelled || quit_ || cached(f); });
        if (!cached(f)) return nullptr;
        return cache_[f].rgb;
    }

    // At half resolution the look is baked into the frames, so they are rendered again. At full
    // resolution the shader applies it; only the matrix is kept here, for ShaderParams.
    void SetSettings(const PreviewSettings& s)
    {
        std::lock_guard<std::mutex> l(m_);
        if (s.matrix != matrix_source_) set_matrix(s.matrix);
        if (full_) return;
        look_ = std::make_shared<PreviewLook>(s, matrix_);
        ++gen_;
        cache_.clear();
        cv_.notify_all();
    }

    void Wake() { cv_.notify_all(); }
};

// What one open clip needs; shared with mpv's stream so it outlives a replaced file.
struct PreviewSource
{
    std::unique_ptr<ZrawClip> clip;
    std::unique_ptr<FrameServer> server;
};

// Fills mpv's stream callbacks for `src`. Reads map byte offsets to frames; nothing here calls mpv.
inline void FillPreviewStream(const std::shared_ptr<PreviewSource>& src, mpv_stream_cb_info* info)
{
    struct Stream { std::shared_ptr<PreviewSource> src; int64_t pos = 0; std::atomic<bool> cancelled{false}; };
    auto s = new Stream;
    s->src = src;
    info->cookie = s;
    info->read_fn = [](void* c, char* buf, uint64_t n) -> int64_t {
        auto s = (Stream*)c;
        auto& server = *s->src->server;
        const int64_t fb = (int64_t)server.FrameBytes(), size = fb * s->src->clip->frames;
        if (s->pos >= size) return 0;
        auto rgb = server.Get((int)(s->pos / fb), s->cancelled);
        if (!rgb) return -1;
        int64_t off = s->pos % fb, take = std::min<int64_t>((int64_t)n, fb - off);
        memcpy(buf, rgb->data() + off, (size_t)take);
        s->pos += take;
        return take;
    };
    info->seek_fn = [](void* c, int64_t offset) -> int64_t {
        auto s = (Stream*)c;
        const int64_t size = (int64_t)s->src->server->FrameBytes() * s->src->clip->frames;
        if (offset < 0 || offset > size) return MPV_ERROR_GENERIC;
        s->pos = offset;
        return offset;
    };
    info->size_fn = [](void* c) -> int64_t { auto s = (Stream*)c; return (int64_t)s->src->server->FrameBytes() * s->src->clip->frames; };
    info->close_fn = [](void* c) { delete (Stream*)c; };
    info->cancel_fn = [](void* c) { auto s = (Stream*)c; s->cancelled = true; s->src->server->Wake(); };
}

// HEVC ZRAW: mpv reads the plain QuickTime file HevcZraw presents and decodes it like any HEVC clip
inline void FillHevcStream(const std::shared_ptr<HevcZraw>& z, mpv_stream_cb_info* info)
{
    struct Stream { std::shared_ptr<HevcZraw> z; uint64_t pos = 0; };
    auto s = new Stream{z};
    info->cookie = s;
    info->read_fn = [](void* c, char* buf, uint64_t n) -> int64_t {
        auto s = (Stream*)c;
        if (s->pos >= s->z->Size()) return 0;
        size_t got = s->z->Read(s->pos, buf, (size_t)std::min<uint64_t>(n, 64u << 20));
        if (!got) return -1;
        s->pos += got;
        return (int64_t)got;
    };
    info->seek_fn = [](void* c, int64_t offset) -> int64_t {
        auto s = (Stream*)c;
        if (offset < 0 || (uint64_t)offset > s->z->Size()) return MPV_ERROR_GENERIC;
        s->pos = (uint64_t)offset;
        return offset;
    };
    info->size_fn = [](void* c) -> int64_t { return (int64_t)((Stream*)c)->z->Size(); };
    info->close_fn = [](void* c) { delete (Stream*)c; };
}

// Live parameters of the 4K debayer shader, as mpv's glsl-shader-opts value
inline std::string ShaderParams(const PreviewSettings& s, const float* matrix)
{
    char p[128];
    snprintf(p, sizeof(p), "exposure=%.4f,wb_red=%.4f,wb_blue=%.4f", s.exposure, s.wb_red, s.wb_blue);
    std::string out = p;
    for (int k = 0; k < 9; ++k) { snprintf(p, sizeof(p), ",zm%d=%.7f", k, matrix[k]); out += p; }
    return out;
}

// Writes the debayer shader to the temp folder (once per process) and returns its path
inline std::filesystem::path WriteDebayerShaderFile()
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::filesystem::path shader = std::filesystem::path(tmp) / ("zraw_debayer_" + std::to_string(GetCurrentProcessId()) + ".glsl");
    if (!std::filesystem::exists(shader)) std::ofstream(shader, std::ios::binary) << DebayerShader();
    return shader;
}

// zrawdng: fast Z CAM ZRAW (true RAW) to CinemaDNG converter.
//
//   zrawdng convert <clip.MOV | folder>... -o <output folder> [--lossy ratio | --quality x [--min-ratio 2] [--max-ratio 8] | --lossless | --uncompressed [--pack]] [--cpu]
//                   [--scale half|quarter] [-j threads]
//       Writes <output>/<clip name>/track_<t>_<frame>.dng plus <clip name>.mov holding the audio and
//       timecode tracks, the same layout zraw-parser produces.
//       By default the DNGs are Blackmagic-style lossy CinemaDNG at 3:1 (12-bit DCT JPEG tiles, the lossy
//       raw Resolve reads); --lossy ratio sets a different ratio (1.5 to 20) relative to 12-bit packed raw.
//       The DCT and rate control run on the GPU when there is one; --cpu keeps them on the CPU, and
//       --gpus 0,1 picks the GPUs by their `zrawdng gpuinfo` number (several share the frames).
//       HEVC ZRAW clips (firmware 0.94 and later, usually .ZRAW files) become <output>/<clip name>/<clip
//       name>.mov, the H.265 stream unscrambled without re-encoding, as zraw-parser does.
//       --lossless writes lossless-JPEG DNGs, byte-identical to zraw-parser's.
//       --uncompressed writes zraw-parser's uncompressed layout (packed samples plus zero padding);
//       --pack drops the padding, 25% smaller for 12-bit footage.
//       --delete-originals (EXPERIMENTAL, untested on real footage): after a clip converts with no errors
//       and every file checks out on disk, move the source clip to the Recycle Bin. Drives without a
//       Recycle Bin keep the clip unless --permanent is also given, which deletes it outright.
//
//   zrawdng play <clip.MOV> [--start frame] [--exposure stops] [--half] [-j threads]
//       Plays a 4K preview (debayered on the GPU), or with --half a CPU-rendered half-resolution
//       preview, in libmpv with audio and full seeking (needs libmpv-2.dll).
//
//   zrawdng hash <clip.MOV> <out.txt> [threads] [first] [count]
//       Decodes frames and writes one fingerprint per frame, same format as zraw-ref, for verification.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <mutex>
#include <thread>
#include <vector>
#include "mov.hpp"
#include "movparse.hpp"
#include "zraw.hpp"
#include "dng.hpp"
#include "preview.hpp"
#include "player.hpp"
#include "delete_original.hpp"
#include "lossy.hpp"
#include "gpu_lossy.hpp"
#include "gpu_denoise.hpp"
#include "hevczraw.hpp"

namespace fs = std::filesystem;
using clk = std::chrono::steady_clock;

static double since(clk::time_point t) { return std::chrono::duration<double>(clk::now() - t).count(); }

static uint64_t fnv1a(const void* p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    auto b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

// TinyMovFileLibrary prints every atom to stdout; zrawdng's own messages go to stderr.
static void silence_stdout()
{
    int nul = _open("NUL", _O_WRONLY);
    if (nul >= 0) { fflush(stdout); _dup2(nul, 1); _close(nul); }
}

static bool is_true_raw(const ZrawTrack& z) { return z.track_index >= 0 && z.version == 0x12EA78D2; }

static bool read_frame(std::ifstream& f, const ZrawTrack& z, int i, std::vector<uint8_t>& data)
{
    data.assign((size_t)z.sizes[i] + 16, 0);   // zero padding for the decoder's 8-byte bit reads
    f.seekg((std::streamoff)z.offsets[i]);
    return (bool)f.read((char*)data.data(), (std::streamsize)z.sizes[i]);
}

// Sequential frame reader using ReadFile directly; frames are stored back to back, so with the
// sequential-scan hint Windows reads ahead and the drive stays busy.
class FrameReader
{
    HANDLE h;
public:
    explicit FrameReader(const fs::path& p)
        : h(CreateFileW(p.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr)) {}
    ~FrameReader() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    bool read(uint64_t offset, uint64_t size, std::vector<uint8_t>& data)
    {
        if (h == INVALID_HANDLE_VALUE) return false;
        data.resize((size_t)size + 16);   // reused buffers: only the padding needs clearing
        memset(data.data() + size, 0, 16);   // zero padding for the decoder's 8-byte bit reads
        LARGE_INTEGER pos; pos.QuadPart = (LONGLONG)offset;
        if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) return false;
        DWORD got = 0;
        return ReadFile(h, data.data(), (DWORD)size, &got, nullptr) && got == size;
    }
};

// Small blocking queue between the disk reader and the workers.
template <class T> class Queue
{
    std::deque<T> q;
    std::mutex m;
    std::condition_variable cv;
    size_t cap;
    bool closed = false;
public:
    explicit Queue(size_t c) : cap(c) {}
    void push(T v) { std::unique_lock<std::mutex> l(m); cv.wait(l, [&] { return q.size() < cap; }); q.push_back(std::move(v)); cv.notify_all(); }
    bool pop(T& v) { std::unique_lock<std::mutex> l(m); cv.wait(l, [&] { return !q.empty() || closed; }); if (q.empty()) return false; v = std::move(q.front()); q.pop_front(); cv.notify_all(); return true; }
    void close() { std::lock_guard<std::mutex> l(m); closed = true; cv.notify_all(); }
};

struct Options { fs::path out; DngStorage storage = DngStorage::LosslessJpeg; bool pack = false; int max_frames = 0; bool no_write = false;
                 DngScale scale = DngScale::Full; int threads = (int)std::thread::hardware_concurrency();
                 bool delete_originals = false, delete_permanently = false;   // delete_*: EXPERIMENTAL, see delete_original.hpp
                 double lossy = 3.0;    // > 0: Blackmagic-style lossy CinemaDNG at this ratio (lossy.hpp), the default
                 double quality = 0;    // > 0: lossy at fixed quality, coding error at most this x the sensor noise
                 double min_ratio = 2, max_ratio = 8;   // fixed quality: the ratio caps per frame
                 bool denoise = false;  // temporal raw denoise on the GPU before encoding (gpu_denoise.hpp)
                 gpu_denoise::Settings dn;                // its strength (--denoise light | strong, --denoise-settings r,t,s)
                 gpu_lossy::Device* dn_device = nullptr;  // the GPU it runs on (filled by cmd_convert)
                 bool gpu = true;       // lossy DCT and rate control on the GPU when one is available (gpu_lossy.hpp)
                 std::vector<gpu_lossy::Device*> gpus;   // the GPUs in use; workers take turns (filled by cmd_convert)
                 ColorSource color = ColorSource::Ours;    // ZRAW Studio's fitted matrices; the alternatives are --zcam-dng-color (alias --zraw-parser-color), --camera-color and --videosuite-color (camera_color.hpp)
                 bool exact = false;          // --zraw-parser-exact: zraw-parser's WhiteLevel too, for byte comparisons
                 bool progress = false; };   // --progress: tab-separated "@" lines on stderr for GUIs (see cmd_convert)

static int halvings(const Options& o) { return o.scale == DngScale::Half ? 1 : o.scale == DngScale::Quarter ? 2 : 0; }

// Lossy CinemaDNG for one frame, after the optional downscale (same as WriteDng does for the other modes).
// quality_scale > 0: the clip's fixed-quality scale (quality_scale below).
static bool write_lossy(const std::string& path, const ZrawFrame& f, std::vector<uint16_t>& cfa, const Options& o,
                        const ZrawTrack& z, lossy::RateState& rate, std::string& err, uint64_t* written, lossy::Backend* backend,
                        double quality_scale)
{
    ZrawFrame g = f;
    for (int i = 0; i < halvings(o); ++i)
    {
        cfa = DownscaleRawTwice(cfa, g.width, g.height);
        g.width /= 2; g.height /= 2;
    }
    lossy::Options lo;
    lo.ratio = o.lossy;
    lo.quality_scale = quality_scale;
    lo.min_ratio = o.min_ratio;
    lo.max_ratio = o.max_ratio;
    lo.fps_num = z.timescale;
    lo.fps_den = z.frame_duration;
    DngColor color;
    if (FrameDngColor(f, o.color, color)) lo.color = &color;   // else zraw-parser's fixed matrices
    if (lossy::WriteLossyDng(path, g, cfa.data(), lo, rate, err, written, nullptr, backend)) return true;
    if (!backend) return false;
    fprintf(stderr, "%s: %s, redone on the CPU\n", fs::path(path).filename().string().c_str(), err.c_str());   // GPU hiccup: this frame on the CPU
    return lossy::WriteLossyDng(path, g, cfa.data(), lo, rate, err, written);
}

static bool skip_clip(const fs::path& clip, const Options& o, const char* why)
{
    if (o.progress) fprintf(stderr, "@skip\t%s\t%s\n", clip.string().c_str(), why);
    else fprintf(stderr, "skip %s: %s\n", clip.filename().string().c_str(), why);
    return true;
}

// HEVC ZRAW: one H.265 .mov in <output>/<clip name>/, same protocol lines as the DNG path
static bool convert_hevc_clip(const fs::path& clip, TinyMovFile& mov, const ZrawTrack& z, const Options& o)
{
    if (z.scheme != 1) return skip_clip(clip, o, "unrecognized HEVC ZRAW variant");
    std::string name = clip.stem().string();
    fs::path dir = o.out / name;
    std::error_code ec;
    if (!o.no_write) fs::create_directories(dir, ec);
    if (ec) { fprintf(stderr, "cannot create %s: %s\n", dir.string().c_str(), ec.message().c_str()); return false; }
    const fs::path out = dir / (name + ".mov");
    const int count = (int)z.offsets.size();   // chunks, one frame each in Z CAM files
    if (o.progress) fprintf(stderr, "@clip\t%s\t%d\t%s\n", clip.string().c_str(), count, dir.string().c_str());
    else fprintf(stderr, "%s: HEVC, %d frames -> %s\n", clip.filename().string().c_str(), count, out.string().c_str());

    const uint64_t clip_size = fs::file_size(clip, ec);   // for --delete-originals
    // The same descrambled file the player streams (hevczraw.hpp): real frame sizes and a keyframe
    // table, so editors seek properly (the camera's own stss lists no keyframes)
    auto t0 = clk::now(), last = t0;
    int frames = 0;
    HevcZraw hz;
    std::string err;
    bool ok = hz.Open(clip, err);
    if (!ok) fprintf(stderr, "%s: %s\n", clip.filename().string().c_str(), err.c_str());
    FILE* f = ok ? _wfopen(o.no_write ? L"NUL" : out.wstring().c_str(), L"wb") : nullptr;   // --no-write: stream everything, keep nothing
    ok = f != nullptr;
    std::vector<uint8_t> buf(8u << 20);
    for (uint64_t pos = 0; ok && pos < hz.Size();)
    {
        size_t n = hz.Read(pos, buf.data(), buf.size());
        ok = n && fwrite(buf.data(), 1, n, f) == n;
        pos += n;
        frames = (int)(pos * count / hz.Size());   // progress by bytes, close enough per frame
        if (since(last) < 0.25 && frames < count) continue;
        last = clk::now();
        if (o.progress) fprintf(stderr, "@progress\t%d\t%d\t%.1f\n", frames, count, frames / since(t0));
        else fprintf(stderr, "\r  %d/%d frames  %.1f fps   ", frames, count, frames / since(t0));
    }
    if (f && fclose(f)) ok = false;
    double secs = since(t0);
    ok = ok && frames == count;
    if (!ok) fprintf(stderr, "%s: writing %s failed after %d of %d frames\n", clip.filename().string().c_str(), out.string().c_str(), frames, count);
    else if (!o.progress) fprintf(stderr, "\r  %d frames in %.1f s, H.265 copied without re-encoding\n", count, secs);
    // EXPERIMENTAL, as for RAW clips. The .mov on disk must hold every byte streamed into it; then
    // DeleteOriginalAfterConversion checks the flush and the unchanged source size before deleting.
    if (o.delete_originals)
    {
        const uint64_t got = fs::file_size(out, ec);
        std::string msg;
        if (!ok || o.no_write) fprintf(stderr, "  kept original: conversion incomplete\n");
        else if (ec || got != hz.Size()) fprintf(stderr, "  kept original: the .mov on disk differs in size from what was written\n");
        else
        {
            DeleteOriginalAfterConversion(clip, out, {out}, {got}, clip_size, o.delete_permanently, msg);
            fprintf(stderr, "  %s\n", msg.c_str());
        }
    }
    if (o.progress) fprintf(stderr, "@done\t%s\t%d\t%d\t%.1f\t%.1f\n", clip.string().c_str(), ok ? 1 : 0, ok ? 0 : count - frames, secs, frames / secs);
    return ok;
}

// The clip's recorded ISO, 0 when it has none (the E2 records it once per clip)
static double clip_iso(const fs::path& clip)
{
    for (auto& kv : ReadClipMeta(clip))
        if (kv.first == "iso" && atof(kv.second.c_str()) > 0) return atof(kv.second.c_str());
    return 0;
}

// Denoise: the frames merged into frame k (k - radius .. k + radius, those inside the clip)
static std::vector<int> dn_window(int k, int count, int r)
{
    std::vector<int> v;
    for (int m = std::max(0, k - r); m <= std::min(count - 1, k + r); ++m) v.push_back(m);
    return v;
}

// Denoises one frame, decoding its whole window itself. Used by fixed-quality calibration, which runs
// before the clip's pipeline, so the Store's first slots are free. `fr` points into one of `data`.
static bool denoise_frame_standalone(const fs::path& clip, const ZrawTrack& z, int count, int k, const Options& o, const lossy::Noise& nz,
                                     gpu_denoise::Store& st, gpu_denoise::Merger& mg, std::vector<uint16_t>& out, ZrawFrame& fr,
                                     std::vector<std::vector<uint8_t>>& data)
{
    const auto win = dn_window(k, count, o.dn.radius);
    const int n = (int)win.size();
    data.assign(n, {});
    std::vector<ZrawFrame> frs(n);
    std::vector<std::vector<uint16_t>> cfas(n);
    std::vector<char> ok(n, 0);
    std::vector<std::thread> ts;
    for (int i = 0; i < n; ++i)
        ts.emplace_back([&, i] {
            FrameReader f(clip);
            std::string err;
            if (!f.read(z.offsets[win[i]], z.sizes[win[i]], data[i]) || !ParseZrawFrame(data[i].data(), data[i].size() - 16, frs[i], err)) return;
            if (frs[i].width != st.W || frs[i].height != st.H) return;
            cfas[i].resize((size_t)frs[i].width * frs[i].height);
            DecodeZrawFrame(frs[i], cfas[i].data());
            ok[i] = 1;
        });
    for (auto& t : ts) t.join();
    const int ref = (int)(std::find(win.begin(), win.end(), k) - win.begin());
    if (!ok[ref]) return false;
    std::vector<uint32_t> slots;
    std::vector<std::pair<int, int>> offs;
    int refpos = 0;
    for (int i = 0; i < n; ++i)
    {
        if (!ok[i]) continue;
        if (i == ref) refpos = (int)slots.size();
        st.Upload((uint32_t)slots.size(), cfas[i].data());
        offs.push_back(i == ref ? std::make_pair(0, 0) : gpu_denoise::Align(cfas[ref].data(), cfas[i].data(), (int)st.W, (int)st.H));
        slots.push_back((uint32_t)slots.size());
    }
    fr = frs[ref];
    out.resize(cfas[ref].size());
    const double black = (fr.black[0] + fr.black[1] + fr.black[2] + fr.black[3]) / 4.0;
    return mg.Run(slots, offs, refpos, black, nz.a, nz.floor2, o.dn, out.data());
}

// Fixed quality: the clip's quantization scale. The CPU measures frames spread through the clip.
// The whole clip uses the strictest scale that keeps every sampled frame within o.quality x the
// noise, so the grain stays constant from frame to frame. The noise model comes from the clip's
// recorded ISO (the E2 records it once per clip). With denoise (st, mg), the sample frames are
// denoised first while the target stays the original noise, which is where denoising saves size.
// Returns 0 when nothing could be measured.
static double quality_scale(const fs::path& clip, const ZrawTrack& z, int count, const Options& o,
                            gpu_denoise::Store* st = nullptr, gpu_denoise::Merger* mg = nullptr)
{
    const double iso = clip_iso(clip);
    const std::string iso_text = iso > 0 ? std::to_string((int)iso) : "unknown (using ISO 500's noise, the strictest)";
    const lossy::Noise nz = lossy::E2Noise(iso);
    const double var_scale = std::pow(0.25, halvings(o));   // a 2x2 average quarters the noise variance
    const int n = std::min(8, count);
    std::vector<double> s(n, 0);
    std::atomic<int> next(0);
    std::vector<std::thread> ts;
    std::mutex dn_m;   // the calibration's denoising shares one Store and Merger
    for (int t = 0; t < std::min(n, o.threads); ++t)
        ts.emplace_back([&] {
            FrameReader f(clip);
            std::vector<uint8_t> data;
            std::vector<std::vector<uint8_t>> window;
            std::vector<uint16_t> cfa;
            lossy::Coefficients co;
            lossy::NoiseMap w;
            for (int i; (i = next++) < n;)
            {
                const int k = (int)((2 * i + 1) * (int64_t)count / (2 * n));
                ZrawFrame fr;
                std::string err;
                if (st)
                {
                    std::lock_guard<std::mutex> l(dn_m);
                    if (!denoise_frame_standalone(clip, z, count, k, o, nz, *st, *mg, cfa, fr, window)) continue;
                }
                else
                {
                    if (!f.read(z.offsets[k], z.sizes[k], data) || !ParseZrawFrame(data.data(), data.size() - 16, fr, err)) continue;
                    cfa.resize((size_t)fr.width * fr.height);
                    DecodeZrawFrame(fr, cfa.data());
                }
                uint32_t W = fr.width, H = fr.height;
                for (int h = 0; h < halvings(o); ++h) { cfa = DownscaleRawTwice(cfa, W, H); W /= 2; H /= 2; }
                lossy::Transform(cfa.data(), (int)W, (int)H, fr.bits(), co);
                lossy::NoiseWeights(co, nz, (fr.black[0] + fr.black[1] + fr.black[2] + fr.black[3]) / 4.0, var_scale, w);
                s[i] = lossy::ScaleForQuality(co, w, o.quality);
            }
        });
    for (auto& t : ts) t.join();
    double best = 0;
    for (double v : s) if (v > 0 && (best == 0 || v < best)) best = v;
    if (best > 0) fprintf(stderr, "fixed quality %.2f x noise, ISO %s: scale %.2f from %d %sframes\n", o.quality, iso_text.c_str(), best, n, st ? "denoised " : "");
    else fprintf(stderr, "fixed quality: no frame could be measured, using %.1f:1 instead\n", o.lossy);
    return best;
}

static bool convert_clip(const fs::path& clip, const Options& o)
{
    // Construct in place, as zraw-parser does: copying a TinyMovFile breaks the later SaveMovFile.
    TinyMovFileReader mov_reader;
    auto mov = mov_reader.OpenMovFile(clip.string());
    auto z = FindZrawTrack(mov);
    if (z.track_index >= 0 && z.version == 0x45A32DEF) return convert_hevc_clip(clip, mov, z, o);
    if (!is_true_raw(z)) return skip_clip(clip, o, "not a ZRAW clip");

    std::string name = clip.stem().string();
    fs::path dir = o.out / name;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) { fprintf(stderr, "cannot create %s: %s\n", dir.string().c_str(), ec.message().c_str()); return false; }

    const int count = o.max_frames > 0 && o.max_frames < (int)z.sizes.size() ? o.max_frames : (int)z.sizes.size();
    if (o.progress) fprintf(stderr, "@clip\t%s\t%d\t%s\n", clip.string().c_str(), count, dir.string().c_str());
    else fprintf(stderr, "%s: %d frames -> %s\n", clip.filename().string().c_str(), count, dir.string().c_str());
    // Denoise: a GPU frame store and a few merge contexts for this clip's frame size (z.width/height are
    // the clip's; every frame is checked against them)
    // Kept across clips of the same frame size, since compiling the shaders and reserving about 1 GB
    // of GPU buffers costs about a second per clip. Clips convert one at a time.
    static std::unique_ptr<gpu_denoise::Store> st_cache;
    static std::vector<std::unique_ptr<gpu_denoise::Merger>> mergers_cache;
    gpu_denoise::Store* st = nullptr;
    std::vector<gpu_denoise::Merger*> mergers;
    const int dn_r = o.dn.radius;
    const lossy::Noise dn_noise = lossy::E2Noise(clip_iso(clip));
    if (o.denoise && o.dn_device && count > 0)
    {
        uint32_t fw = 0, fh = 0;
        {
            FrameReader f(clip);
            std::vector<uint8_t> data;
            ZrawFrame fr;
            std::string err;
            if (f.read(z.offsets[0], z.sizes[0], data) && ParseZrawFrame(data.data(), data.size() - 16, fr, err)) { fw = fr.width; fh = fr.height; }
        }
        const uint32_t slots = (uint32_t)(o.threads + 4 * dn_r + 8);
        if (!st_cache || &st_cache->g != o.dn_device || st_cache->W != fw || st_cache->H != fh || st_cache->slots != slots)
        {
            mergers_cache.clear();
            st_cache.reset();
            st_cache = std::make_unique<gpu_denoise::Store>(*o.dn_device, fw, fh, slots);
            for (int i = 0; i < std::min(3, o.threads) && st_cache->ok(); ++i)
            {
                mergers_cache.push_back(std::make_unique<gpu_denoise::Merger>(*st_cache));
                if (!mergers_cache.back()->ok()) { mergers_cache.pop_back(); break; }
            }
        }
        st = st_cache.get();
        for (auto& m : mergers_cache) mergers.push_back(m.get());
        if (!st->ok() || mergers.empty())
        {
            fprintf(stderr, "denoise unavailable (%s): converting this clip without it\n", st->ok() ? "not enough GPU memory" : st->error.c_str());
            st = nullptr;
            mergers.clear();
            mergers_cache.clear();
            st_cache.reset();
        }
        else fprintf(stderr, "denoise: %d frames per merge (temporal %.1f, spatial %.2f), ISO %.0f noise model\n", 2 * dn_r + 1, o.dn.temporal, o.dn.spatial, clip_iso(clip));
    }
    const bool dn = st != nullptr;
    const double qscale = o.lossy > 0 && o.quality > 0 ? quality_scale(clip, z, count, o, st, dn ? mergers[0] : nullptr) : 0;

    // For --delete-originals: each frame's output path, the bytes written for it, and the source clip's size before conversion
    std::vector<fs::path> dngs(count);
    for (int i = 0; i < count; ++i) dngs[i] = dir / ("track_" + std::to_string(z.track_index) + "_" + std::to_string(i) + ".dng");
    std::vector<uint64_t> written(count, 0);
    const uint64_t clip_size = fs::file_size(clip, ec);

    struct Job { int index; std::vector<uint8_t> data; };
    Queue<Job> queue((size_t)o.threads * 2);
    // Frame buffers go back here after decoding, so the reader reuses them instead of allocating and
    // page-faulting 5.7 MB per frame
    std::vector<std::vector<uint8_t>> spare;
    std::mutex spare_m;
    std::atomic<int> done(0), failed(0);
    // Wakes the progress loop the moment the last frame is done, instead of at its next 250/500 ms tick
    std::mutex fin_m; std::condition_variable fin_cv;
    auto finished = [&] { if (done + failed >= count) { std::lock_guard<std::mutex> l(fin_m); fin_cv.notify_all(); } };
    auto t0 = clk::now();

    std::thread reader([&] {
        FrameReader f(clip);
        for (int i = 0; i < count; ++i)
        {
            Job j{i, {}};
            { std::lock_guard<std::mutex> l(spare_m); if (!spare.empty()) { j.data = std::move(spare.back()); spare.pop_back(); } }
            if (!f.read(z.offsets[i], z.sizes[i], j.data))
            {
                fprintf(stderr, "frame %d: read failed\n", i);
                if (dn) { j.data.clear(); queue.push(std::move(j)); continue; }   // its neighbors still wait for it
                ++failed; finished(); continue;
            }
            queue.push(std::move(j));
        }
        queue.close();
    });

    // Denoise: each frame is decoded once, by whichever worker pops it, and put in a GPU slot; the
    // worker that completes the last frame of some frame's window merges that frame and writes it.
    // Nobody waits for a neighbor, so this can't deadlock; a frame and its slot are freed when the
    // last window that uses it is done. Frames that fail to read or decode are left out of their
    // neighbors' windows.
    struct Decoded { std::vector<uint8_t> data; ZrawFrame fr; std::vector<uint16_t> cfa; int slot = -1, uses = 0; bool ok = false; };
    std::vector<std::unique_ptr<Decoded>> dec(dn ? count : 0);
    std::vector<char> dstate(dn ? count : 0, 0);   // 0 not decoded yet, 1 decoded (or failed), 2 being merged
    std::vector<uint32_t> free_slots;
    if (dn) for (uint32_t s = 0; s < st->slots; ++s) free_slots.push_back(s);
    std::vector<gpu_denoise::Merger*> free_mergers;
    for (auto* m : mergers) free_mergers.push_back(m);
    std::mutex dm, mm;
    std::condition_variable slot_cv, merger_cv;
    std::map<std::pair<int, int>, std::pair<int, int>> pair_off;   // (earlier, later) frame -> later's offset against earlier
    auto denoise_worker = [&](auto& write_frame) {
        std::vector<uint16_t> out;
        auto process = [&](int k) {
            const auto win = dn_window(k, count, dn_r);
            std::vector<Decoded*> ws;
            Decoded* ref;
            {
                std::lock_guard<std::mutex> l(dm);
                for (int m : win) ws.push_back(dec[m].get());
                ref = dec[k].get();
            }
            auto dec_cfa = [&](int m) { return ws[m - win.front()]->cfa.data(); };
            std::string err;
            bool ok = false;
            if (ref->ok)
            {
                std::vector<uint32_t> slots;
                std::vector<std::pair<int, int>> offs;
                int refpos = 0;
                for (size_t i = 0; i < ws.size(); ++i)
                {
                    Decoded* d = ws[i];
                    if (!d->ok) continue;
                    if (d == ref) refpos = (int)slots.size();
                    slots.push_back((uint32_t)d->slot);
                    std::pair<int, int> off(0, 0);
                    if (d != ref)
                    {
                        // Each pair is aligned once: the offset of the later frame against the earlier one,
                        // negated when the reference is the later frame
                        const int m = win[i], lo = std::min(k, m), hi = std::max(k, m);
                        bool have;
                        {
                            std::lock_guard<std::mutex> l(dm);
                            auto it = pair_off.find({lo, hi});
                            have = it != pair_off.end();
                            if (have) off = it->second;
                        }
                        if (!have)
                        {
                            off = gpu_denoise::Align(dec_cfa(lo), dec_cfa(hi), (int)st->W, (int)st->H);
                            std::lock_guard<std::mutex> l(dm);
                            pair_off[{lo, hi}] = off;
                        }
                        if (k > m) off = {-off.first, -off.second};
                    }
                    offs.push_back(off);
                }
                gpu_denoise::Merger* mg;
                {
                    std::unique_lock<std::mutex> l(mm);
                    merger_cv.wait(l, [&] { return !free_mergers.empty(); });
                    mg = free_mergers.back();
                    free_mergers.pop_back();
                }
                out.resize(ref->cfa.size());
                const double black = (ref->fr.black[0] + ref->fr.black[1] + ref->fr.black[2] + ref->fr.black[3]) / 4.0;
                const bool merged = mg->Run(slots, offs, refpos, black, dn_noise.a, dn_noise.floor2, o.dn, out.data());
                { std::lock_guard<std::mutex> l(mm); free_mergers.push_back(mg); }
                merger_cv.notify_one();
                if (merged) ok = write_frame(k, ref->fr, out, err); else err = "denoise failed on the GPU";
            }
            else err = "could not be read or decoded";
            if (ok) ++done; else { fprintf(stderr, "frame %d: %s\n", k, err.c_str()); ++failed; }
            finished();
            {
                std::lock_guard<std::mutex> l(dm);
                for (int m : win)
                    if (--dec[m]->uses == 0)
                    {
                        if (dec[m]->slot >= 0) free_slots.push_back((uint32_t)dec[m]->slot);
                        { std::lock_guard<std::mutex> s(spare_m); spare.push_back(std::move(dec[m]->data)); }
                        dec[m].reset();
                    }
            }
            slot_cv.notify_all();
        };
        Job j;
        for (;;)
        {
            // A slot first, then the next frame: frames then hold slots in order, so the oldest one's
            // neighbors are always in slots too and it can finish. (Taking the slot after decoding
            // would deadlock: frames decoded far ahead fill every slot while the few they need wait.)
            int slot;
            {
                std::unique_lock<std::mutex> l(dm);
                slot_cv.wait(l, [&] { return !free_slots.empty(); });
                slot = (int)free_slots.back();
                free_slots.pop_back();
            }
            if (!queue.pop(j))
            {
                { std::lock_guard<std::mutex> l(dm); free_slots.push_back((uint32_t)slot); }
                slot_cv.notify_all();
                break;
            }
            auto d = std::make_unique<Decoded>();
            d->data = std::move(j.data);
            d->slot = slot;
            std::string err;
            d->ok = d->data.size() > 16 && ParseZrawFrame(d->data.data(), d->data.size() - 16, d->fr, err) && d->fr.width == st->W && d->fr.height == st->H;
            if (d->ok) { d->cfa.resize((size_t)d->fr.width * d->fr.height); DecodeZrawFrame(d->fr, d->cfa.data()); }
            d->uses = (int)dn_window(j.index, count, dn_r).size();   // the windows that include this frame
            if (d->ok) st->Upload((uint32_t)d->slot, d->cfa.data());
            std::vector<int> claims;
            {
                std::lock_guard<std::mutex> l(dm);
                dec[j.index] = std::move(d);
                dstate[j.index] = 1;
                for (int k : dn_window(j.index, count, dn_r))
                {
                    if (dstate[k] != 1) continue;
                    bool ready = true;
                    for (int m : dn_window(k, count, dn_r)) ready = ready && dstate[m] != 0;
                    if (ready) { dstate[k] = 2; claims.push_back(k); }
                }
            }
            for (int k : claims) process(k);
        }
    };

    std::vector<std::thread> workers;
    for (int t = 0; t < o.threads; ++t)
        workers.emplace_back([&, t] {
            Job j;
            std::vector<uint16_t> cfa;
            lossy::RateState rate;   // per worker: the quantization scale carries over between its frames
            // Per-worker GPU buffers. With several GPUs the workers take turns, and since every worker pulls
            // from the same queue, a slower GPU ends up with fewer frames.
            // Kept across clips (one per worker index), since creating 16 of them costs about half a second per clip.
            static std::vector<std::unique_ptr<gpu_lossy::Backend>> pool;
            static std::mutex pool_m;
            gpu_lossy::Backend* gpu = nullptr;
            if (o.lossy > 0 && !o.gpus.empty())
            {
                std::lock_guard<std::mutex> l(pool_m);
                if ((int)pool.size() <= t) pool.resize(t + 1);
                if (!pool[t]) pool[t] = std::make_unique<gpu_lossy::Backend>(*o.gpus[t % o.gpus.size()]);
                gpu = pool[t].get();
            }
            // Writes frame `index` in the chosen format
            auto write_frame = [&](int index, ZrawFrame& fr, std::vector<uint16_t>& img, std::string& err) -> bool {
                if (o.no_write && o.lossy <= 0)
                {
                    const uint8_t* jpeg; size_t len;
                    FastLj92Encode(img.data(), (int)fr.width * 2, (int)fr.height / 2, fr.bits(), jpeg, len);
                    return true;
                }
                const std::string path = dngs[index].string();
                if (o.lossy > 0) return write_lossy(o.no_write ? "NUL" : path, fr, img, o, z, rate, err, &written[index], gpu, qscale);
                DngColor color;
                const bool own = FrameDngColor(fr, o.color, color);
                return WriteDng(path, fr, img, o.storage, o.scale, err, &written[index], own ? &color : nullptr, o.exact);
            };
            if (dn) { denoise_worker(write_frame); return; }
            for (;;)
            {
                // The previous frame is fully written (fr points into its buffer), so the buffer goes back
                if (!j.data.empty()) { std::lock_guard<std::mutex> l(spare_m); spare.push_back(std::move(j.data)); j.data = {}; }
                if (!queue.pop(j)) break;
                ZrawFrame fr;
                std::string err;
                if (ParseZrawFrame(j.data.data(), j.data.size() - 16, fr, err))
                {
                    cfa.resize((size_t)fr.width * fr.height);
                    DecodeZrawFrame(fr, cfa.data());
                    if (write_frame(j.index, fr, cfa, err)) { ++done; finished(); continue; }
                }
                fprintf(stderr, "frame %d: %s\n", j.index, err.c_str());
                ++failed; finished();
            }
        });

    while (done + failed < count)
    {
        { std::unique_lock<std::mutex> l(fin_m); fin_cv.wait_for(l, std::chrono::milliseconds(o.progress ? 250 : 500), [&] { return done + failed >= count; }); }
        if (done + failed >= count) break;
        if (o.progress) fprintf(stderr, "@progress\t%d\t%d\t%.1f\n", done.load(), count, done / since(t0));
        else fprintf(stderr, "\r  %d/%d frames  %.1f fps   ", done.load(), count, done / since(t0));
    }
    reader.join();
    for (auto& w : workers) w.join();
    double secs = since(t0);
    if (o.progress) fprintf(stderr, "@progress\t%d\t%d\t%.1f\n", done.load(), count, done / secs);
    else fprintf(stderr, "\r  %d/%d frames in %.1f s (%.1f fps)%s\n", done.load(), count, secs, done / secs, failed ? "  SOME FRAMES FAILED" : "");

    // Audio + timecode sidecar: the source MOV minus its ZRAW video track, as zraw-parser does
    std::vector<TinyMovTrack> keep;
    for (auto& t : mov.Tracks())
    {
        bool is_zraw = t.Media().Type() == TinyMovTrackMedia::Type_t::Video &&
                       t.Media().Info().DescriptionTable().VideoDescriptionTable().size() == 1 &&
                       t.Media().Info().DescriptionTable().VideoDescriptionTable()[0].DataFormat() == MKTAG('z', 'r', 'a', 'w');
        if (!is_zraw) keep.push_back(t);
    }
    mov.Tracks() = keep;
    TinyMovFileWriter writer;
    const fs::path sidecar = dir / (name + ".mov");
    writer.SaveMovFile(mov, sidecar.string());

    // EXPERIMENTAL: only after a complete, error-free conversion of every frame of the clip
    if (o.delete_originals)
    {
        std::string msg;
        if (failed || done != (int)z.sizes.size() || o.max_frames || o.no_write)
            fprintf(stderr, "  kept original: conversion incomplete\n");
        else
        {
            DeleteOriginalAfterConversion(clip, sidecar, dngs, written, clip_size, o.delete_permanently, msg);
            fprintf(stderr, "  %s\n", msg.c_str());
        }
    }
    if (o.progress) fprintf(stderr, "@done\t%s\t%d\t%d\t%.1f\t%.1f\n", clip.string().c_str(), failed == 0 ? 1 : 0, failed.load(), secs, done / secs);
    return failed == 0;
}

static int cmd_convert(int argc, char** argv)
{
    Options o;
    std::vector<fs::path> inputs;
    bool lossy_given = false, lossless = false;
    std::vector<int> gpu_list;
    for (int i = 2; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) o.out = argv[++i];
        else if (a == "-j" && i + 1 < argc) o.threads = atoi(argv[++i]);
        else if (a == "--uncompressed") o.storage = DngStorage::Uncompressed;
        else if (a == "--pack") o.pack = true;
        else if (a == "--lossless") lossless = true;   // lossless-JPEG DNGs, what zraw-parser writes
        else if (a == "--lossy")   // optional ratio, 3 when left out
        {
            lossy_given = true;
            o.lossy = 3.0;
            if (i + 1 < argc && (isdigit((unsigned char)argv[i + 1][0]) || argv[i + 1][0] == '.')) o.lossy = atof(argv[++i]);
        }
        else if (a == "--cpu") o.gpu = false;   // lossy: keep the DCT and rate control on the CPU
        else if (a == "--curve" && i + 1 < argc) { if (!strcmp(argv[++i], "bmd")) lossy::UseBlackmagicCurve(); }   // lossy: Blackmagic's log curve instead of the noise-matched one
        else if (a == "--quality" && i + 1 < argc) { o.quality = atof(argv[++i]); lossy_given = true; }   // fixed quality, e.g. 0.25 (x sensor noise)
        else if (a == "--denoise")   // temporal raw denoise on the GPU: light (the default) or strong
        {
            o.denoise = true;
            if (i + 1 < argc && !strcmp(argv[i + 1], "strong")) { ++i; o.dn = {3, 8.0f, 1.0f}; }
            else if (i + 1 < argc && !strcmp(argv[i + 1], "light")) ++i;
        }
        else if (a == "--denoise-settings" && i + 1 < argc)   // testing: radius,temporal,spatial
        {
            o.denoise = true;
            if (sscanf(argv[++i], "%d,%f,%f", &o.dn.radius, &o.dn.temporal, &o.dn.spatial) != 3 || o.dn.radius < 1 || o.dn.radius > 3)
                { fprintf(stderr, "--denoise-settings expects radius,temporal,spatial, with radius 1 to 3\n"); return 2; }
        }
        else if (a == "--min-ratio" && i + 1 < argc) o.min_ratio = atof(argv[++i]);   // fixed quality: never bigger than this ratio
        else if (a == "--max-ratio" && i + 1 < argc) o.max_ratio = atof(argv[++i]);   // fixed quality: never smaller than this ratio
        else if (a == "--camera-color") o.color = ColorSource::Camera;            // the camera's own color matrices instead of zraw-parser's fixed ones
        else if (a == "--videosuite-color") o.color = ColorSource::VideoSuite;    // ZRAW VideoSuite's fixed matrix (white-corrected)
        else if (a == "--zcam-dng-color" || a == "--zraw-parser-color") o.color = ColorSource::Fixed;   // Z CAM firmware's DNG matrices (zraw-parser's)
        else if (a == "--zraw-parser-exact") o.exact = true;     // lossless/uncompressed: zraw-parser's bytes exactly (WhiteLevel 4095)
        else if (a == "--gpus" && i + 1 < argc)   // lossy: which GPUs, by gpuinfo number, e.g. 0,1
            for (const char* p = argv[++i]; *p; ++p) { if (isdigit((unsigned char)*p)) { gpu_list.push_back(atoi(p)); while (isdigit((unsigned char)p[1])) ++p; } }
        else if (a == "--progress") o.progress = true;   // GUI protocol: @clip, @progress, @skip, @done lines
        else if (a == "--frames" && i + 1 < argc) o.max_frames = atoi(argv[++i]);   // first N frames only (testing)
        else if (a == "--no-write") o.no_write = true;   // benchmarking: decode + encode, skip the disk
        else if (a == "--delete-originals") o.delete_originals = true;   // EXPERIMENTAL
        else if (a == "--permanent") o.delete_permanently = true;        // with --delete-originals: skip the Recycle Bin
        else if (a == "--scale" && i + 1 < argc) { std::string s = argv[++i]; o.scale = s == "half" ? DngScale::Half : s == "quarter" ? DngScale::Quarter : DngScale::Full; }
        else inputs.push_back(a);
    }
    if (o.pack) o.storage = DngStorage::Packed;   // --pack implies uncompressed
    if (o.exact) o.color = ColorSource::Fixed;    // zraw-parser's bytes include its matrices
    // Lossy 3:1 is the default; --lossless, --uncompressed and --pack switch it off
    const bool lossless_mode = lossless || o.storage != DngStorage::LosslessJpeg;
    if (lossy_given && lossless_mode) { fprintf(stderr, "--lossy cannot be combined with --lossless, --uncompressed or --pack\n"); return 2; }
    if (lossless_mode) o.lossy = 0;
    if (o.lossy > 0 && (o.lossy < 1.5 || o.lossy > 20)) { fprintf(stderr, "--lossy ratio must be between 1.5 and 20\n"); return 2; }
    if (o.quality < 0 || (o.quality > 0 && (o.min_ratio < 1.5 || o.max_ratio > 20 || o.min_ratio >= o.max_ratio)))
        { fprintf(stderr, "--quality needs a positive value and 1.5 <= --min-ratio < --max-ratio <= 20\n"); return 2; }
    if (o.lossy > 0)
    {
        std::string why, names;
        if (gpu_list.empty()) gpu_list.push_back(0);   // the fastest GPU
        for (int g : gpu_list)
        {
            if (!o.gpu) break;
            std::string w;
            gpu_lossy::Device* d = gpu_lossy::Device::Get(g, &w);
            if (!d) { why += (why.empty() ? "" : "; ") + w; continue; }
            if (std::find(o.gpus.begin(), o.gpus.end(), d) != o.gpus.end()) continue;
            o.gpus.push_back(d);
            names += (names.empty() ? "" : " + ") + d->name;
        }
        char mode[96];
        if (o.quality > 0) snprintf(mode, sizeof mode, "lossy, fixed quality %.2f x noise (%.1f to %.1f:1)", o.quality, o.min_ratio, o.max_ratio);
        else snprintf(mode, sizeof mode, "lossy %.2f:1", o.lossy);
        if (!o.gpus.empty()) fprintf(stderr, "%s, DCT and rate control on %s%s%s\n", mode, names.c_str(), why.empty() ? "" : " (not used: ", why.empty() ? "" : (why + ")").c_str());
        else fprintf(stderr, "%s on the CPU%s%s\n", mode, o.gpu ? ", GPU unavailable: " : "", o.gpu ? why.c_str() : "");
    }
    if (o.denoise)   // on the first lossy GPU, or the fastest one
    {
        std::string why;
        o.dn_device = !o.gpus.empty() ? o.gpus[0] : gpu_lossy::Device::Get(0, &why);
        if (!o.dn_device) { fprintf(stderr, "denoise needs a GPU (%s): converting without it\n", why.c_str()); o.denoise = false; }
        else fprintf(stderr, "denoise on %s\n", o.dn_device->name.c_str());
    }
    if (inputs.empty() || o.out.empty() || o.threads < 1) { fprintf(stderr, "usage: zrawdng convert <clip.MOV | folder>... -o <output folder> [--lossy ratio | --quality x [--min-ratio 2] [--max-ratio 8] | --lossless | --uncompressed [--pack]] [--cpu] [--denoise [light|strong]] [--scale half|quarter] [-j threads] [--zcam-dng-color | --camera-color | --videosuite-color] [--delete-originals [--permanent]]\n"); return 2; }

    std::vector<fs::path> clips;
    for (auto& in : inputs)
    {
        if (fs::is_directory(in))
        {
            for (auto& e : fs::directory_iterator(in))
                if (e.is_regular_file() && (_stricmp(e.path().extension().string().c_str(), ".mov") == 0 || _stricmp(e.path().extension().string().c_str(), ".zraw") == 0)) clips.push_back(e.path());
        }
        else clips.push_back(in);
    }
    std::sort(clips.begin(), clips.end());

    if (o.delete_permanently && !o.delete_originals) { fprintf(stderr, "--permanent requires --delete-originals\n"); return 2; }
    if (o.delete_originals)
        fprintf(stderr, "EXPERIMENTAL: --delete-originals is on. Each source clip is %s after its conversion is complete and verified%s.\n",
                o.delete_permanently ? "PERMANENTLY DELETED" : "moved to the Recycle Bin",
                o.lossy > 0 ? " (the converted copies are lossy)" : "");

    silence_stdout();
    bool ok = true;
    for (auto& c : clips) ok &= convert_clip(c, o);
    return ok ? 0 : 1;
}

static int cmd_hash(int argc, char** argv)
{
    if (argc < 4) { fprintf(stderr, "usage: zrawdng hash <clip.MOV> <out.txt> [threads] [first] [count]\n"); return 2; }
    silence_stdout();
    std::string clip = argv[2];
    int threads = argc > 4 ? atoi(argv[4]) : (int)std::thread::hardware_concurrency();

    TinyMovFileReader mov_reader;
    auto mov = mov_reader.OpenMovFile(clip);
    auto z = FindZrawTrack(mov);
    if (!is_true_raw(z)) { fprintf(stderr, "no true-RAW ZRAW track (version 0x%X)\n", z.version); return 1; }

    int first = argc > 5 ? atoi(argv[5]) : 0;
    int count = argc > 6 ? atoi(argv[6]) : (int)z.sizes.size() - first;
    std::vector<uint64_t> hash(count);
    std::vector<int> ww(count), hh(count);
    std::atomic<int> next(0), failed(0);

    auto t0 = clk::now();
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            std::ifstream f(clip, std::ios::binary);
            std::vector<uint8_t> data;
            std::vector<uint16_t> cfa;
            int k;
            while ((k = next++) < count)
            {
                int i = first + k;
                ZrawFrame fr;
                std::string err;
                if (!read_frame(f, z, i, data) || !ParseZrawFrame(data.data(), data.size() - 16, fr, err)) { fprintf(stderr, "frame %d: %s\n", i, err.c_str()); ++failed; continue; }
                cfa.resize((size_t)fr.width * fr.height);
                DecodeZrawFrame(fr, cfa.data());
                hash[k] = fnv1a(cfa.data(), cfa.size() * 2);
                ww[k] = (int)fr.width; hh[k] = (int)fr.height;
            }
        });
    for (auto& t : pool) t.join();
    double s = since(t0);

    FILE* out = fopen(argv[3], "w");
    for (int k = 0; k < count; ++k) fprintf(out, "%d %016llx %dx%d\n", first + k, (unsigned long long)hash[k], ww[k], hh[k]);
    fclose(out);
    fprintf(stderr, "%d frames in %.2f s (%.1f fps, %d threads)\n", count, s, count / s, threads);
    return failed ? 1 : 0;
}

// Single-thread time per stage, to see where a frame's time goes.
static int cmd_bench(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: zrawdng bench <clip.MOV> [frames] [scratch.dng]\n"); return 2; }
    silence_stdout();
    std::string clip = argv[2];
    TinyMovFileReader mov_reader;
    auto mov = mov_reader.OpenMovFile(clip);
    auto z = FindZrawTrack(mov);
    if (!is_true_raw(z)) { fprintf(stderr, "no true-RAW ZRAW track\n"); return 1; }
    int frames = argc > 3 ? atoi(argv[3]) : 20;
    if (frames > (int)z.sizes.size()) frames = (int)z.sizes.size();

    std::ifstream f(clip, std::ios::binary);
    std::vector<uint8_t> data;
    std::vector<uint16_t> cfa;
    const uint8_t* jpeg;
    size_t jpeg_len;
    double t_read = 0, t_decode = 0, t_lj92 = 0, t_dng = 0;
    std::string scratch = argc > 4 ? argv[4] : "";   // optional file path: also time a full DNG write there
    for (int i = 0; i < frames; ++i)
    {
        auto t = clk::now();
        read_frame(f, z, i, data);
        t_read += since(t); t = clk::now();
        ZrawFrame fr;
        std::string err;
        if (!ParseZrawFrame(data.data(), data.size() - 16, fr, err)) { fprintf(stderr, "frame %d: %s\n", i, err.c_str()); return 1; }
        if (i == 0) fprintf(stderr, "%ux%u, %d-bit, lossless flag %u, bayer mode %u\n", fr.width, fr.height, fr.bits(), fr.is_lossless, fr.bayer_mode);
        cfa.resize((size_t)fr.width * fr.height);
        DecodeZrawFrame(fr, cfa.data());
        t_decode += since(t); t = clk::now();
        FastLj92Encode(cfa.data(), (int)fr.width * 2, (int)fr.height / 2, fr.bits(), jpeg, jpeg_len);
        t_lj92 += since(t); t = clk::now();
        if (!scratch.empty()) WriteDng(scratch, fr, cfa, DngStorage::LosslessJpeg, DngScale::Full, err);
        t_dng += since(t);
    }
    fprintf(stderr, "per frame, 1 thread, %d frames: read %.1f ms, decode %.1f ms, lossless JPEG %.1f ms\n",
            frames, 1000 * t_read / frames, 1000 * t_decode / frames, 1000 * t_lj92 / frames);
    if (!scratch.empty()) fprintf(stderr, "full DNG write (encode again + assemble + file): %.1f ms\n", 1000 * t_dng / frames);
#ifdef ZRAW_PROFILE
    double total = (double)(g_prof[0] + g_prof[1] + g_prof[2] + g_prof[3]);
    fprintf(stderr, "decode split: main loop %.0f%%, output copy %.0f%%, smoothing %.0f%%, bayer reorder %.0f%%\n",
            100 * g_prof[0] / total, 100 * g_prof[1] / total, 100 * g_prof[2] / total, 100 * g_prof[3] / total);
    fprintf(stderr, "coverage: lines at noise 0/1/2 = %llu/%llu/%llu, raw blocks %llu, VL blocks %llu, blocks with diff>0 %llu, lossless frames %llu of %llu\n",
            g_cov[0], g_cov[1], g_cov[2], g_cov[3], g_cov[4], g_cov[5], g_cov[6], g_cov[7]);
#endif
    return 0;
}

// Plays a clip in libmpv (mpv's own window) with full seeking, scrubbing and frame stepping.
static int cmd_play(int argc, char** argv)
{
    std::string clip, still;
    int start = 0, threads = 6;
    PreviewSettings settings;
    for (int i = 2; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--start" && i + 1 < argc) start = atoi(argv[++i]);
        else if (a == "--exposure" && i + 1 < argc) settings.exposure = atof(argv[++i]);
        else if (a == "-j" && i + 1 < argc) threads = atoi(argv[++i]);
        else if (a == "--still" && i + 1 < argc) still = argv[++i];
        else if (a == "--half") settings.full_res = false;
        else clip = a;
    }
    if (clip.empty()) { fprintf(stderr, "usage: zrawdng play <clip.MOV | clip.ZRAW> [--start frame] [--exposure stops] [--half] [-j threads] [--still out.ppm]\n"); return 2; }
    silence_stdout();

    if (!still.empty())   // render one frame to a PPM and stop, for checking the look
    {
        ZrawClip c;
        std::string err;
        std::vector<uint8_t> data;
        std::vector<uint16_t> cfa;
        ZrawFrame f;
        if (!c.Open(clip, err) || !c.Decode(start, data, f, cfa, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
        std::vector<uint8_t> rgb((size_t)(c.width / 2) * (c.height / 2) * 3);
        RepairDefects(cfa.data(), c.width, c.height, BuildDefectMap(c));   // as the player does
        float m[9];
        PreviewMatrix(f, settings.matrix, m);
        PreviewLook(settings, m).Render(f, cfa.data(), rgb.data());
        FILE* o = fopen(still.c_str(), "wb");
        if (!o) { fprintf(stderr, "cannot write %s\n", still.c_str()); return 1; }
        fprintf(o, "P6\n%d %d\n255\n", c.width / 2, c.height / 2);
        fwrite(rgb.data(), 1, rgb.size(), o);
        fclose(o);
        return 0;
    }

    ZrawPlayer player(threads);
    std::string err;
    if (!player.Create(nullptr, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    player.SetSettings(settings);
    if (!player.Open(clip, err, start)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    fprintf(stderr, "playing %s. Seek with the arrow keys or the on-screen bar; Ctrl+Up/Down exposure (Ctrl+Shift finer), Ctrl+Left/Right warmth, Ctrl+0 reset.\n", clip.c_str());
    player.WaitUntilClosed();
    return 0;
}

// Study of fixed quality against fixed ratio: writes a CSV of the fixed-quality measure and the exact
// entropy-coded size against the quantization scale, for 8 frames spread through each true-RAW clip.
// Each row also holds the measure per brightness band (b0..b9, see lossy::NoiseMap) and each band's
// share of the frame (n0..n9). --curve bmd: Blackmagic's curve instead of the noise-matched one.
//   zrawdng qstudy <out.csv> <clip.MOV>... [-j threads] [--curve bmd]
static int cmd_qstudy(int argc, char** argv)
{
    if (argc < 4) { fprintf(stderr, "usage: zrawdng qstudy <out.csv> <clip.MOV>... [-j threads] [--curve bmd]\n"); return 2; }
    int threads = (int)std::thread::hardware_concurrency();
    std::vector<fs::path> clips;
    for (int i = 3; i < argc; ++i)
        if (!strcmp(argv[i], "-j") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--curve") && i + 1 < argc) { if (!strcmp(argv[++i], "bmd")) lossy::UseBlackmagicCurve(); }
        else clips.push_back(argv[i]);
    struct Job { size_t clip; int frame; };
    std::vector<Job> jobs;
    std::vector<MovZraw> movs(clips.size());
    std::vector<double> isos(clips.size(), 0);
    for (size_t c = 0; c < clips.size(); ++c)
    {
        std::string err;
        if (!ParseZrawMov(clips[c], movs[c], err) || movs[c].version != 0x12EA78D2) { fprintf(stderr, "skip %s\n", clips[c].filename().string().c_str()); continue; }
        for (auto& kv : ReadClipMeta(clips[c])) if (kv.first == "iso") isos[c] = atof(kv.second.c_str());
        const int count = (int)movs[c].offsets.size(), n = std::min(8, count);
        for (int i = 0; i < n; ++i) jobs.push_back({c, (int)((2 * i + 1) * (int64_t)count / (2 * n))});
    }
    std::vector<std::string> rows(jobs.size());
    std::atomic<int> next(0);
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
        ts.emplace_back([&] {
            std::vector<uint8_t> data;
            std::vector<uint16_t> cfa;
            lossy::Coefficients co;
            lossy::NoiseMap nm;
            for (int j; (j = next++) < (int)jobs.size();)
            {
                const Job& job = jobs[j];
                const MovZraw& m = movs[job.clip];
                FrameReader f(clips[job.clip]);
                ZrawFrame fr;
                std::string err;
                if (!f.read(m.offsets[job.frame], m.sizes[job.frame], data) || !ParseZrawFrame(data.data(), data.size() - 16, fr, err)) continue;
                cfa.resize((size_t)fr.width * fr.height);
                DecodeZrawFrame(fr, cfa.data());
                lossy::Transform(cfa.data(), (int)fr.width, (int)fr.height, fr.bits(), co);
                const double black = (fr.black[0] + fr.black[1] + fr.black[2] + fr.black[3]) / 4.0;
                lossy::NoiseWeights(co, lossy::E2Noise(isos[job.clip]), black, 1.0, nm);
                std::string shares;
                for (int b = 0; b < lossy::kBands; ++b) shares += "," + std::to_string(nm.band_n[b] / (double)nm.w.size()).substr(0, 7);
                char line[512];
                for (int k = 0; k < 40; ++k)
                {
                    const double s = 0.7 * std::pow(400 / 0.7, k / 39.0);
                    const double bytes = lossy::CountBits(co, s) / 8.0 * 1.004 + 4 * 660;   // + headers; stuffing as at 3:1
                    double bands[lossy::kBands];
                    const double frame = lossy::NoiseRatio(co, nm, s, bands);
                    int n = snprintf(line, sizeof line, "%s,%.0f,%d,%u,%u,%.1f,%.4f,%.0f,%.5f", clips[job.clip].stem().string().c_str(), isos[job.clip], job.frame,
                                     fr.width, fr.height, black, s, bytes, frame);
                    for (int b = 0; b < lossy::kBands; ++b) n += snprintf(line + n, sizeof line - n, ",%.5f", bands[b]);
                    rows[j] += line + shares + "\n";
                }
                fprintf(stderr, "\r  %d/%d frames ", j + 1, (int)jobs.size());
            }
        });
    for (auto& t : ts) t.join();
    FILE* o = fopen(argv[2], "w");
    if (!o) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    fprintf(o, "clip,iso,frame,width,height,black,scale,bytes,measure");
    for (int b = 0; b < lossy::kBands; ++b) fprintf(o, ",b%d", b);
    for (int b = 0; b < lossy::kBands; ++b) fprintf(o, ",n%d", b);
    fprintf(o, "\n");
    for (auto& r : rows) fputs(r.c_str(), o);
    fclose(o);
    fprintf(stderr, "\n%d frames from %d clips -> %s\n", (int)jobs.size(), (int)clips.size(), argv[2]);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc >= 2 && !strcmp(argv[1], "qstudy")) return cmd_qstudy(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "play")) return cmd_play(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "bench")) return cmd_bench(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "convert")) return cmd_convert(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "gpuinfo"))   // for GUIs: "@gpu\t<number>\t<name>\t<why not usable>" per GPU, fastest first
    {
        auto adapters = gpu_lossy::Device::Adapters();
        int usable = 0;
        for (int i = 0; i < (int)adapters.size(); ++i)
        {
            std::string why;
            bool ok = gpu_lossy::Device::Get(i, &why) != nullptr;
            usable += ok;
            fprintf(stderr, "@gpu\t%d\t%s\t%s\n", i, gpu_lossy::Device::Name(adapters[i].Get()).c_str(), ok ? "" : why.c_str());
        }
        if (adapters.empty()) fprintf(stderr, "@gpu\t-1\t\tno hardware GPU\n");
        return usable ? 0 : 1;
    }
    if (argc >= 2 && !strcmp(argv[1], "hash")) return cmd_hash(argc, argv);
    fprintf(stderr,
        "zrawdng convert <clip.MOV | folder>... -o <output folder> [--lossy ratio | --quality x [--min-ratio 2] [--max-ratio 8] | --lossless | --uncompressed [--pack]] [--cpu] [--denoise [light|strong]] [--scale half|quarter] [-j threads] [--delete-originals [--permanent]]\n"
        "zrawdng play <clip.MOV> [--start frame] [--exposure stops] [--half] [-j threads]\n"
        "zrawdng hash <clip.MOV> <out.txt> [threads] [first] [count]\n");
    return 2;
}

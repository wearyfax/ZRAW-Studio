// HEVC ZRAW: the clips Z CAM firmware 0.94 and later records as "ZRAW" (E2 1.0.9 names them .ZRAW).
// Inside is an ordinary HEVC Main 10 4:2:0 stream, scrambled two ways (zraw-parser's
// ZrawNewExtractor.hpp, process_zraw_XORred):
//  - the video track's chunk offsets are XORed with the stco/co64 version+flags word, which holds
//    the movie's creation time;
//  - each frame starts with the NAL unit count and the unit sizes, XORed with the low byte of the
//    creation time. The NAL units follow, rotated by (creation time mod count) places, and the first
//    0x2000..0x8000 bytes of each unit are XORed with that unit's own first bytes and with the
//    frame's last 4 bytes.
//
// HevcZraw presents such a clip as a plain QuickTime file that mpv (FFmpeg) and Windows' Media
// Foundation read like any camera file, audio included. It serves the file's own bytes with three
// changes: each video frame is descrambled as it is read (stored as length-prefixed NAL units, so the
// sample shrinks by its small header); the old moov is renamed 'free'; and a rewritten moov is
// appended at the end. The new sample table has an 'hev1' entry (hvcC built from the clip's own
// parameter sets), the real frame offsets and sizes, and a keyframe table (IDR every 24 frames on the
// E2, P frames between).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

class HevcZraw
{
public:
    using ReadFn = std::function<bool(uint64_t offset, void* buf, size_t n)>;   // exact read from the real file

    int frames = 0, width = 0, height = 0;
    double fps = 0;
    std::vector<int> keyframes;   // 0-based frame numbers

    HevcZraw() = default;
    HevcZraw(const HevcZraw&) = delete;
    HevcZraw& operator=(const HevcZraw&) = delete;
    ~HevcZraw() { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }

    bool Open(const std::filesystem::path& path, std::string& err)
    {
        file_ = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) { err = "cannot open file"; return false; }
        LARGE_INTEGER size;
        GetFileSizeEx(file_, &size);
        HANDLE h = file_;
        return Open([h](uint64_t off, void* buf, size_t n) {
            OVERLAPPED ov = {};
            ov.Offset = (DWORD)off;
            ov.OffsetHigh = (DWORD)(off >> 32);
            DWORD got = 0;
            return n <= 0xFFFFFFFFu && ReadFile(h, buf, (DWORD)n, &got, &ov) && got == n;
        }, (uint64_t)size.QuadPart, err);
    }

    bool Open(ReadFn read, uint64_t file_size, std::string& err)
    {
        read_ = std::move(read);
        file_size_ = file_size;
        return Parse(err);
    }

    // Size and contents of the plain QuickTime file
    uint64_t Size() const { return file_size_ + moov_.size(); }

    // Up to n bytes at `offset`; returns the count (0 at the end, or if the file can't be read)
    size_t Read(uint64_t offset, void* buf, size_t n)
    {
        if (offset >= Size() || !n) return 0;
        uint8_t* out = (uint8_t*)buf;
        if (offset >= file_size_)
        {
            n = (size_t)std::min<uint64_t>(n, Size() - offset);
            memcpy(out, moov_.data() + (offset - file_size_), n);
            return n;
        }
        n = (size_t)std::min<uint64_t>(n, file_size_ - offset);
        // Inside a frame? (by_offset_ is sorted by offset)
        auto it = std::upper_bound(by_offset_.begin(), by_offset_.end(), offset, [&](uint64_t o, int s) { return o < samples_[s].offset; });
        if (it != by_offset_.begin())
        {
            const Sample& s = samples_[*(it - 1)];
            if (offset < s.offset + s.plain_size)
            {
                std::lock_guard<std::mutex> l(m_);
                const std::vector<uint8_t>* f = Frame(*(it - 1));
                if (!f) return 0;
                n = (size_t)std::min<uint64_t>(n, s.offset + s.plain_size - offset);
                memcpy(out, f->data() + (offset - s.offset), n);
                return n;
            }
        }
        if (it != by_offset_.end()) n = (size_t)std::min<uint64_t>(n, samples_[*it].offset - offset);   // stop at the next frame
        if (!read_(offset, out, n)) return 0;
        // The old moov becomes a 'free' atom
        for (int i = 0; i < 4; ++i)
            if (old_moov_type_ + i >= offset && old_moov_type_ + i < offset + n) out[old_moov_type_ + i - offset] = "free"[i];
        return n;
    }

    // One frame as an HEVC Annex B access unit (start codes), for decoders fed frame by frame
    bool FrameAnnexB(int index, std::vector<uint8_t>& out)
    {
        std::lock_guard<std::mutex> l(m_);
        const std::vector<uint8_t>* f = index >= 0 && index < frames ? Frame(index) : nullptr;
        if (!f) return false;
        out = *f;
        for (size_t p = 0; p + 4 <= out.size();)
        {
            uint32_t len = be32(&out[p]);
            out[p] = out[p + 1] = out[p + 2] = 0;
            out[p + 3] = 1;
            p += 4 + (size_t)len;
        }
        return true;
    }

private:
    struct Sample
    {
        uint64_t offset = 0;
        uint32_t size = 0;         // scrambled, as stored
        uint32_t plain_size = 0;   // descrambled: the NAL units with 4-byte lengths
        bool key = false;
    };

    HANDLE file_ = INVALID_HANDLE_VALUE;
    ReadFn read_;
    uint64_t file_size_ = 0, old_moov_type_ = 0;
    uint32_t key_ = 0;   // the movie's creation time
    std::vector<Sample> samples_;
    std::vector<int> by_offset_;
    std::vector<uint8_t> moov_;   // the rewritten moov atom, appended after the file
    std::mutex m_;
    int cached_[2] = {-1, -1};     // last two frames read (the demuxer reads one, then audio, then the next)
    std::vector<uint8_t> cache_[2];

    static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
    static uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
    static void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 3; i >= 0; --i) v.push_back((uint8_t)(x >> (8 * i))); }
    static void put64(std::vector<uint8_t>& v, uint64_t x) { put32(v, (uint32_t)(x >> 32)); put32(v, (uint32_t)x); }
    static void put16(std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }

    struct Atom { const uint8_t* body; size_t size; const uint8_t* start; size_t total; char type[5]; };

    static std::vector<Atom> children(const uint8_t* p, size_t n)
    {
        std::vector<Atom> out;
        for (size_t pos = 0; pos + 8 <= n;)
        {
            uint64_t size = be32(p + pos);
            size_t hdr = 8;
            if (size == 1 && pos + 16 <= n) { size = be64(p + pos + 8); hdr = 16; }
            else if (size == 0) size = n - pos;
            if (size < hdr || pos + size > n) break;
            Atom a{p + pos + hdr, (size_t)(size - hdr), p + pos, (size_t)size, {}};
            memcpy(a.type, p + pos + 4, 4);
            out.push_back(a);
            pos += (size_t)size;
        }
        return out;
    }
    static const Atom* find(const std::vector<Atom>& atoms, const char* type)
    {
        for (auto& a : atoms) if (!memcmp(a.type, type, 4)) return &a;
        return nullptr;
    }
    static void atom(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& body)
    {
        put32(out, (uint32_t)(8 + body.size()));
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), body.begin(), body.end());
    }

    // The frame's header: NAL unit count and sizes, and the rotation
    struct Layout { int count = 0, rotation = 0; uint32_t sizes[31] = {}; uint32_t total = 0; };

    bool layout(const uint8_t* h, size_t avail, uint32_t stored, Layout& L) const
    {
        const uint8_t x = (uint8_t)key_;
        if (avail < 1) return false;
        L.count = (uint8_t)(h[0] ^ x);
        if (L.count < 1 || L.count > 31 || avail < 1 + 4 * (size_t)L.count) return false;
        uint64_t total = 0;
        for (int i = 0; i < L.count; ++i)
        {
            const uint8_t* p = h + 1 + 4 * i;
            L.sizes[i] = (uint32_t)(uint8_t)(p[0] ^ x) << 24 | (uint32_t)(uint8_t)(p[1] ^ x) << 16 | (uint32_t)(uint8_t)(p[2] ^ x) << 8 | (uint8_t)(p[3] ^ x);
            if (L.sizes[i] < 5) return false;
            total += L.sizes[i];
        }
        if (total + 1 + 4 * L.count > stored) return false;
        L.total = (uint32_t)total;
        L.rotation = (int)((L.count - key_ % L.count) % L.count);
        return true;
    }

    // Where NAL unit i (in stream order) is stored, relative to the frame start
    static uint32_t position(const Layout& L, int i)
    {
        uint32_t head = 1 + 4 * L.count, pos = head;
        if (i < L.rotation)
        {
            for (int k = L.rotation; k < L.count; ++k) pos += L.sizes[k];
            for (int k = 0; k < i; ++k) pos += L.sizes[k];
        }
        else
            for (int k = L.rotation; k < i; ++k) pos += L.sizes[k];
        return pos;
    }

    // How many leading bytes of a NAL unit are XORed, from its first 4 stored bytes (zraw-parser's `hm`)
    static uint32_t scrambled_length(const Layout& L, int i, const uint8_t* first4)
    {
        uint32_t hm = first4[3] | (uint32_t)first4[2] << 8;
        if (hm < 0x2000 || hm > 0x8000) hm = 0x4000;
        if (hm > L.sizes[i]) hm = L.sizes[i];
        if (i == L.count - 1 && L.sizes[i] - hm < 4) hm = L.sizes[i] - 4;
        return hm;
    }

    // Descrambles a stored frame into NAL units with 4-byte big-endian lengths
    bool descramble(const uint8_t* in, uint32_t stored, std::vector<uint8_t>& out) const
    {
        Layout L;
        if (!layout(in, stored, stored, L)) return false;
        out.resize(L.total);
        for (int i = 0, o = 0; i < L.count; o += L.sizes[i], ++i) memcpy(&out[o], in + position(L, i), L.sizes[i]);
        const uint8_t tail[4] = {out[L.total - 4], out[L.total - 3], out[L.total - 2], out[L.total - 1]};
        uint8_t* p = out.data();
        for (int i = 0; i < L.count; p += L.sizes[i], ++i)
        {
            const uint32_t hm = scrambled_length(L, i, p);
            const uint8_t k1 = p[1], k2 = p[2], k3 = p[3];
            for (uint32_t k = 0; k < (hm - 4) >> 3; ++k)
            {
                uint8_t* q = p + 8 * k;
                q[4] ^= k1; q[5] ^= k1; q[6] ^= k2; q[7] ^= k3;
                q[8] ^= tail[0]; q[9] ^= tail[1]; q[10] ^= tail[2]; q[11] ^= tail[3];
            }
            for (uint32_t l = (hm - 4) & 7; l > 0; --l) p[hm - l] ^= tail[0];
            const uint32_t len = L.sizes[i] - 4;
            p[0] = (uint8_t)(len >> 24); p[1] = (uint8_t)(len >> 16); p[2] = (uint8_t)(len >> 8); p[3] = (uint8_t)len;
        }
        return true;
    }

    // Is this frame an IRAP (IDR/CRA) frame? Reads only the start of each NAL unit.
    bool is_key(const Sample& s, const uint8_t* head, size_t head_size) const
    {
        Layout L;
        if (!layout(head, head_size, s.size, L)) return false;
        for (int i = 0; i < L.count; ++i)
        {
            uint8_t b[6];
            uint32_t at = position(L, i);
            if (at + 6 <= head_size) memcpy(b, head + at, 6);
            else if (!read_(s.offset + at, b, 6)) return false;
            // A NAL unit under 12 bytes keeps its type byte XORed with the frame tail; none is a slice
            if (scrambled_length(L, i, b) < 12) continue;
            int type = ((b[4] ^ b[1]) >> 1) & 0x3F;
            if (type >= 16 && type <= 21) return true;
        }
        return false;
    }

    const std::vector<uint8_t>* Frame(int index)   // m_ held
    {
        for (int i = 0; i < 2; ++i) if (cached_[i] == index) return &cache_[i];
        const Sample& s = samples_[index];
        std::vector<uint8_t> stored(s.size);
        int slot = cached_[0] == -1 ? 0 : cached_[1] == -1 ? 1 : (cached_[0] < cached_[1] ? 0 : 1);   // evicts the lower frame number
        if (!read_(s.offset, stored.data(), s.size)) return nullptr;
        if (!descramble(stored.data(), s.size, cache_[slot])) cache_[slot].swap(stored);   // not scrambled after all: pass it on
        cache_[slot].resize(s.plain_size);
        cached_[slot] = index;
        return &cache_[slot];
    }

    // hvcC from the parameter sets in an IDR frame (length-prefixed NAL units)
    static bool build_hvcc(const std::vector<uint8_t>& f, std::vector<uint8_t>& hvcc)
    {
        std::vector<std::vector<uint8_t>> ps[3];   // VPS, SPS, PPS
        for (size_t p = 0; p + 6 <= f.size();)
        {
            uint32_t len = be32(&f[p]);
            if (p + 4 + len > f.size()) break;
            int type = (f[p + 4] >> 1) & 0x3F;
            if (type >= 32 && type <= 34) ps[type - 32].emplace_back(f.begin() + p + 4, f.begin() + p + 4 + len);
            p += 4 + (size_t)len;
        }
        if (ps[0].empty() || ps[1].empty() || ps[2].empty()) return false;
        // SPS payload without emulation prevention bytes: header (2), then one byte with
        // max_sub_layers_minus1 and the nesting flag, then the 12-byte general profile_tier_level
        std::vector<uint8_t> sps;
        const auto& raw = ps[1][0];
        for (size_t i = 2, zeros = 0; i < raw.size() && sps.size() < 13; ++i)
        {
            if (zeros >= 2 && raw[i] == 3) { zeros = 0; continue; }
            zeros = raw[i] == 0 ? zeros + 1 : 0;
            sps.push_back(raw[i]);
        }
        if (sps.size() < 13) return false;
        hvcc.clear();
        hvcc.push_back(1);
        hvcc.insert(hvcc.end(), sps.begin() + 1, sps.begin() + 13);
        // Chroma 4:2:0 and 10-bit written as constants; every HEVC ZRAW clip is Main 10 4:2:0
        const uint8_t fixed[] = {0xF0, 0x00, 0xFC, 0xFD, 0xFA, 0xFA, 0x00, 0x00};
        hvcc.insert(hvcc.end(), fixed, fixed + sizeof(fixed));
        int layers = ((sps[0] >> 1) & 7) + 1, nested = sps[0] & 1;
        hvcc.push_back((uint8_t)(layers << 3 | nested << 2 | 3));   // 4-byte NAL lengths
        hvcc.push_back(3);
        for (int t = 0; t < 3; ++t)
        {
            hvcc.push_back((uint8_t)(0x80 | (32 + t)));   // complete: no parameter sets elsewhere are needed
            put16(hvcc, (uint32_t)ps[t].size());
            for (auto& n : ps[t]) { put16(hvcc, (uint32_t)n.size()); hvcc.insert(hvcc.end(), n.begin(), n.end()); }
        }
        return true;
    }

    bool Parse(std::string& err)
    {
        // Top-level atoms: find moov (Z CAM writes it after mdat)
        std::vector<uint8_t> moov;
        uint64_t moov_off = 0;
        size_t moov_hdr = 8;
        for (uint64_t pos = 0; pos + 8 <= file_size_;)
        {
            uint8_t h[16];
            if (!read_(pos, h, 8)) break;
            uint64_t size = be32(h);
            size_t hl = 8;
            if (size == 1) { if (!read_(pos + 8, h + 8, 8)) break; size = be64(h + 8); hl = 16; }
            else if (size == 0) size = file_size_ - pos;
            if (size < hl || pos + size > file_size_) break;
            if (!memcmp(h + 4, "moov", 4))
            {
                if (size > (64u << 20)) break;
                moov.resize((size_t)size);
                if (!read_(pos, moov.data(), moov.size())) moov.clear();
                moov_off = pos;
                moov_hdr = hl;
                break;
            }
            pos += size;
        }
        if (moov.empty()) { err = "no readable moov atom"; return false; }
        old_moov_type_ = moov_off + 4;
        auto top = children(moov.data() + moov_hdr, moov.size() - moov_hdr);

        const Atom* mvhd = find(top, "mvhd");
        if (!mvhd || mvhd->size < 8) { err = "no mvhd"; return false; }
        key_ = mvhd->body[0] == 1 ? be32(mvhd->body + 8) : be32(mvhd->body + 4);   // low 32 bits of a v1 creation time

        // The video track with a 'zraw' sample entry of version 0x45A32DEF
        const Atom* trak = nullptr;
        std::vector<Atom> st;
        uint32_t timescale = 0, duration = 0;
        for (auto& t : top)
        {
            if (memcmp(t.type, "trak", 4)) continue;
            auto tk = children(t.body, t.size);
            const Atom* mdia = find(tk, "mdia");
            if (!mdia) continue;
            auto md = children(mdia->body, mdia->size);
            const Atom *mdhd = find(md, "mdhd"), *hdlr = find(md, "hdlr"), *minf = find(md, "minf");
            if (!mdhd || !hdlr || !minf || hdlr->size < 12 || memcmp(hdlr->body + 8, "vide", 4)) continue;
            auto mi = children(minf->body, minf->size);
            const Atom* stbl = find(mi, "stbl");
            if (!stbl) continue;
            auto s = children(stbl->body, stbl->size);
            const Atom* stsd = find(s, "stsd");
            if (!stsd || stsd->size < 16 + 78 || be32(stsd->body + 4) != 1 || memcmp(stsd->body + 12, "zraw", 4)) continue;
            uint32_t entry = be32(stsd->body + 8);
            if (entry < 8 + 78 || 8 + entry > stsd->size) continue;
            auto kids = children(stsd->body + 16 + 78, entry - 8 - 78);
            const Atom* ext = find(kids, "zraw");
            if (!ext || ext->size < 4) continue;
            if (be32(ext->body) != 0x45A32DEF) { err = "not an HEVC ZRAW clip"; return false; }
            if (ext->size < 12 || be32(ext->body + 8) != 1)
            {
                err = "unrecognized HEVC ZRAW variant (the 'zraw' box's third word is not 1)";
                return false;
            }
            width = stsd->body[16 + 24] << 8 | stsd->body[16 + 25];
            height = stsd->body[16 + 26] << 8 | stsd->body[16 + 27];
            timescale = be32(mdhd->body + (mdhd->body[0] == 1 ? 20 : 12));
            const Atom* stts = find(s, "stts");
            if (stts && stts->size >= 16 && be32(stts->body + 4) > 0) duration = be32(stts->body + 12);
            trak = &t;
            st = s;
            break;
        }
        if (!trak) { err = "no ZRAW video track"; return false; }
        fps = duration ? (double)timescale / duration : 0;

        // Frame sizes, then chunk offsets (XORed with the box's version+flags word) and sample-to-chunk
        const Atom *stsz = find(st, "stsz"), *stsc = find(st, "stsc"), *stco = find(st, "stco"), *co64 = find(st, "co64");
        if (!stsz || !stsc || (!stco && !co64) || stsz->size < 12 || stsc->size < 8) { err = "incomplete sample table"; return false; }
        uint32_t fixed = be32(stsz->body + 4), count = be32(stsz->body + 8);
        if (!fixed && stsz->size < 12 + 4ull * count) { err = "truncated stsz"; return false; }
        samples_.resize(count);
        for (uint32_t i = 0; i < count; ++i) samples_[i].size = fixed ? fixed : be32(stsz->body + 12 + 4 * i);

        const Atom* co = co64 ? co64 : stco;
        uint32_t x = be32(co->body);
        uint32_t nchunks = be32(co->body + 4) ^ x;
        if (co->size < 8 + (co64 ? 8ull : 4ull) * nchunks) { err = "truncated chunk offsets"; return false; }
        std::vector<uint64_t> chunks(nchunks);
        for (uint32_t i = 0; i < nchunks; ++i)
            chunks[i] = co64 ? be64(co->body + 8 + 8 * i) ^ (x ? 0xFFFFFFFF00000000ull | x : 0) : (uint64_t)(be32(co->body + 8 + 4 * i) ^ x);
        uint32_t runs = be32(stsc->body + 4);
        if (!runs || stsc->size < 8 + 12ull * runs) { err = "bad stsc"; return false; }
        uint32_t n = 0;
        for (uint32_t c = 0; c < nchunks && n < count; ++c)
        {
            uint32_t per = 0;
            for (uint32_t r = 0; r < runs && be32(stsc->body + 8 + 12 * r) <= c + 1; ++r) per = be32(stsc->body + 8 + 12 * r + 4);
            uint64_t off = chunks[c];
            for (uint32_t k = 0; k < per && n < count; ++k, ++n) { samples_[n].offset = off; off += samples_[n].size; }
        }
        if (n != count || !count) { err = "sample table mismatch"; return false; }
        for (auto& s : samples_)
            if (s.offset + s.size > file_size_) { err = "frame past the end of the file (clip cut short?)"; return false; }

        // Each frame's header: real size and keyframe flag
        // One or two small reads per frame, serial; a 27 GB clip is ~10k frames, about a second on an SSD
        for (int i = 0; i < (int)count; ++i)
        {
            Sample& s = samples_[i];
            uint8_t head[256];
            size_t hs = std::min<size_t>(sizeof(head), s.size);
            if (!read_(s.offset, head, hs)) { err = "cannot read frame " + std::to_string(i); return false; }
            Layout L;
            s.plain_size = layout(head, hs, s.size, L) ? L.total : s.size;
            s.key = is_key(s, head, hs);
            if (s.key) keyframes.push_back(i);
        }
        if (keyframes.empty() || keyframes[0] != 0) { err = "the clip does not start with a keyframe"; return false; }
        frames = (int)count;
        by_offset_.resize(count);
        for (uint32_t i = 0; i < count; ++i) by_offset_[i] = (int)i;
        std::sort(by_offset_.begin(), by_offset_.end(), [&](int a, int b) { return samples_[a].offset < samples_[b].offset; });

        std::vector<uint8_t> first, hvcc;
        {
            std::lock_guard<std::mutex> l(m_);
            const std::vector<uint8_t>* f = Frame(0);
            if (f) first = *f;
        }
        if (!build_hvcc(first, hvcc)) { err = "no parameter sets in the first frame (not the XOR kind of HEVC ZRAW?)"; return false; }

        // The new moov: everything copied, except the ZRAW track's sample table
        std::function<void(const uint8_t*, size_t, std::vector<uint8_t>&)> copy = [&](const uint8_t* p, size_t size, std::vector<uint8_t>& out) {
            for (auto& a : children(p, size))
            {
                bool inside = a.body <= trak->body + trak->size && a.body + a.size >= trak->body;   // on the path to our stbl
                if (inside && (!memcmp(a.type, "trak", 4) || !memcmp(a.type, "mdia", 4) || !memcmp(a.type, "minf", 4)))
                {
                    if (!memcmp(a.type, "trak", 4) && a.body != trak->body) { out.insert(out.end(), a.start, a.start + a.total); continue; }
                    std::vector<uint8_t> body;
                    copy(a.body, a.size, body);
                    atom(out, a.type, body);
                }
                else if (inside && !memcmp(a.type, "stbl", 4))
                {
                    std::vector<uint8_t> body;
                    write_stbl(a, hvcc, body);
                    atom(out, "stbl", body);
                }
                else
                    out.insert(out.end(), a.start, a.start + a.total);
            }
        };
        std::vector<uint8_t> body;
        copy(moov.data() + moov_hdr, moov.size() - moov_hdr, body);
        moov_.clear();
        atom(moov_, "moov", body);
        return true;
    }

    void write_stbl(const Atom& stbl, const std::vector<uint8_t>& hvcc, std::vector<uint8_t>& out)
    {
        bool need64 = false;
        for (auto& s : samples_) need64 |= s.offset > 0xFFFFFFFFull;
        for (auto& a : children(stbl.body, stbl.size))
        {
            std::vector<uint8_t> b;
            if (!memcmp(a.type, "stsd", 4))
            {
                // 'hev1' entry: same visual fields and children, with hvcC in place of the 'zraw' box
                const uint8_t* e = a.body + 8;
                uint32_t esize = be32(e);
                std::vector<uint8_t> entry(e + 8, e + 8 + 78);
                for (auto& c : children(e + 8 + 78, esize - 8 - 78))
                    if (!memcmp(c.type, "zraw", 4)) atom(entry, "hvcC", hvcc);
                    else entry.insert(entry.end(), c.start, c.start + c.total);
                put32(b, 0);
                put32(b, 1);
                atom(b, "hev1", entry);
                atom(out, "stsd", b);
            }
            else if (!memcmp(a.type, "stsz", 4))
            {
                put32(b, 0); put32(b, 0); put32(b, (uint32_t)samples_.size());
                for (auto& s : samples_) put32(b, s.plain_size);
                atom(out, "stsz", b);
            }
            else if (!memcmp(a.type, "stsc", 4))   // one frame per chunk, so frames can shrink in place
            {
                put32(b, 0); put32(b, 1); put32(b, 1); put32(b, 1); put32(b, 1);
                atom(out, "stsc", b);
            }
            else if (!memcmp(a.type, "stco", 4) || !memcmp(a.type, "co64", 4))
            {
                put32(b, 0); put32(b, (uint32_t)samples_.size());
                for (auto& s : samples_) if (need64) put64(b, s.offset); else put32(b, (uint32_t)s.offset);
                atom(out, need64 ? "co64" : "stco", b);
            }
            else if (!memcmp(a.type, "stss", 4)) {}   // written below from the frames themselves
            else out.insert(out.end(), a.start, a.start + a.total);
        }
        std::vector<uint8_t> b;
        put32(b, 0); put32(b, (uint32_t)keyframes.size());
        for (int k : keyframes) put32(b, (uint32_t)k + 1);
        atom(out, "stss", b);
    }
};

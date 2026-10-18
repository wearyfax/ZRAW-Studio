// Small, silent MOV parser for the ZRAW video track: frame offsets and sizes, frame rate, ZRAW version.
// Used by the player and the mpv plugin, where TinyMovFileLibrary's console output would end up in
// mpv's terminal. Seeks over mdat, so it reads only the moov atom (Z CAM writes it at the end).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include "portable.hpp"
#endif

struct MovZraw
{
    int track_index = -1;      // position among the file's trak atoms
    uint32_t version = 0;      // 0x12EA78D2 = true RAW, 0x45A32DEF = encrypted HEVC
    std::vector<uint64_t> offsets, sizes;
    uint32_t timescale = 0, frame_duration = 0;

    double fps() const { return frame_duration ? (double)timescale / frame_duration : 0; }
};

namespace movparse_detail
{
    inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
    inline uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

    struct Atom { const uint8_t* body; size_t size; char type[5]; };

    // Children of [p, p+n); stops at the first malformed header
    inline std::vector<Atom> children(const uint8_t* p, size_t n)
    {
        std::vector<Atom> out;
        for (size_t pos = 0; pos + 8 <= n;)
        {
            uint64_t size = be32(p + pos);
            size_t hdr = 8;
            if (size == 1 && pos + 16 <= n) { size = be64(p + pos + 8); hdr = 16; }
            else if (size == 0) size = n - pos;
            if (size < hdr || pos + size > n) break;
            Atom a{p + pos + hdr, (size_t)(size - hdr), {}};
            memcpy(a.type, p + pos + 4, 4);
            out.push_back(a);
            pos += (size_t)size;
        }
        return out;
    }

    inline const Atom* find(const std::vector<Atom>& atoms, const char* type)
    {
        for (auto& a : atoms) if (!memcmp(a.type, type, 4)) return &a;
        return nullptr;
    }
}

// The file's moov atom (its body), read without touching mdat.
inline bool LoadMoov(const std::filesystem::path& path, std::vector<uint8_t>& moov, std::string& err)
{
    using namespace movparse_detail;
    moov.clear();
#ifdef _WIN32
    HANDLE h = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = "cannot open file"; return false; }
    LARGE_INTEGER li;
    GetFileSizeEx(h, &li);
    const uint64_t fsize = (uint64_t)li.QuadPart;
    auto read_at = [&](uint64_t off, void* buf, uint32_t n) {
        OVERLAPPED ov = {};
        ov.Offset = (DWORD)off;
        ov.OffsetHigh = (DWORD)(off >> 32);
        DWORD got = 0;
        return ReadFile(h, buf, n, &got, &ov) && got == n;
    };
#else
    PosixFile f(path);
    if (!f.ok()) { err = "cannot open file"; return false; }
    const uint64_t fsize = f.size();
    auto read_at = [&](uint64_t off, void* buf, uint32_t n) { return f.read_at(off, buf, n); };
#endif
    for (uint64_t pos = 0; pos + 8 <= fsize;)
    {
        uint8_t hdr[16];
        if (!read_at(pos, hdr, 8)) break;
        uint64_t size = be32(hdr);
        size_t hlen = 8;
        if (size == 1) { if (!read_at(pos + 8, hdr + 8, 8)) break; size = be64(hdr + 8); hlen = 16; }
        else if (size == 0) size = fsize - pos;
        if (size < hlen) break;
        if (!memcmp(hdr + 4, "moov", 4))
        {
            if (size > (64u << 20)) break;
            moov.resize((size_t)(size - hlen));
            if (!read_at(pos + hlen, moov.data(), (uint32_t)moov.size())) moov.clear();
            break;
        }
        pos += size;
    }
#ifdef _WIN32
    CloseHandle(h);
#endif
    if (moov.empty()) { err = "no readable moov atom"; return false; }
    return true;
}

// Z CAM's per-clip metadata (moov/meta, QuickTime 'keys' + 'ilst'), text values only, keys with
// their "com.zcam.camera." / "com.apple.proapps." prefix dropped: iso, shutterSpeed, aperture,
// noiseReduction, lut (the preview look), temperature and so on. Per clip, not per frame.
inline std::vector<std::pair<std::string, std::string>> ReadClipMeta(const std::filesystem::path& path)
{
    using namespace movparse_detail;
    std::vector<std::pair<std::string, std::string>> out;
    std::vector<uint8_t> moov;
    std::string err;
    if (!LoadMoov(path, moov, err)) return out;
    const Atom* meta = nullptr;
    auto top = children(moov.data(), moov.size());
    if (!(meta = find(top, "meta"))) return out;
    auto m = children(meta->body, meta->size);
    const Atom *keys = find(m, "keys"), *ilst = find(m, "ilst");
    if (!keys || !ilst || keys->size < 8) return out;
    std::vector<std::string> names;
    for (size_t p = 8, n = be32(keys->body + 4); n-- && p + 8 <= keys->size;)
    {
        uint32_t sz = be32(keys->body + p);
        if (sz < 8 || p + sz > keys->size) break;
        std::string k((const char*)keys->body + p + 8, sz - 8);
        for (const char* pre : {"com.zcam.camera.", "com.apple.proapps."})
            if (!k.compare(0, strlen(pre), pre)) k = k.substr(strlen(pre));
        names.push_back(k);
        p += sz;
    }
    for (auto& item : children(ilst->body, ilst->size))
    {
        uint32_t index = be32((const uint8_t*)item.type);   // the item's "type" is its 1-based key index
        const auto kids = children(item.body, item.size);   // kept alive: `data` points into it
        const Atom* data = find(kids, "data");
        if (!data || data->size < 8 || index == 0 || index > names.size() || be32(data->body) != 1) continue;   // 1 = UTF-8
        out.emplace_back(names[index - 1], std::string((const char*)data->body + 8, data->size - 8));
    }
    return out;
}

// The clip's start timecode, from its QuickTime timecode track ('tmcd'): the frame count in its one
// sample, the timecode's frame base (24 for 23.976 footage), whether it is drop-frame, and the reel
// name from the sample description. Z CAM records one per clip; video frame i is start + i.
struct ClipTimecode { bool ok = false; uint32_t start = 0, base = 0; bool drop = false; std::string reel; };

inline ClipTimecode ReadTimecode(const std::filesystem::path& path)
{
    using namespace movparse_detail;
    ClipTimecode tc;
    std::vector<uint8_t> moov;
    std::string err;
    if (!LoadMoov(path, moov, err)) return tc;
    for (auto& trak : children(moov.data(), moov.size()))
    {
        if (memcmp(trak.type, "trak", 4)) continue;
        auto t = children(trak.body, trak.size);
        const Atom* mdia = find(t, "mdia");
        if (!mdia) continue;
        auto md = children(mdia->body, mdia->size);
        const Atom *hdlr = find(md, "hdlr"), *minf = find(md, "minf");
        if (!hdlr || !minf || hdlr->size < 12 || memcmp(hdlr->body + 8, "tmcd", 4)) continue;
        auto mi = children(minf->body, minf->size);
        const Atom* stbl = find(mi, "stbl");
        if (!stbl) continue;
        auto st = children(stbl->body, stbl->size);
        const Atom *stsd = find(st, "stsd"), *stco = find(st, "stco"), *co64 = find(st, "co64");
        // Sample description: size, 'tmcd', 6 reserved, data reference (2), 4 reserved, flags (4),
        // timescale (4), frame duration (4), frames per second (1), 1 reserved, then a 'name' atom
        if (!stsd || (!stco && !co64) || stsd->size < 8 + 34 || memcmp(stsd->body + 12, "tmcd", 4)) continue;
        const uint8_t* e = stsd->body + 8;
        const uint32_t esize = be32(e);
        if (esize < 34 || 8ull + esize > stsd->size) continue;   // 64-bit: 8 + 0xFFFFFFF8 wraps to 0
        tc.drop = (be32(e + 20) & 1) != 0;
        tc.base = e[32];
        for (auto& k : children(e + 34, esize - 34))
            if (!memcmp(k.type, "name", 4) && k.size >= 4)
                tc.reel.assign((const char*)k.body + 4, std::min<size_t>(k.size - 4, ((size_t)k.body[0] << 8) | k.body[1]));
        const Atom* co = co64 ? co64 : stco;
        if (co->size < 12 || be32(co->body + 4) == 0) continue;
        const uint64_t off = co64 ? be64(co->body + 8) : be32(co->body + 8);
        std::ifstream f(path, std::ios::binary);
        uint8_t sample[4];
        if (!f.seekg((std::streamoff)off) || !f.read((char*)sample, 4)) continue;
        tc.start = be32(sample);
        tc.ok = tc.base > 0;
        return tc;
    }
    return tc;
}

// Video frame `frame` of a clip as an 8-byte SMPTE 12M timecode, the layout of the DNG TimeCodes tag:
// frames, seconds, minutes, hours in BCD (drop-frame flag in bit 6 of the frames byte), then 4 bytes
// of user bits, left zero.
inline void TimecodeBytes(const ClipTimecode& tc, uint32_t frame, uint8_t out[8])
{
    const uint64_t base = tc.base;
    uint64_t n = (uint64_t)tc.start + frame;
    if (tc.drop && base % 30 == 0)   // 29.97 / 59.94 drop-frame: skip 2 (4) frame numbers each minute, except every tenth
    {
        const uint64_t d = base / 15, per10 = base * 600 - 9 * d, per1 = base * 60 - d, tens = n / per10, rem = n % per10;
        n += 9 * d * tens + (rem > d ? d * ((rem - d) / per1) : 0);
    }
    const uint32_t ff = (uint32_t)(n % base), ss = (uint32_t)(n / base % 60), mm = (uint32_t)(n / (base * 60) % 60), hh = (uint32_t)(n / (base * 3600) % 24);
    auto bcd = [](uint32_t v) { return (uint8_t)((v / 10) << 4 | (v % 10)); };
    out[0] = (uint8_t)(bcd(ff) | (tc.drop ? 0x40 : 0));
    out[1] = bcd(ss); out[2] = bcd(mm); out[3] = bcd(hh);
    out[4] = out[5] = out[6] = out[7] = 0;
}

// How the clip was shot, from its metadata, for reference in the DNGs: lens name, focal length,
// f-number and focus distance (only with an electronic lens), ISO and shutter. Z CAM records these
// once per clip, so every frame gets the same values; 0 or empty means not recorded.
struct ClipShot { std::string lens; double focal_mm = 0, fnumber = 0, focus_mm = 0; uint32_t iso = 0, shutter_num = 0, shutter_den = 0; };

inline ClipShot ReadClipShot(const std::filesystem::path& path, uint32_t fps_num, uint32_t fps_den)
{
    ClipShot s;
    double angle = 0;
    for (auto& kv : ReadClipMeta(path))
    {
        const std::string& v = kv.second;
        const char* c = v.c_str();
        if (kv.first == "lensType") s.lens = v;
        else if (kv.first == "lensFocalLength") s.focal_mm = atof(c);                         // "24mm"
        else if (kv.first == "aperture") s.fnumber = atof(c + (c[0] == 'F' || c[0] == 'f'));   // "F4.5"
        else if (kv.first == "focusDistance") s.focus_mm = atof(c);                           // "595mm", 327675 = infinity
        else if (kv.first == "iso") s.iso = (uint32_t)atoi(c);
        else if (kv.first == "shutterSpeed")                                                  // "1/60"
        {
            unsigned n = 0, d = 0;
            if (sscanf(c, "%u/%u", &n, &d) == 2 && n && d) { s.shutter_num = n; s.shutter_den = d; }
            else if (atof(c) > 0) { s.shutter_num = (uint32_t)(atof(c) * 1000 + 0.5); s.shutter_den = 1000; }
        }
        else if (kv.first == "shutterAngle") angle = atof(c);                                // "180°"
    }
    if (!s.shutter_den && angle > 0 && fps_num && fps_den)   // exposure = angle / 360 / fps
    {
        s.shutter_num = (uint32_t)(angle * 10 + 0.5) * fps_den;
        s.shutter_den = 3600u * fps_num;
    }
    return s;
}

// A finished TIFF tag (little-endian data), for either DNG writer
struct RawTag { uint16_t tag, type; uint32_t count; std::vector<uint8_t> data; };

// The clip's shooting settings as standard EXIF / TIFF-EP tags in the DNG's main IFD: ExposureTime
// (33434), FNumber (33437), ISOSpeedRatings (34855), SubjectDistance (37382, focus distance in m),
// FocalLength (37386) and LensModel (42036). Only the ones the clip recorded.
inline std::vector<RawTag> ShotTags(const ClipShot& s)
{
    std::vector<RawTag> t;
    auto rational = [&](uint16_t tag, uint32_t num, uint32_t den) {
        RawTag r{tag, 5, 1, std::vector<uint8_t>(8)};
        memcpy(r.data.data(), &num, 4); memcpy(r.data.data() + 4, &den, 4);
        t.push_back(r);
    };
    if (s.shutter_num && s.shutter_den) rational(33434, s.shutter_num, s.shutter_den);
    if (s.fnumber > 0) rational(33437, (uint32_t)(s.fnumber * 100 + 0.5), 100);
    if (s.iso > 0 && s.iso <= 65535)
    {
        RawTag r{34855, 3, 1, std::vector<uint8_t>(2)};
        const uint16_t v = (uint16_t)s.iso;
        memcpy(r.data.data(), &v, 2);
        t.push_back(r);
    }
    if (s.focus_mm >= 327675) rational(37382, 0xFFFFFFFFu, 1);   // the lens's "infinity" reading; EXIF's infinity
    else if (s.focus_mm > 0) rational(37382, (uint32_t)(s.focus_mm + 0.5), 1000);
    if (s.focal_mm > 0) rational(37386, (uint32_t)(s.focal_mm * 100 + 0.5), 100);
    if (!s.lens.empty())
    {
        RawTag r{42036, 2, 0, std::vector<uint8_t>(s.lens.begin(), s.lens.end())};
        r.data.resize(std::max<size_t>(r.data.size() + 1, 4), 0);   // with its null; tinydngwriter can't store a 3-byte value
        r.count = (uint32_t)r.data.size();
        t.push_back(r);
    }
    return t;
}

// What a DNG frame carries about time and shooting (zrawdng writes it unless --zraw-parser-exact)
struct FrameStamp { const ClipTimecode* tc = nullptr; uint32_t frame = 0, fps_num = 0, fps_den = 0; const ClipShot* shot = nullptr; };

// Finds the first video track whose single sample description is 'zraw' with a 'zraw' extension.
inline bool ParseZrawMov(const std::filesystem::path& path, MovZraw& out, std::string& err)
{
    using namespace movparse_detail;
    out = MovZraw();
    std::vector<uint8_t> moov;
    if (!LoadMoov(path, moov, err)) return false;

    int index = -1;
    for (auto& trak : children(moov.data(), moov.size()))
    {
        if (memcmp(trak.type, "trak", 4)) continue;
        ++index;
        auto t = children(trak.body, trak.size);
        const Atom* mdia = find(t, "mdia");
        if (!mdia) continue;
        auto md = children(mdia->body, mdia->size);
        const Atom *mdhd = find(md, "mdhd"), *hdlr = find(md, "hdlr"), *minf = find(md, "minf");
        if (!mdhd || !hdlr || !minf || hdlr->size < 12 || memcmp(hdlr->body + 8, "vide", 4)) continue;
        auto mi = children(minf->body, minf->size);
        const Atom* stbl = find(mi, "stbl");
        if (!stbl) continue;
        auto st = children(stbl->body, stbl->size);
        const Atom *stsd = find(st, "stsd"), *stsz = find(st, "stsz"), *stsc = find(st, "stsc"), *stts = find(st, "stts");
        const Atom *stco = find(st, "stco"), *co64 = find(st, "co64");
        if (!stsd || !stsz || !stsc || (!stco && !co64) || stsd->size < 16) continue;

        // One sample description, format 'zraw', with a 'zraw' child carrying the version
        if (be32(stsd->body + 4) != 1 || memcmp(stsd->body + 12, "zraw", 4)) continue;
        uint32_t entry_size = be32(stsd->body + 8);
        const size_t visual_fields = 78;   // VisualSampleEntry fields after the 8-byte header
        if (entry_size < 8 + visual_fields || 8ull + entry_size > stsd->size) continue;   // 64-bit: 8 + 0xFFFFFFF8 wraps to 0
        auto kids = children(stsd->body + 8 + 8 + visual_fields, entry_size - 8 - visual_fields);
        const Atom* ext = find(kids, "zraw");
        if (!ext || ext->size < 4) continue;

        MovZraw z;
        z.track_index = index;
        z.version = be32(ext->body);
        z.timescale = be32(mdhd->body + (mdhd->body[0] == 1 ? 20 : 12));
        if (stts && stts->size >= 16 && be32(stts->body + 4) > 0) z.frame_duration = be32(stts->body + 12);

        uint32_t fixed = be32(stsz->body + 4), count = be32(stsz->body + 8);
        if (!fixed && stsz->size < 12 + 4ull * count) { err = "truncated stsz"; return false; }
        z.sizes.resize(count);
        for (uint32_t i = 0; i < count; ++i) z.sizes[i] = fixed ? fixed : be32(stsz->body + 12 + 4 * i);

        std::vector<uint64_t> chunks;
        const Atom* co = co64 ? co64 : stco;
        // HEVC ZRAW XORs the count and offsets with the box's version+flags word (0 in other files)
        const uint32_t x = be32(co->body);
        uint32_t nchunks = be32(co->body + 4) ^ x;
        size_t width = co64 ? 8 : 4;
        if (co->size < 8 + width * (uint64_t)nchunks) { err = "truncated chunk offsets"; return false; }
        for (uint32_t i = 0; i < nchunks; ++i)
            chunks.push_back(co64 ? be64(co->body + 8 + 8 * i) ^ (x ? 0xFFFFFFFF00000000ull | x : 0) : (uint64_t)(be32(co->body + 8 + 4 * i) ^ x));

        // Sample-to-chunk runs: samples are stored back to back inside each chunk
        uint32_t runs = be32(stsc->body + 4);
        if (stsc->size < 8 + 12ull * runs || !runs) { err = "bad stsc"; return false; }
        z.offsets.resize(count);
        uint32_t s = 0;
        for (uint32_t c = 0; c < nchunks && s < count; ++c)
        {
            uint32_t spc = 0;
            for (uint32_t r = 0; r < runs && be32(stsc->body + 8 + 12 * r) <= c + 1; ++r) spc = be32(stsc->body + 8 + 12 * r + 4);
            uint64_t off = chunks[c];
            for (uint32_t k = 0; k < spc && s < count; ++k, ++s) { z.offsets[s] = off; off += z.sizes[s]; }
        }
        if (s != count) { err = "sample table mismatch"; return false; }
        out = std::move(z);
        return true;
    }
    err = "no ZRAW video track";
    return false;
}

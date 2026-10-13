// A ZRAW clip opened for random access: frame table, frame rate, geometry, and thread-safe reads.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "movparse.hpp"
#include "zraw.hpp"

class ZrawClip
{
    HANDLE file_ = INVALID_HANDLE_VALUE;
    MovZraw track_;

public:
    std::filesystem::path path;
    int frames = 0;
    double fps = 0;
    int width = 0, height = 0, bits = 0;   // from the first frame's header

    ZrawClip() = default;
    ZrawClip(const ZrawClip&) = delete;
    ZrawClip& operator=(const ZrawClip&) = delete;
    ~ZrawClip() { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }

    bool Open(const std::filesystem::path& p, std::string& err)
    {
        path = p;
        if (!ParseZrawMov(p, track_, err)) return false;
        if (track_.version != 0x12EA78D2) { err = "only true-RAW ZRAW (firmware before 0.94) is supported"; return false; }
        frames = (int)track_.sizes.size();
        fps = track_.fps() > 0 ? track_.fps() : 29.97;
        file_ = CreateFileW(p.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) { err = "cannot open file"; return false; }

        std::vector<uint8_t> data;
        ZrawFrame f;
        if (!frames || !Read(0, data) || !ParseZrawFrame(data.data(), data.size() - 16, f, err)) { if (err.empty()) err = "cannot read the first frame"; return false; }
        width = (int)f.width; height = (int)f.height; bits = f.bits();
        return true;
    }

    // Positional read, safe to call from several threads at once. Adds the 16 zero bytes the decoder needs.
    bool Read(int i, std::vector<uint8_t>& data) const
    {
        if (i < 0 || i >= frames) return false;
        const uint64_t size = track_.sizes[i];
        data.assign((size_t)size + 16, 0);
        OVERLAPPED ov = {};
        ov.Offset = (DWORD)track_.offsets[i];
        ov.OffsetHigh = (DWORD)(track_.offsets[i] >> 32);
        DWORD got = 0;
        return ReadFile(file_, data.data(), (DWORD)size, &got, &ov) && got == size;
    }

    // Reads, parses and decodes frame i into cfa (width * height). `f` keeps pointers into `data`.
    bool Decode(int i, std::vector<uint8_t>& data, ZrawFrame& f, std::vector<uint16_t>& cfa, std::string& err) const
    {
        if (!Read(i, data)) { err = "read failed"; return false; }
        if (!ParseZrawFrame(data.data(), data.size() - 16, f, err)) return false;
        if ((int)f.width != width || (int)f.height != height) { err = "frame size changed mid-clip"; return false; }
        cfa.resize((size_t)f.width * f.height);
        DecodeZrawFrame(f, cfa.data());
        return true;
    }
};

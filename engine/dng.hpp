// Writes one decoded ZRAW frame as a DNG with the same tags zraw-parser 0.11.0-alpha writes
// (ZrawConverterThread::process_dng), so Resolve treats the output identically.
#pragma once
#include <sstream>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cstdio>
#endif
#include "zraw.hpp"
#include "camera_color.hpp"

#define TINY_DNG_WRITER_IMPLEMENTATION
#include <tiny_dng_writer.h>
#include "lj92.hpp"

enum class DngScale { Full, Half, Quarter };

// How the image data is stored.
//   LosslessJpeg  - default, what zraw-parser writes.
//   Uncompressed  - samples packed MSB-first at the clip's bit depth, followed by zero padding up to
//                   16 bits per sample. Byte-identical to zraw-parser (tinydngwriter pads this way).
//   Packed        - the same packed samples without the padding: 25% smaller for 12-bit footage.
enum class DngStorage { LosslessJpeg, Uncompressed, Packed };

// Packs samples MSB-first at `bits` per sample, the TIFF/DNG layout for bit depths below 16.
// Writes ceil(count * bits / 8) bytes and returns that count.
inline size_t PackSamples(const uint16_t* in, size_t count, int bits, uint8_t* out)
{
    uint8_t* w = out;
    uint64_t acc = 0;
    int nbits = 0;
    for (size_t i = 0; i < count; ++i)
    {
        acc = (acc << bits) | in[i];
        nbits += bits;
        while (nbits >= 8) { nbits -= 8; *w++ = (uint8_t)(acc >> nbits); }
    }
    if (nbits) *w++ = (uint8_t)(acc << (8 - nbits));
    return (size_t)(w - out);
}

// tinydngwriter copies the image data several times on the way to disk (every data_os_.str() call
// copies it). Instead, let it lay out the file around a 1-byte stand-in image, then write the real
// data in its place. The image is the last thing in the data area, so only two values depend on its
// length: the IFD offset in the header and the StripByteCounts entry. The result is byte-identical
// to DNGWriter::WriteToFile with the real data.
inline bool WriteDngWithStandIn(const std::string& path, tinydngwriter::DNGImage& img, const uint8_t* data, size_t len, std::string& err,
                                uint64_t* written = nullptr)
{
    const unsigned char standin = 0;
    img.SetImageData(&standin, 1);
    std::ostringstream data_os, ifd_os;
    if (!img.WriteDataToStream(&data_os) || !img.WriteIFDToStream(0, (unsigned)img.GetStripOffset(), &ifd_os)) { err = img.Error(); return false; }
    std::string area = data_os.str(), ifd = ifd_os.str();
    const size_t strip = img.GetStripOffset();
    if (strip + 1 != area.size()) { err = "unexpected DNG layout"; return false; }

    // Patch StripByteCounts (tag 279, one inline LONG) in the IFD: 2-byte count, then 12-byte entries
    uint16_t entries;
    memcpy(&entries, ifd.data(), 2);
    bool patched = false;
    for (size_t e = 0; e < entries; ++e)
    {
        char* entry = &ifd[2 + 12 * e];
        uint16_t tag;
        memcpy(&tag, entry, 2);
        if (tag == 279) { uint32_t v = (uint32_t)len; memcpy(entry + 8, &v, 4); patched = true; }
    }
    if (!patched) { err = "StripByteCounts missing"; return false; }

    uint8_t header[8] = {0x49, 0x49, 0x2A, 0x00};   // little-endian TIFF
    uint32_t ifd_offset = (uint32_t)(8 + strip + len);
    memcpy(header + 4, &ifd_offset, 4);
    const uint32_t next_ifd = 0;

#ifdef _WIN32
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = "cannot create " + path; return false; }
    auto put = [&](const void* p, size_t n) { DWORD done = 0; return n == 0 || (WriteFile(h, p, (DWORD)n, &done, nullptr) && done == n); };
#else
    FILE* h = fopen(path.c_str(), "wb");
    if (!h) { err = "cannot create " + path; return false; }
    auto put = [&](const void* p, size_t n) { return n == 0 || fwrite(p, 1, n, h) == n; };
#endif
    bool ok = put(header, 8) && put(area.data(), strip) && put(data, len) && put(ifd.data(), ifd.size()) && put(&next_ifd, 4);
#ifdef _WIN32
    CloseHandle(h);
#else
    if (fclose(h)) ok = false;
#endif
    if (!ok) err = "write failed: " + path;
    if (ok && written) *written = 8 + strip + len + ifd.size() + 4;   // file size, checked before --delete-originals
    return ok;
}

// RAW -> 1/2 RAW, averaging same-color photosites (original: downscale_raw_twice)
inline std::vector<uint16_t> DownscaleRawTwice(const std::vector<uint16_t>& img, uint32_t w, uint32_t h)
{
    std::vector<uint16_t> r((size_t)w * h / 4);
    for (uint32_t y = 0; y < h; y += 4)
        for (uint32_t x = 0; x < w; x += 4)
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx)
                    r[(size_t)(y / 2 + dy) * (w / 2) + x / 2 + dx] = (uint16_t)(((uint32_t)img[(size_t)(y + dy) * w + x + dx] + img[(size_t)(y + dy) * w + x + dx + 2] +
                                                                                img[(size_t)(y + dy + 2) * w + x + dx] + img[(size_t)(y + dy + 2) * w + x + dx + 2]) / 4);
    return r;
}

// Every tag zraw-parser sets, in the same order (tinydngwriter stores tag payloads in call order).
// `color` (camera_color.hpp) replaces the two fixed matrices with one calibration (ColorMatrix1 plus
// ForwardMatrix1). `exact`: also use zraw-parser's white level, for byte comparisons (zrawdng
// --zraw-parser-exact).
inline void SetDngTags(tinydngwriter::DNGImage& img, const ZrawFrame& f, uint32_t w, uint32_t h, DngStorage storage, const DngColor* color = nullptr,
                       bool exact = false)
{
    img.SetBigEndian(false);
    img.SetSubfileType(false, false, false);
    img.SetImageWidth(w);
    img.SetImageLength(h);
    img.SetRowsPerStrip(h);
    img.SetSamplesPerPixel(1);
    uint16_t bps[1] = {(uint16_t)f.bits()};
    img.SetBitsPerSample(1, bps);
    img.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG);
    img.SetCompression(storage == DngStorage::LosslessJpeg ? tinydngwriter::COMPRESSION_NEW_JPEG : tinydngwriter::COMPRESSION_NONE);
    img.SetPhotometric(tinydngwriter::PHOTOMETRIC_CFA);
    img.SetXResolution(300.0);
    img.SetYResolution(300.0);
    img.SetOrientation(tinydngwriter::ORIENTATION_TOPLEFT);
    img.SetResolutionUnit(tinydngwriter::RESUNIT_NONE);
    img.SetImageDescription("[Storyboard Creativity] ZRAW -> DNG converter generated image.");
    img.SetUniqueCameraModel("Z CAM E2");   // the original hard-codes E2 too; the model could be read from the MOV metadata

    if (color)
    {
        img.SetColorMatrix1(3, color->color_matrix);
        img.SetCalibrationIlluminant1(0);   // the shot's own illuminant, which no standard number names
        img.SetForwardMatrix1(3, color->forward_matrix);
    }
    else
    {
        double cm1[] = {0.9784, -0.4995, 0.003, -0.3625, 1.1454, 0.2475, -0.0961, 0.2097, 0.6377};
        img.SetColorMatrix1(3, cm1);
        img.SetCalibrationIlluminant1(17);
        double cm2[] = {0.6770, -0.1895, -0.0744, -0.5232, 1.3145, 0.2303, -0.1664, 0.2691, 0.5703};
        img.SetColorMatrix2(3, cm2);
        img.SetCalibrationIlluminant2(21);
    }

    double analog[] = {1.0, 1.0, 1.0};
    img.SetAnalogBalance(3, analog);
    double r = f.awb_r, g = f.awb_g, b = f.awb_b;
    double neutral[3] = {g / r, 1.0, g / b};   // same as the original, including a 0 gain giving inf
    img.SetAsShotNeutral(3, neutral);

    img.SetBlackLevelRepeatDim(2, 2);
    img.SetBlackLevel(4, f.black);
    img.SetDNGVersion(1, 2, 0, 0);
    img.SetCFARepeatPatternDim(2, 2);
    uint8_t pattern[4] = {0, 1, 1, 2};
    img.SetCFAPattern(4, pattern);
    if (exact)   // zraw-parser's tag, byte for byte: full scale, stored as a RATIONAL
    {
        double white[1] = {(double)((1 << f.bits()) - 1)};
        img.SetWhiteLevelRational(1, white);
    }
    else   // where the sensor really clips: 4094 on the E2 (clipped photosites read 4094, a few 4095), as a
           // standard LONG. At 4095, clipped photosites sit just under white and Resolve's highlight recovery
           // never treats them as clipped.
        img.SetCustomFieldULong(50717, (1u << f.bits()) - 2);
}

inline bool WriteDng(const std::string& path, const ZrawFrame& f, std::vector<uint16_t>& cfa, DngStorage storage, DngScale scale, std::string& err,
                     uint64_t* written = nullptr, const DngColor* color = nullptr, bool exact = false)
{
    uint32_t w = f.width, h = f.height;
    for (int i = 0; i < (scale == DngScale::Half ? 1 : scale == DngScale::Quarter ? 2 : 0); ++i)
    {
        cfa = DownscaleRawTwice(cfa, w, h);
        w /= 2; h /= 2;
    }

    tinydngwriter::DNGImage img;
    SetDngTags(img, f, w, h, storage, color, exact);

    if (storage != DngStorage::LosslessJpeg)
    {
        // Packed samples; Uncompressed keeps zraw-parser's zero padding so its files stay identical
        thread_local std::vector<uint8_t> buf;
        buf.resize(cfa.size() * 2);
        size_t len = PackSamples(cfa.data(), cfa.size(), f.bits(), buf.data());
        if (storage == DngStorage::Uncompressed) { memset(buf.data() + len, 0, buf.size() - len); len = buf.size(); }
        return WriteDngWithStandIn(path, img, buf.data(), len, err, written);
    }

    // Same layout trick as SetImageDataJpeg: each pair of CFA rows becomes one row twice as wide
    const uint8_t* jpeg;
    size_t jpeg_len;
    if (!FastLj92Encode(cfa.data(), (int)w * 2, (int)h / 2, f.bits(), jpeg, jpeg_len)) { err = "pixel value out of range"; return false; }
    return WriteDngWithStandIn(path, img, jpeg, jpeg_len, err, written);
}

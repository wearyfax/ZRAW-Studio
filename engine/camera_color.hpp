// DNG color tags from a single color matrix: ZRAW Studio's own fitted one (the default), ZRAW
// VideoSuite's, or the camera's own calibration (zrawdng convert --camera-color, described below).
//
// Every true-RAW frame has a 'ccm ' block: a big-endian count, then that many 40-byte records
// encrypted with AES-128-CBC (key "jisofsdguhfusxc", zero IV; the same key zraw-decoder-lib uses).
// Each record is a little-endian uint16 color temperature, 2 spare bytes, and nine int32 values in
// thousandths: a 3x3 matrix from white-balanced camera RGB to linear Rec.709, rows summing to 1000
// (HiSilicon's high / mid / low color-temperature CCMs, 7306 K / 5000 K / 2760 K on the E2). The
// first record is the shot's: its temperature is the camera's estimate (2900 to 6300 K on the clips
// examined). Its matrix is usually the identity, but two daylight clips carried a real one there,
// so the code relies only on its position. The camera's matrix for the frame blends the calibrated
// records at that temperature, the way the camera's processor does.
//
// On a ColorChecker Video frame (zraw-parser's checker.raw, LED light, white balanced on the gray
// bar), the camera's matrices put the six saturated chips and their 50% versions on average 3.7
// degrees from the Rec.709 vectorscope targets, against 6.1 for zraw-parser's fixed ColorMatrix1/2
// (green -4.9 against -12.6). The skin chips land about 3 degrees closer to the skin line.
//
// In the DNG: ForwardMatrix1 = Bradford(D65->D50) * Rec.709-to-XYZ * CCM, which is how DNG wants a
// white-balanced camera-to-XYZ(D50) matrix; ColorMatrix1 is its consistent XYZ-to-camera partner for
// the shot's illuminant and white balance, so a raw processor reads the white as the camera's
// temperature. Only one calibration is written (the camera stores the white balance of the shot, not
// of its calibration illuminants, so a second ColorMatrix can't be derived); a large white balance
// change in the grade keeps the shot's matrix.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#include "zraw.hpp"

struct DngColor
{
    double color_matrix[9];     // ColorMatrix1, XYZ (shot illuminant) -> camera
    double forward_matrix[9];   // ForwardMatrix1, white-balanced camera -> XYZ (D50)
    int cct = 0;                // the camera's color temperature for the frame
};

namespace camera_color_detail
{
    using M3 = std::array<double, 9>;
    inline M3 mul(const M3& a, const M3& b)
    {
        M3 r{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k) r[3 * i + j] += a[3 * i + k] * b[3 * k + j];
        return r;
    }
    inline bool inv(const M3& m, M3& r)
    {
        double d = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
        if (std::fabs(d) < 1e-12) return false;
        r = {(m[4] * m[8] - m[5] * m[7]) / d, (m[2] * m[7] - m[1] * m[8]) / d, (m[1] * m[5] - m[2] * m[4]) / d,
             (m[5] * m[6] - m[3] * m[8]) / d, (m[0] * m[8] - m[2] * m[6]) / d, (m[2] * m[3] - m[0] * m[5]) / d,
             (m[3] * m[7] - m[4] * m[6]) / d, (m[1] * m[6] - m[0] * m[7]) / d, (m[0] * m[4] - m[1] * m[3]) / d};
        return true;
    }
    inline M3 diag(double a, double b, double c) { return {a, 0, 0, 0, b, 0, 0, 0, c}; }

    const M3 kRec709ToXYZ = {0.4124564, 0.3575761, 0.1804375, 0.2126729, 0.7151522, 0.0721750, 0.0193339, 0.1191920, 0.9503041};
    const M3 kBradford = {0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296};

    inline void white(double x, double y, double out[3]) { out[0] = x / y; out[1] = 1; out[2] = (1 - x - y) / y; }

    // Bradford chromatic adaptation from white s to white d (XYZ)
    inline M3 adapt(const double s[3], const double d[3])
    {
        M3 bi;
        inv(kBradford, bi);
        double cs[3], cd[3];
        for (int i = 0; i < 3; ++i)
        {
            cs[i] = kBradford[3 * i] * s[0] + kBradford[3 * i + 1] * s[1] + kBradford[3 * i + 2] * s[2];
            cd[i] = kBradford[3 * i] * d[0] + kBradford[3 * i + 1] * d[1] + kBradford[3 * i + 2] * d[2];
        }
        return mul(bi, mul(diag(cd[0] / cs[0], cd[1] / cs[1], cd[2] / cs[2]), kBradford));
    }

    // Chromaticity of a color temperature: Planckian locus below 4000 K (Kim et al.), CIE daylight above
    inline void cct_xy(double T, double& x, double& y)
    {
        T = std::min(25000.0, std::max(1667.0, T));
        if (T < 4000)
        {
            x = -0.2661239e9 / (T * T * T) - 0.2343589e6 / (T * T) + 0.8776956e3 / T + 0.179910;
            y = T < 2222 ? -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x - 0.20219683
                         : -0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - 0.16748867;
        }
        else
        {
            x = T <= 7000 ? -4.6070e9 / (T * T * T) + 2.9678e6 / (T * T) + 0.09911e3 / T + 0.244063
                          : -2.0064e9 / (T * T * T) + 1.9018e6 / (T * T) + 0.24748e3 / T + 0.237040;
            y = -3.000 * x * x + 2.870 * x - 0.275;
        }
    }

    inline bool decrypt(const uint8_t* in, size_t n, uint8_t* out)
    {
        thread_local BCRYPT_ALG_HANDLE alg = nullptr;
        thread_local BCRYPT_KEY_HANDLE key = nullptr;
        if (!key)
        {
            if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return false;
            if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0) != 0) return false;
            static const char kKey[16] = "jisofsdguhfusxc";   // 15 characters and the terminating zero
            if (BCryptGenerateSymmetricKey(alg, &key, nullptr, 0, (PUCHAR)kKey, 16, 0) != 0) { key = nullptr; return false; }
        }
        UCHAR iv[16] = {};
        ULONG got = 0;
        return BCryptDecrypt(key, (PUCHAR)in, (ULONG)n, nullptr, iv, 16, out, (ULONG)n, &got, 0) == 0 && got == n;
    }
}

// Which matrices a conversion writes:
//   Ours        ZRAW Studio's own (the default).
//   Fixed       the pair zraw-parser writes, which its author took from the DNG writer in Z CAM's E2 /
//               E2-M4 firmware (factory matrices, no per-camera calibration). Selected by
//               --zcam-dng-color (alias --zraw-parser-color), and always with --zraw-parser-exact.
//   Camera      the camera's own per-frame matrices (--camera-color).
//   VideoSuite  ZRAW VideoSuite's (--videosuite-color).
enum class ColorSource { Ours, Fixed, Camera, VideoSuite };

namespace camera_color_detail
{
    // The color matrix Z CAM's ZRAW VideoSuite uses (the same for every clip), white-balanced
    // camera RGB -> linear Rec.709. Its rows sum to 0.913 / 0.926 / 0.959, which grays the picture 0.13
    // stops and tints it blue; each row is scaled to sum to 1 so white stays white, as DNG requires of
    // a ForwardMatrix.
    const M3 kVideoSuite = {1.462, -0.434, -0.115, -0.159, 1.33, -0.245, 0.061, -0.444, 1.342};

    // ZRAW Studio's own matrices, white-balanced camera RGB -> linear Rec.709, rows summing to 1,
    // blended linearly in mired by the camera's own color temperature for the shot (the 'ccm ' first
    // record), clamped at the ends. Fitted from an i1Pro 2 scan of a SpyderCheckr 24 times the
    // measured spectrum of each light, against ZRAW clips under a household tungsten bulb (2566 K
    // measured, the camera reads 2900 K), a tube LED (4733 K, camera 3900 K), outdoor fluorescents
    // (5859 K, camera 4700 K), low sun an hour before sunset (4109 K, camera 4900 K, glare on the card
    // removed) and shade (6714 K, camera 6300 K). The anchors are the camera's readings of the tungsten
    // and the shade. Held out of the fit, the sun scores 2.05 CIEDE2000 and the LED 1.99; the
    // fluorescent is the weak spot (3.4), since it and the sun read alike on the camera.
    const int kOursLowK = 2900, kOursHighK = 6300;
    const M3 kOursLow = {1.8120, -0.7214, -0.0905, -0.3849, 1.5868, -0.2019, 0.1269, -0.9230, 1.7961};
    const M3 kOursHigh = {1.6108, -0.5139, -0.0969, -0.2326, 1.5937, -0.3610, 0.0189, -0.4122, 1.3933};

    // DNG tags for a white-balanced camera -> linear Rec.709 matrix at the shot's temperature
    inline bool ToDng(const M3& ccm, int now, const ZrawFrame& f, DngColor& out)
    {
        double d65[3], d50[3], shot[3], x, y;
        white(0.31271, 0.32902, d65);
        white(0.34567, 0.35850, d50);
        cct_xy(now, x, y);
        white(x, y, shot);

        const M3 fm = mul(adapt(d65, d50), mul(kRec709ToXYZ, ccm));
        const M3 gains = diag((double)f.awb_r / f.awb_g, 1.0, (double)f.awb_b / f.awb_g);
        M3 cam_to_xyz = mul(adapt(d65, shot), mul(kRec709ToXYZ, mul(ccm, gains))), cm;
        if (!inv(cam_to_xyz, cm)) return false;
        double neutral[3];   // the shot's white in camera RGB; scale so its largest channel is 1, as DNG expects
        for (int i = 0; i < 3; ++i) neutral[i] = cm[3 * i] * shot[0] + cm[3 * i + 1] * shot[1] + cm[3 * i + 2] * shot[2];
        const double s = 1.0 / std::max(neutral[0], std::max(neutral[1], neutral[2]));
        for (int k = 0; k < 9; ++k) { out.color_matrix[k] = cm[k] * s; out.forward_matrix[k] = fm[k]; }
        out.cct = now;
        return true;
    }
}

// The camera's matrix for one frame (white-balanced camera RGB -> linear Rec.709) and the shot's
// temperature. With `fixed`, that matrix replaces it (rows scaled to sum to 1); with `ours`, ZRAW
// Studio's own pair, blended at the shot's temperature, replaces it. Either way the 'ccm ' block still
// supplies the temperature. False when the frame has no usable 'ccm ' block.
inline bool FrameMatrix(const ZrawFrame& f, camera_color_detail::M3& ccm, int& now, const camera_color_detail::M3* fixed = nullptr, bool ours = false)
{
    using namespace camera_color_detail;
    if (!f.ccm || f.ccm_size < 4 || !f.awb_r || !f.awb_g || !f.awb_b) return false;
    const uint32_t count = (uint32_t)f.ccm[0] << 24 | (uint32_t)f.ccm[1] << 16 | (uint32_t)f.ccm[2] << 8 | f.ccm[3];
    const size_t n = (f.ccm_size - 4) / 16 * 16;
    if (count == 0 || count > 10 || n < 40 * (size_t)count) return false;
    std::vector<uint8_t> d(n);
    if (!decrypt(f.ccm + 4, n, d.data())) return false;

    struct Cal { int cct; M3 m; };
    std::vector<Cal> cal;
    now = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint8_t* r = d.data() + 40 * i;
        int cct = r[0] | r[1] << 8;
        M3 m;
        for (int k = 0; k < 9; ++k)
        {
            int32_t v;
            memcpy(&v, r + 4 + 4 * k, 4);
            m[k] = v / 1000.0;
        }
        if (cct < 1000 || cct > 20000) return false;   // implausible temperature: leave the colors alone
        // The first record is the shot's; its matrix is usually the identity but not always (two daylight
        // clips carried a real one), so the code identifies it by position, not by its matrix.
        if (i == 0) now = cct; else cal.push_back({cct, m});
    }
    if (cal.empty() || !now) return false;
    std::sort(cal.begin(), cal.end(), [](const Cal& a, const Cal& b) { return a.cct < b.cct; });

    // Blend the two calibrations around the shot's temperature, linearly in mired
    ccm = cal.front().m;
    if (now >= cal.back().cct) ccm = cal.back().m;
    else
        for (size_t i = 0; i + 1 < cal.size(); ++i)
            if (now >= cal[i].cct && now <= cal[i + 1].cct)
            {
                double w = (1.0 / now - 1.0 / cal[i + 1].cct) / (1.0 / cal[i].cct - 1.0 / cal[i + 1].cct);
                for (int k = 0; k < 9; ++k) ccm[k] = w * cal[i].m[k] + (1 - w) * cal[i + 1].m[k];
            }
    if (fixed)
        for (int i = 0; i < 3; ++i)
        {
            const M3& m = *fixed;
            const double sum = m[3 * i] + m[3 * i + 1] + m[3 * i + 2];
            for (int k = 0; k < 3; ++k) ccm[3 * i + k] = m[3 * i + k] / sum;
        }
    if (ours)
    {
        const double w = std::min(1.0, std::max(0.0, (1.0 / now - 1.0 / kOursHighK) / (1.0 / kOursLowK - 1.0 / kOursHighK)));
        for (int k = 0; k < 9; ++k) ccm[k] = w * kOursLow[k] + (1 - w) * kOursHigh[k];
    }
    return true;
}

// FrameMatrix's result as DNG tags
inline bool CameraDngColor(const ZrawFrame& f, DngColor& out, const camera_color_detail::M3* fixed = nullptr, bool ours = false)
{
    camera_color_detail::M3 ccm;
    int now;
    return FrameMatrix(f, ccm, now, fixed, ours) && camera_color_detail::ToDng(ccm, now, f, out);
}

// The DNG color tags for one frame. False for Fixed, or when the frame has no usable 'ccm ' block
// (the caller then writes zraw-parser's fixed matrices).
inline bool FrameDngColor(const ZrawFrame& f, ColorSource source, DngColor& out)
{
    using namespace camera_color_detail;
    switch (source)
    {
        case ColorSource::Ours: return CameraDngColor(f, out, nullptr, true);
        case ColorSource::Camera: return CameraDngColor(f, out);
        case ColorSource::VideoSuite: return CameraDngColor(f, out, &kVideoSuite);
        default: return false;
    }
}

// The matrix the previews use for `source` (white-balanced camera RGB -> linear Rec.709, which shares
// sRGB's primaries). False for Fixed, or when the frame has no usable 'ccm ' block.
inline bool PreviewSourceMatrix(const ZrawFrame& f, ColorSource source, double out[9])
{
    using namespace camera_color_detail;
    M3 ccm;
    int now;
    bool ok = source == ColorSource::Ours ? FrameMatrix(f, ccm, now, nullptr, true)
            : source == ColorSource::Camera ? FrameMatrix(f, ccm, now)
            : source == ColorSource::VideoSuite ? FrameMatrix(f, ccm, now, &kVideoSuite)
            : false;
    if (ok) for (int k = 0; k < 9; ++k) out[k] = ccm[k];
    return ok;
}

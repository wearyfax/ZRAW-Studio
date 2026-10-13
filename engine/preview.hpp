// Half-resolution viewing image from a decoded ZRAW frame: one RGB pixel per 2x2 Bayer cell.
// Applies black level, the frame's own white balance, a camera-to-sRGB matrix (the one chosen in
// PreviewSettings::matrix, the same choices the DNGs can carry), then the sRGB curve. For browsing
// clips, not grading.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "zraw.hpp"
#include "camera_color.hpp"

// What the viewer can change. Defaults show the clip as the camera saw it.
struct PreviewSettings
{
    double exposure = 0;     // stops, applied in linear light before the curve
    double wb_red = 1;       // multipliers on top of the frame's own white balance
    double wb_blue = 1;
    bool full_res = true;    // 4K debayered on the GPU in mpv; false = half resolution rendered on the CPU
    int matrix = 0;          // ColorSource order: 0 ZRAW Studio (our calibration), 1 ZCam-DNG (zraw-parser's
                             // fixed one), 2 the camera's own (Camera built-in matrix), 3 VideoSuite

    bool operator==(const PreviewSettings& o) const { return exposure == o.exposure && wb_red == o.wb_red && wb_blue == o.wb_blue && full_res == o.full_res && matrix == o.matrix; }
};

// Names used in the mpv plugin's zraw.conf, in PreviewSettings::matrix order
inline const char* const kPreviewMatrixNames[] = {"zrawstudio", "zcamdng", "camera", "videosuite"};

// zraw-parser's ColorMatrix2 (XYZ -> camera, D65) turned into white-balanced camera -> linear sRGB,
// the way dcraw does it: camera <- sRGB, rows normalized so white maps to white, then inverted.
inline void FixedPreviewMatrix(float out[9])
{
    const double cam_xyz[3][3] = {{0.6770, -0.1895, -0.0744}, {-0.5232, 1.3145, 0.2303}, {-0.1664, 0.2691, 0.5703}};
    const double xyz_rgb[3][3] = {{0.412453, 0.357580, 0.180423}, {0.212671, 0.715160, 0.072169}, {0.019334, 0.119193, 0.950227}};
    double a[3][3];
    for (int r = 0; r < 3; ++r)
    {
        double sum = 0;
        for (int c = 0; c < 3; ++c)
        {
            a[r][c] = 0;
            for (int k = 0; k < 3; ++k) a[r][c] += cam_xyz[r][k] * xyz_rgb[k][c];
            sum += a[r][c];
        }
        for (int c = 0; c < 3; ++c) a[r][c] /= sum;
    }
    const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
        {
            int r1 = (c + 1) % 3, r2 = (c + 2) % 3, c1 = (r + 1) % 3, c2 = (r + 2) % 3;
            out[3 * r + c] = (float)((a[r1][c1] * a[r2][c2] - a[r1][c2] * a[r2][c1]) / det);
        }
}

// The preview matrix for `source` from frame `f` (its 'ccm ' block gives the shot's temperature);
// the fixed one when the frame has none.
inline void PreviewMatrix(const ZrawFrame& f, int source, float out[9])
{
    double m[9];
    if (source < 0 || source > 3 || !PreviewSourceMatrix(f, (ColorSource)source, m)) { FixedPreviewMatrix(out); return; }
    for (int k = 0; k < 9; ++k) out[k] = (float)m[k];
}

class PreviewLook
{
    PreviewSettings settings_;
    float cam_to_srgb[3][3];
    uint8_t curve[4096];   // linear 0..1 in 4096 steps -> sRGB 8-bit

public:
    // matrix: from PreviewMatrix; null = the fixed one
    explicit PreviewLook(const PreviewSettings& s = {}, const float* matrix = nullptr) : settings_(s)
    {
        float m[9];
        if (matrix) memcpy(m, matrix, sizeof(m)); else FixedPreviewMatrix(m);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) cam_to_srgb[r][c] = m[3 * r + c];

        const double gain = std::pow(2.0, s.exposure);
        for (int i = 0; i < 4096; ++i)
        {
            double v = std::fmin(1.0, i / 4095.0 * gain);
            v = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
            curve[i] = (uint8_t)std::lround(v * 255);
        }
    }

    const PreviewSettings& Settings() const { return settings_; }

    // cfa: width x height from DecodeZrawFrame (R G / G B cells). out: (width/2) x (height/2) rgb24.
    void Render(const ZrawFrame& f, const uint16_t* cfa, uint8_t* out) const
    {
        const int W = (int)f.width, H = (int)f.height;
        const float white = (float)((1 << f.bits()) - 1);
        float blk[4], scale[4];
        for (int i = 0; i < 4; ++i) { blk[i] = f.black[i]; scale[i] = 1.0f / (white - blk[i]); }
        // Frame's white balance: AsShotNeutral is (g/r, 1, g/b), so these gains neutralize it
        const float wb_r = (f.awb_g ? (float)f.awb_r / f.awb_g : 1.0f) * (float)settings_.wb_red;
        const float wb_b = (f.awb_g ? (float)f.awb_b / f.awb_g : 1.0f) * (float)settings_.wb_blue;
        // After white balance each channel saturates at its own gain (red near 2.2, green 1). Where the
        // sensor clipped, red and blue would then sit far above green and turn purple, so all channels
        // are capped where the first one saturates: clipped areas come out white.
        const float ceiling = std::fmin(1.0f, std::fmin(wb_r, wb_b));

        for (int y = 0; y < H; y += 2)
        {
            const uint16_t* a = cfa + (size_t)y * W;
            const uint16_t* b = a + W;
            uint8_t* o = out + (size_t)(y / 2) * (W / 2) * 3;
            for (int x = 0; x < W; x += 2, o += 3)
            {
                float cr = std::fmin(ceiling, (a[x] - blk[0]) * scale[0] * wb_r);
                float cg = std::fmin(ceiling, ((a[x + 1] - blk[1]) * scale[1] + (b[x] - blk[2]) * scale[2]) * 0.5f);
                float cb = std::fmin(ceiling, (b[x + 1] - blk[3]) * scale[3] * wb_b);
                for (int c = 0; c < 3; ++c)
                {
                    float v = cam_to_srgb[c][0] * cr + cam_to_srgb[c][1] * cg + cam_to_srgb[c][2] * cb;
                    int i = (int)(v * 4095.0f);
                    o[c] = curve[i < 0 ? 0 : i > 4095 ? 4095 : i];
                }
            }
        }
    }
};

// Full-resolution preview: the decoded Bayer frame goes to mpv untouched and a custom mpv shader
// debayers it on the GPU.
//
// Frame layout handed to mpv (rgba64, 16 bits per channel): width/4 x height texels. Each texel holds
// four consecutive photosites of a row, the layout DecodeZrawFrame already produces, so packing is
// one memcpy. mpv is told the picture is width x height (video-aspect-override), so it shows the
// shader's output pixel for pixel.
//
// Each frame's metadata rides in the unused top 4 bits of the first 32 photosites of row 0 (samples
// are at most 12 bits), as eight 16-bit words, least significant nibble first:
//   0..2 = AWB gains r, g, b   3 = bit depth   4..7 = black level per 2x2 CFA position (row-major)
// 14-bit footage is shifted down to 12 bits on the way (a picture on screen doesn't need the last two).
//
// The shader rebuilds RGB at full resolution with Malvar-He-Cutler (a 5x5 linear demosaic that keeps
// fine detail without zipper edges), then applies the same look as the CPU preview: white balance,
// camera-to-sRGB matrix, exposure, sRGB curve. The matrix, exposure and white-balance trims are shader
// parameters, so changing them needs no re-decoding.
#pragma once
#include <cstdio>
#include <cstring>
#include <string>
#include "preview.hpp"

inline bool FullResSupported(int bits) { return bits <= 14; }

inline void PackFullResFrame(const ZrawFrame& f, const uint16_t* cfa, uint8_t* out)
{
    const size_t n = (size_t)f.width * f.height;
    const int down = f.bits() > 12 ? f.bits() - 12 : 0;
    if (down) { uint16_t* o = (uint16_t*)out; for (size_t i = 0; i < n; ++i) o[i] = (uint16_t)(cfa[i] >> down); }
    else memcpy(out, cfa, n * 2);
    auto clamp16 = [](uint32_t v) { return (uint16_t)(v > 65535 ? 65535 : v); };
    const uint16_t meta[8] = {clamp16(f.awb_r), clamp16(f.awb_g), clamp16(f.awb_b), (uint16_t)(f.bits() - down),
                              (uint16_t)(f.black[0] >> down), (uint16_t)(f.black[1] >> down), (uint16_t)(f.black[2] >> down), (uint16_t)(f.black[3] >> down)};
    uint16_t* row0 = (uint16_t*)out;
    for (int i = 0; i < 32; ++i) row0[i] = (uint16_t)((row0[i] & 0x0FFF) | (((meta[i / 4] >> (4 * (i % 4))) & 0xF) << 12));
}

// The camera -> linear sRGB matrix is nine parameters too (zm0..zm8, row-major), set per clip from
// the chosen color source; their defaults are the fixed ZCam-DNG matrix.
inline std::string DebayerShader()
{
    float fixed[9];
    FixedPreviewMatrix(fixed);
    std::string params;
    for (int k = 0; k < 9; ++k)
    {
        char p[160];
        snprintf(p, sizeof(p), "//!PARAM zm%d\n//!DESC Camera to linear sRGB matrix, row %d column %d\n//!TYPE float\n%.7f\n\n", k, k / 3, k % 3, fixed[k]);
        params += p;
    }
    return params + std::string(R"(//!PARAM exposure
//!DESC Exposure in stops
//!TYPE float
0.0

//!PARAM wb_red
//!DESC Red gain on top of the camera white balance
//!TYPE float
1.0

//!PARAM wb_blue
//!DESC Blue gain on top of the camera white balance
//!TYPE float
1.0

//!HOOK RGB
//!BIND HOOKED
//!WIDTH HOOKED.w 4 *
//!HEIGHT HOOKED.h
//!DESC ZRAW debayer (Malvar-He-Cutler) and preview look

int zW, zH;
float zblack[4], zgain[4], zceiling;

float zraw16(ivec2 p)
{
    vec4 t = texelFetch(HOOKED_raw, ivec2(p.x >> 2, p.y), 0);
    return floor(t[p.x & 3] * HOOKED_mul * 65535.0 + 0.5);
}

// Metadata word i: the top nibbles of photosites 4i..4i+3 of row 0
float zword(int i)
{
    float w = 0.0;
    for (int k = 3; k >= 0; --k) w = w * 16.0 + floor(zraw16(ivec2(4 * i + k, 0)) / 4096.0);
    return w;
}

// White-balanced, zblack-subtracted photosite value, 0..1 for a white sample
float zs(ivec2 p)
{
    p = clamp(p, ivec2(0), ivec2(zW - 1, zH - 1));
    int c = (p.y & 1) * 2 + (p.x & 1);
    return min((mod(zraw16(p), 4096.0) - zblack[c]) * zgain[c], zceiling);
}

vec4 hook()
{
    zW = int(HOOKED_size.x) * 4;
    zH = int(HOOKED_size.y);
    ivec2 o = ivec2(floor(HOOKED_pos * vec2(zW, zH)));

    float white = exp2(zword(3)) - 1.0;
    float wb_r = zword(0) / max(zword(1), 1.0) * wb_red;
    float wb_b = zword(2) / max(zword(1), 1.0) * wb_blue;
    float cfa_wb[4] = float[4](wb_r, 1.0, 1.0, wb_b);   // R G / G B
    // Each channel saturates at its own white-balance gain; cap all of them where the first one
    // saturates, so sensor-clipped highlights come out white instead of purple
    zceiling = min(1.0, min(wb_r, wb_b));
    for (int i = 0; i < 4; ++i)
    {
        zblack[i] = zword(4 + i);
        zgain[i] = cfa_wb[i] / (white - zblack[i]);
    }

    float c = zs(o);
    float cross1 = zs(o + ivec2(0, -1)) + zs(o + ivec2(0, 1)) + zs(o + ivec2(-1, 0)) + zs(o + ivec2(1, 0));
    float cross2 = zs(o + ivec2(0, -2)) + zs(o + ivec2(0, 2)) + zs(o + ivec2(-2, 0)) + zs(o + ivec2(2, 0));
    float diag = zs(o + ivec2(-1, -1)) + zs(o + ivec2(1, -1)) + zs(o + ivec2(-1, 1)) + zs(o + ivec2(1, 1));
    float horiz1 = zs(o + ivec2(-1, 0)) + zs(o + ivec2(1, 0));
    float vert1 = zs(o + ivec2(0, -1)) + zs(o + ivec2(0, 1));
    float horiz2 = zs(o + ivec2(-2, 0)) + zs(o + ivec2(2, 0));
    float vert2 = zs(o + ivec2(0, -2)) + zs(o + ivec2(0, 2));

    // Malvar-He-Cutler, coefficients over 8
    float g_at_rb = (4.0 * c + 2.0 * cross1 - cross2) / 8.0;
    float rb_at_br = (6.0 * c + 2.0 * diag - 1.5 * cross2) / 8.0;                     // B at R, R at B
    float rb_at_g_h = (5.0 * c + 4.0 * horiz1 - horiz2 - diag + 0.5 * vert2) / 8.0;   // color left/right of a G
    float rb_at_g_v = (5.0 * c + 4.0 * vert1 - vert2 - diag + 0.5 * horiz2) / 8.0;    // color above/below a G

    vec3 cam;
    bool odd_x = (o.x & 1) == 1, odd_y = (o.y & 1) == 1;
    if (!odd_x && !odd_y)     cam = vec3(c, g_at_rb, rb_at_br);       // R site
    else if (odd_x && odd_y)  cam = vec3(rb_at_br, g_at_rb, c);       // B site
    else if (odd_x)           cam = vec3(rb_at_g_h, c, rb_at_g_v);    // G in a red row
    else                      cam = vec3(rb_at_g_v, c, rb_at_g_h);    // G in a blue row

    cam = max(cam, 0.0);
    vec3 rgb = vec3(dot(vec3(zm0, zm1, zm2), cam), dot(vec3(zm3, zm4, zm5), cam), dot(vec3(zm6, zm7, zm8), cam)) * exp2(exposure);
    rgb = clamp(rgb, 0.0, 1.0);
    rgb = mix(12.92 * rgb, 1.055 * pow(rgb, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, rgb));
    return vec4(rgb, 1.0);
}
)");
}

// zrawcore.dll: the C API in zrawcore.h over clip.hpp, preview.hpp and player.hpp.
#define ZRAWCORE_BUILD
#include "zrawcore.h"
#include "player.hpp"
#include "hevc_frame.hpp"

static thread_local std::string g_error;

static int fail(const std::string& e) { g_error = e; return -1; }

// Every export is a function-try-block ending in this: an exception from the parsers (bad_alloc on a damaged
// file, say) must not unwind into P/Invoke or the caller's C code, so it becomes an error for zr_last_error
static int fail_exception()
{
    try { throw; }
    catch (const std::exception& e) { return fail(std::string("internal error: ") + e.what()); }
    catch (...) { return fail("internal error"); }
}

static PreviewSettings to_settings(const zr_preview_settings* s)
{
    PreviewSettings r;
    if (s) { r.exposure = s->exposure; r.wb_red = s->wb_red; r.wb_blue = s->wb_blue; r.full_res = s->full_res != 0; r.matrix = s->matrix; }
    return r;
}

extern "C" {

ZRAWCORE_API const char* zr_last_error(void) { return g_error.c_str(); }

static bool is_hevc(const wchar_t* path)
{
    MovZraw z;
    std::string err;
    return ParseZrawMov(path, z, err) && z.version == 0x45A32DEF;
}

ZRAWCORE_API int zr_clip_info_get(const wchar_t* path, zr_clip_info* out)
try
{
    if (!path || !out) return fail("null argument");
    std::string err;
    if (is_hevc(path))
    {
        HevcZraw z;
        if (!z.Open(path, err)) return fail(err);
        *out = {z.frames, z.fps, z.width, z.height, 10};
        return 0;
    }
    ZrawClip clip;
    if (!clip.Open(path, err)) return fail(err);
    *out = {clip.frames, clip.fps, clip.width, clip.height, clip.bits};
    return 0;
}
catch (...) { return fail_exception(); }

ZRAWCORE_API int zr_clip_data_bits(const wchar_t* path, int32_t* out)
try
{
    if (!path || !out) return fail("null argument");
    if (is_hevc(path)) { *out = 10; return 0; }
    std::string err;
    ZrawClip clip;
    std::vector<uint8_t> data;
    ZrawFrame f;
    std::vector<uint16_t> cfa;
    if (!clip.Open(path, err) || !clip.Decode(0, data, f, cfa, err)) return fail(err);
    uint32_t any = 0;
    for (uint16_t v : cfa) any |= v;
    unsigned long low = 0;
    *out = any && _BitScanForward(&low, any) ? clip.bits - (int)low : clip.bits;
    return 0;
}
catch (...) { return fail_exception(); }

ZRAWCORE_API int zr_render_frame(const wchar_t* path, int32_t frame, const zr_preview_settings* settings, uint8_t* rgb, int64_t rgb_size)
try
{
    if (!path || !rgb) return fail("null argument");
    std::string err;
    if (is_hevc(path))
    {
        // Already developed in the camera, so the preview settings don't apply
        HevcZraw z;
        if (!z.Open(path, err)) return fail(err);
        if (rgb_size < (int64_t)(z.width / 2) * (z.height / 2) * 3) return fail("buffer too small");
        return RenderHevcZrawFrame(z, frame, z.width / 2, z.height / 2, rgb, err) ? 0 : fail(err);
    }
    ZrawClip clip;
    if (!clip.Open(path, err)) return fail(err);
    if (frame < 0 || frame >= clip.frames) return fail("frame out of range");
    if (rgb_size < (int64_t)(clip.width / 2) * (clip.height / 2) * 3) return fail("buffer too small");
    std::vector<uint8_t> data;
    std::vector<uint16_t> cfa;
    ZrawFrame f;
    if (!clip.Decode(frame, data, f, cfa, err)) return fail(err);
    const PreviewSettings s = to_settings(settings);
    float m[9];
    PreviewMatrix(f, s.matrix, m);   // this frame's temperature; the players use the clip's first frame
    PreviewLook(s, m).Render(f, cfa.data(), rgb);
    return 0;
}
catch (...) { return fail_exception(); }

struct zr_player { ZrawPlayer impl; };

ZRAWCORE_API zr_player* zr_player_create(void* hwnd)
try
{
    auto p = new zr_player;
    std::string err;
    if (!p->impl.Create((HWND)hwnd, err)) { delete p; fail(err); return nullptr; }
    return p;
}
catch (...) { return (fail_exception(), nullptr); }

ZRAWCORE_API int zr_player_open(zr_player* p, const wchar_t* path)
try
{
    if (!p || !path) return fail("null argument");
    std::string err;
    return p->impl.Open(path, err) ? 0 : fail(err);
}
catch (...) { return fail_exception(); }

ZRAWCORE_API int zr_player_seek_frame(zr_player* p, int32_t frame)
try
{
    if (!p) return fail("null argument");
    return p->impl.SeekFrame(frame) < 0 ? fail("seek failed (is a clip open?)") : 0;
}
catch (...) { return fail_exception(); }

ZRAWCORE_API int32_t zr_player_get_frame(zr_player* p) try { return p ? p->impl.CurrentFrame() : -1; } catch (...) { return fail_exception(); }

ZRAWCORE_API int zr_player_set_preview(zr_player* p, const zr_preview_settings* s)
try
{
    if (!p || !s) return fail("null argument");
    p->impl.SetSettings(to_settings(s));
    return 0;
}
catch (...) { return fail_exception(); }

ZRAWCORE_API int zr_player_get_preview(zr_player* p, zr_preview_settings* out)
try
{
    if (!p || !out) return fail("null argument");
    PreviewSettings s = p->impl.Settings();
    *out = {s.exposure, s.wb_red, s.wb_blue, s.full_res ? 1 : 0, s.matrix};
    return 0;
}
catch (...) { return fail_exception(); }

ZRAWCORE_API int zr_player_command(zr_player* p, const char* command)
try
{
    if (!p || !command) return fail("null argument");
    int r = p->impl.Command(command);
    return r < 0 ? fail(std::string("mpv command failed: ") + command) : 0;
}
catch (...) { return fail_exception(); }

ZRAWCORE_API int zr_player_set_property(zr_player* p, const char* name, const char* value)
try
{
    if (!p || !name || !value) return fail("null argument");
    int r = p->impl.SetProperty(name, value);
    return r < 0 ? fail(std::string("cannot set ") + name) : 0;
}
catch (...) { return fail_exception(); }

ZRAWCORE_API int zr_player_get_property(zr_player* p, const char* name, char* out, int32_t out_size)
try
{
    if (!p || !name || !out || out_size < 1) return fail("null argument");
    std::string v;
    if (!p->impl.GetProperty(name, v)) return fail(std::string("property unavailable: ") + name);
    if ((int32_t)v.size() >= out_size) return fail("buffer too small");
    memcpy(out, v.c_str(), v.size() + 1);
    return 0;
}
catch (...) { return fail_exception(); }

ZRAWCORE_API int zr_player_is_closed(zr_player* p) try { return p && p->impl.Closed() ? 1 : 0; } catch (...) { fail_exception(); return 0; }

ZRAWCORE_API void zr_player_destroy(zr_player* p) try { delete p; } catch (...) { fail_exception(); }

}

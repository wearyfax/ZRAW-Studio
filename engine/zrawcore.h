/* zrawcore: a plain C API for GUIs (C# via P/Invoke, CharSet.Unicode for the paths) that provides
 * ZRAW clip info, preview frames and an embeddable libmpv player. It handles true RAW (Z CAM firmware
 * before 0.94) and HEVC ZRAW (0.94 on).
 *
 * zrawcore.dll needs libmpv-2.dll next to it for the player functions; clip info and frame rendering
 * work without it. Functions returning int return 0 on success and a negative value on failure;
 * zr_last_error() gives the reason on the same thread.
 */
#pragma once
#include <stdint.h>
#include <wchar.h>

#ifdef ZRAWCORE_BUILD
#define ZRAWCORE_API __declspec(dllexport)
#else
#define ZRAWCORE_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zr_clip_info
{
    int32_t frames;
    double fps;
    int32_t width, height;   /* full sensor resolution; previews are half of each */
    int32_t bits;            /* bit depth per photosite, 12 on the E2 */
} zr_clip_info;

typedef struct zr_preview_settings
{
    double exposure;   /* stops, 0 = as recorded */
    double wb_red;     /* multipliers on top of the camera's white balance, 1 = as recorded */
    double wb_blue;
    int32_t full_res;  /* player only: 1 = 4K debayered on the GPU, 0 = half resolution on the CPU.
                          Changing it reopens the clip at the same frame. */
    int32_t matrix;    /* camera to sRGB matrix: 0 = ZRAW Studio (our calibration, the default),
                          1 = ZCam-DNG (zraw-parser's fixed one), 2 = Camera built-in matrix (the camera's own),
                          3 = VideoSuite. 0, 2 and 3 follow each clip's color temperature. */
} zr_preview_settings;

typedef struct zr_player zr_player;

ZRAWCORE_API const char* zr_last_error(void);

ZRAWCORE_API int zr_clip_info_get(const wchar_t* path, zr_clip_info* out);

/* Half-resolution rgb24 preview of one frame, rows top to bottom, (width/2) * 3 bytes per row.
 * `rgb` must hold (width/2) * (height/2) * 3 bytes. `settings` may be NULL for the recorded look. */
ZRAWCORE_API int zr_render_frame(const wchar_t* path, int32_t frame, const zr_preview_settings* settings, uint8_t* rgb, int64_t rgb_size);

/* Creates a player that draws into `hwnd` (a panel in your window), or opens its own window when
 * hwnd is NULL. In both cases mpv's keyboard and mouse controls and its on-screen seek bar work, and
 * so do Ctrl+Up/Down (exposure in 0.5 stops, Ctrl+Shift for 0.25), Ctrl+0 (reset) and Ctrl+Left/Right
 * (warmer/cooler, Ctrl+Shift for finer steps). */
ZRAWCORE_API zr_player* zr_player_create(void* hwnd);
/* Returns once the first frame is on screen (or after 10 s with an error), so seeking can follow. */
ZRAWCORE_API int zr_player_open(zr_player* p, const wchar_t* path);
/* Frame-accurate seek and position, in frames from 0. Use these for a scrub bar. */
ZRAWCORE_API int zr_player_seek_frame(zr_player* p, int32_t frame);
ZRAWCORE_API int32_t zr_player_get_frame(zr_player* p);   /* -1 if nothing is loaded */
ZRAWCORE_API int zr_player_set_preview(zr_player* p, const zr_preview_settings* settings);
ZRAWCORE_API int zr_player_get_preview(zr_player* p, zr_preview_settings* out);

/* Pass-through to mpv, for everything else a GUI needs:
 *   commands:   "cycle pause", "seek 10", "seek 50 absolute-percent+exact", "frame-step", "frame-back-step"
 *   properties: set "pause" "yes", "lut" "C:/luts/zlog2_to_709.cube", "speed" "2";
 *               get "time-pos", "duration", "pause", "estimated-frame-number" */
ZRAWCORE_API int zr_player_command(zr_player* p, const char* command);
ZRAWCORE_API int zr_player_set_property(zr_player* p, const char* name, const char* value);
ZRAWCORE_API int zr_player_get_property(zr_player* p, const char* name, char* out, int32_t out_size);

/* 1 once mpv's own window has been closed (or mpv quit), else 0. */
ZRAWCORE_API int zr_player_is_closed(zr_player* p);
ZRAWCORE_API void zr_player_destroy(zr_player* p);

#ifdef __cplusplus
}
#endif

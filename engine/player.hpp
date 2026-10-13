// ZRAW playback in libmpv with full seeking.
//
// mpv opens "zraw://current" through a custom stream: the clip's preview frames laid end to end as
// raw rgb24 (packed Bayer as rgba64 at full resolution). mpv's rawvideo demuxer turns every seek into
// a byte offset, so we decode exactly the frame it lands on. Worker threads decode ahead of the read
// position into a small cache (keeping a few frames behind it for stepping back), so playback,
// scrubbing and frame stepping all work. Changing the preview settings clears the cache and makes mpv
// re-read the current frame.
#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <mpv/client.h>
#include <mpv/stream_cb.h>
#include "frameserver.hpp"

#pragma comment(lib, "user32.lib")   // SetProcessDpiAwarenessContext

// libmpv-2.dll is loaded at run time: the official Windows dev package has no MSVC import library.
struct MpvApi
{
    HMODULE dll = nullptr;
#define MPV_FN(name) decltype(&::name) name = nullptr;
    MPV_FN(mpv_create) MPV_FN(mpv_initialize) MPV_FN(mpv_terminate_destroy) MPV_FN(mpv_set_option_string)
    MPV_FN(mpv_set_property_string) MPV_FN(mpv_get_property_string) MPV_FN(mpv_free) MPV_FN(mpv_command)
    MPV_FN(mpv_command_string) MPV_FN(mpv_wait_event) MPV_FN(mpv_wakeup) MPV_FN(mpv_stream_cb_add_ro) MPV_FN(mpv_error_string)
#undef MPV_FN

    // Looks next to the module containing this code (zrawdng.exe or zrawcore.dll), then on the DLL path.
    bool Load(std::string& err)
    {
        if (dll) return true;
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&MpvApi::Instance, &self);
        wchar_t path[MAX_PATH];
        if (self && GetModuleFileNameW(self, path, MAX_PATH))
            dll = LoadLibraryW((std::filesystem::path(path).parent_path() / L"libmpv-2.dll").c_str());
        if (!dll) dll = LoadLibraryW(L"libmpv-2.dll");
        if (!dll) { err = "libmpv-2.dll not found next to the program"; return false; }
#define MPV_LOAD(name) if (!(name = (decltype(name))GetProcAddress(dll, #name))) { err = "libmpv-2.dll lacks " #name; return false; }
        MPV_LOAD(mpv_create) MPV_LOAD(mpv_initialize) MPV_LOAD(mpv_terminate_destroy) MPV_LOAD(mpv_set_option_string)
        MPV_LOAD(mpv_set_property_string) MPV_LOAD(mpv_get_property_string) MPV_LOAD(mpv_free) MPV_LOAD(mpv_command)
        MPV_LOAD(mpv_command_string) MPV_LOAD(mpv_wait_event) MPV_LOAD(mpv_wakeup) MPV_LOAD(mpv_stream_cb_add_ro) MPV_LOAD(mpv_error_string)
#undef MPV_LOAD
        return true;
    }

    static MpvApi& Instance() { static MpvApi api; return api; }
};

class ZrawPlayer
{
    MpvApi& api_ = MpvApi::Instance();
    mpv_handle* mpv_ = nullptr;
    std::mutex m_;
    std::shared_ptr<PreviewSource> source_;
    std::shared_ptr<HevcZraw> hevc_;   // set instead of source_ for HEVC ZRAW clips
    PreviewSettings settings_;
    std::filesystem::path path_;
    std::thread events_;
    std::atomic<bool> closed_{false};
    std::mutex load_m_;
    std::condition_variable load_cv_;
    bool loaded_ = false;    // set when mpv has the clip ready to show (first frame decoded)
    double demux_fps_ = 0;   // the exact rate mpv was given, so frame <-> time maps both ways
    int threads_;

    static int open_stream(void* user, char* uri, mpv_stream_cb_info* info)
    {
        auto self = (ZrawPlayer*)user;
        std::lock_guard<std::mutex> l(self->m_);
        if (self->hevc_ && !strcmp(uri, "zraw://hevc")) { FillHevcStream(self->hevc_, info); return 0; }
        if (!self->source_ || strcmp(uri, "zraw://current") != 0) return MPV_ERROR_LOADING_FAILED;
        FillPreviewStream(self->source_, info);
        return 0;
    }

    // mpv's %length% quoting, so paths with commas survive the per-file option list
    static std::string quoted(const std::string& v) { return "%" + std::to_string(v.size()) + "%" + v; }

    void event_loop()
    {
        while (true)
        {
            mpv_event* e = api_.mpv_wait_event(mpv_, -1);
            if (e->event_id == MPV_EVENT_SHUTDOWN) { closed_ = true; { std::lock_guard<std::mutex> l(load_m_); loaded_ = true; } load_cv_.notify_all(); return; }
            if (e->event_id == MPV_EVENT_PLAYBACK_RESTART) { { std::lock_guard<std::mutex> l(load_m_); loaded_ = true; } load_cv_.notify_all(); }
            if (e->event_id == MPV_EVENT_CLIENT_MESSAGE)
            {
                // Key bindings send "script-message zraw <what> <amount>"
                auto msg = (mpv_event_client_message*)e->data;
                if (msg->num_args == 3 && !strcmp(msg->args[0], "zraw"))
                {
                    PreviewSettings s = Settings();
                    double v = atof(msg->args[2]);
                    if (!strcmp(msg->args[1], "exposure")) s.exposure = v == 0 ? 0 : s.exposure + v;
                    else if (!strcmp(msg->args[1], "warmth")) { s.wb_red *= std::pow(2.0, v); s.wb_blue /= std::pow(2.0, v); }
                    SetSettings(s);
                    char text[96];
                    snprintf(text, sizeof(text), "Exposure %+.2f stops   WB red x%.2f blue x%.2f", s.exposure, s.wb_red, s.wb_blue);
                    const char* cmd[] = {"show-text", text, nullptr};
                    api_.mpv_command(mpv_, cmd);
                }
            }
        }
    }

public:
    explicit ZrawPlayer(int decode_threads = 6) : threads_(decode_threads) {}
    ZrawPlayer(const ZrawPlayer&) = delete;
    ZrawPlayer& operator=(const ZrawPlayer&) = delete;

    // parent: window to draw into, or null for mpv's own window. Keyboard, mouse and the on-screen
    // controller work in both cases.
    bool Create(HWND parent, std::string& err)
    {
        if (!api_.Load(err)) return false;
        // With display scaling (say 175%), a DPI-unaware process gets a smaller virtual window that
        // Windows stretches, which blurs 4K. mpv's own window needs the process to be DPI aware; an
        // embedding GUI decides that for itself (WinForms: Application.SetHighDpiMode).
        if (!parent) SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        mpv_ = api_.mpv_create();
        if (!mpv_) { err = "mpv_create failed"; return false; }
        if (parent) api_.mpv_set_option_string(mpv_, "wid", std::to_string((intptr_t)parent).c_str());
        const char* opts[][2] = {
            {"osc", "yes"}, {"input-default-bindings", "yes"}, {"input-vo-keyboard", "yes"},
            {"force-window", "yes"}, {"keep-open", "yes"}, {"idle", "yes"}, {"hr-seek", "yes"}};
        for (auto& o : opts) api_.mpv_set_option_string(mpv_, o[0], o[1]);
        if (api_.mpv_initialize(mpv_) < 0) { err = "mpv_initialize failed"; return false; }
        api_.mpv_stream_cb_add_ro(mpv_, "zraw", this, open_stream);
        api_.mpv_command_string(mpv_, "keybind Ctrl+UP \"script-message zraw exposure 0.5\"");
        api_.mpv_command_string(mpv_, "keybind Ctrl+DOWN \"script-message zraw exposure -0.5\"");
        api_.mpv_command_string(mpv_, "keybind Ctrl+0 \"script-message zraw exposure 0\"");
        api_.mpv_command_string(mpv_, "keybind Ctrl+Shift+UP \"script-message zraw exposure 0.25\"");
        api_.mpv_command_string(mpv_, "keybind Ctrl+Shift+DOWN \"script-message zraw exposure -0.25\"");
        api_.mpv_command_string(mpv_, "keybind Ctrl+RIGHT \"script-message zraw warmth 0.1\"");
        api_.mpv_command_string(mpv_, "keybind Ctrl+LEFT \"script-message zraw warmth -0.1\"");
        api_.mpv_command_string(mpv_, "keybind Ctrl+Shift+RIGHT \"script-message zraw warmth 0.05\"");
        api_.mpv_command_string(mpv_, "keybind Ctrl+Shift+LEFT \"script-message zraw warmth -0.05\"");
        events_ = std::thread([this] { event_loop(); });
        return true;
    }

    ~ZrawPlayer()
    {
        if (mpv_)
        {
            if (!closed_) api_.mpv_command_string(mpv_, "quit");
            if (events_.joinable()) events_.join();
            api_.mpv_terminate_destroy(mpv_);
        }
    }

    // wait: return only once the first frame is up. Never wait from mpv's event thread, which delivers that news.
    bool Open(const std::filesystem::path& path, std::string& err, int start_frame = 0, bool wait = true)
    {
        MovZraw mz;
        std::string ignored;
        if (ParseZrawMov(path, mz, ignored) && mz.version == 0x45A32DEF) return OpenHevc(path, err, start_frame, wait);
        auto src = std::make_shared<PreviewSource>();
        src->clip = std::make_unique<ZrawClip>();
        if (!src->clip->Open(path, err)) return false;
        {
            std::lock_guard<std::mutex> l(m_);
            src->server = std::make_unique<FrameServer>(*src->clip, settings_, threads_);
            source_ = src;
            hevc_.reset();
            path_ = path;
        }
        char fps[32];
        snprintf(fps, sizeof(fps), "%.6f", src->clip->fps);
        demux_fps_ = atof(fps);
        std::string opts = std::string("demuxer=rawvideo,demuxer-rawvideo-mp-format=") + src->server->Format() +
            ",demuxer-rawvideo-w=" + std::to_string(src->server->Width()) +
            ",demuxer-rawvideo-h=" + std::to_string(src->server->Height()) +
            ",demuxer-rawvideo-fps=" + fps +
            ",audio-file=" + quoted(path.u8string()) +
            ",force-media-title=" + quoted(path.filename().u8string() + " (ZRAW preview)");
        if (start_frame > 0) opts += ",start=" + std::to_string(start_frame / src->clip->fps);
        if (src->server->FullRes())
        {
            // The debayer shader plus its live parameters
            std::filesystem::path shader = WriteDebayerShaderFile();
            opts += ",glsl-shaders=" + quoted(shader.u8string()) + ",glsl-shader-opts=" + quoted(ShaderParams(Settings(), src->server->Matrix()));
            // The texture is a quarter as wide as the picture; this makes mpv show it at its real shape
            opts += ",video-aspect-override=" + std::to_string(src->clip->width) + ":" + std::to_string(src->clip->height);
        }
        return load("zraw://current", opts, err, wait);
    }

    // HEVC ZRAW: mpv plays the descrambled clip like any HEVC file. full_res picks the decoder:
    // hardware (GPU) or software (CPU). Exposure and white balance don't apply: the picture is
    // already developed in the camera.
    bool OpenHevc(const std::filesystem::path& path, std::string& err, int start_frame = 0, bool wait = true)
    {
        auto z = std::make_shared<HevcZraw>();
        if (!z->Open(path, err)) return false;
        bool gpu;
        {
            std::lock_guard<std::mutex> l(m_);
            hevc_ = z;
            source_.reset();
            path_ = path;
            gpu = settings_.full_res;
        }
        demux_fps_ = z->fps;
        std::string opts = std::string("hwdec=") + (gpu ? "auto-safe" : "no") +
                           ",force-media-title=" + quoted(path.filename().u8string() + " (ZRAW HEVC)");
        if (start_frame > 0) opts += ",start=" + std::to_string(start_frame / z->fps);
        return load("zraw://hevc", opts, err, wait);
    }

    bool load(const char* uri, const std::string& opts, std::string& err, bool wait)
    {
        const char* cmd[] = {"loadfile", uri, "replace", "0", opts.c_str(), nullptr};
        { std::lock_guard<std::mutex> l(load_m_); loaded_ = false; }
        int r = api_.mpv_command(mpv_, cmd);
        if (r < 0) { err = std::string("loadfile: ") + api_.mpv_error_string(r); return false; }
        if (!wait) return true;
        std::unique_lock<std::mutex> l(load_m_);
        if (!load_cv_.wait_for(l, std::chrono::seconds(10), [&] { return loaded_; })) { err = "mpv did not start the clip"; return false; }
        return true;
    }

    PreviewSettings Settings() { std::lock_guard<std::mutex> l(m_); return settings_; }

    void SetSettings(const PreviewSettings& s)
    {
        std::shared_ptr<PreviewSource> src;
        std::filesystem::path path;
        bool hevc;
        { std::lock_guard<std::mutex> l(m_); settings_ = s; src = source_; path = path_; hevc = hevc_ != nullptr; }
        if (hevc) { api_.mpv_set_property_string(mpv_, "hwdec", s.full_res ? "auto-safe" : "no"); return; }   // mpv switches decoders in place
        if (!src) return;
        if ((s.full_res && FullResSupported(src->clip->bits)) != src->server->FullRes())
        {
            // Different frame format: reopen the clip at the same frame
            std::string err;
            Open(path, err, std::max(0, CurrentFrame()), false);
            return;
        }
        if (s.full_res)
        {
            // The shader applies the look: just update its parameters, nothing is decoded again
            src->server->SetSettings(s);   // keeps the matrix for a new color choice
            api_.mpv_set_property_string(mpv_, "glsl-shader-opts", ShaderParams(s, src->server->Matrix()).c_str());
            return;
        }
        src->server->SetSettings(s);
        // Drop what mpv already buffered and re-read the frame on screen, also while paused.
        // Read the frame number first: drop-buffers moves the position.
        int frame = CurrentFrame();
        api_.mpv_command_string(mpv_, "drop-buffers");
        if (frame >= 0) SeekFrame(frame);
    }

    // Frame-accurate seek. mpv shows the first frame whose timestamp is at or after the target
    // (within 5 ms), and frame n's timestamp is n / fps, so the target is exactly that.
    int SeekFrame(int frame)
    {
        if (demux_fps_ <= 0) return MPV_ERROR_INVALID_PARAMETER;
        char t[48];
        snprintf(t, sizeof(t), "%.9f", frame / demux_fps_);
        const char* cmd[] = {"seek", t, "absolute+exact", nullptr};
        return api_.mpv_command(mpv_, cmd);
    }

    // Frame on screen, or -1 if nothing is loaded
    // From time-pos: mpv's estimated-frame-number floors time x container fps, and for HEVC ZRAW that
    // fps (23.976025) is a hair above 24000/1001, so it reads one frame low
    int CurrentFrame()
    {
        std::string v;
        return GetProperty("time-pos", v) && demux_fps_ > 0 ? (int)std::lround(atof(v.c_str()) * demux_fps_) : -1;
    }

    int Command(const char* command) { return api_.mpv_command_string(mpv_, command); }
    int SetProperty(const char* name, const char* value) { return api_.mpv_set_property_string(mpv_, name, value); }
    bool GetProperty(const char* name, std::string& out)
    {
        char* v = api_.mpv_get_property_string(mpv_, name);
        if (!v) return false;
        out = v;
        api_.mpv_free(v);
        return true;
    }
    bool Closed() const { return closed_; }
    void WaitUntilClosed() { if (events_.joinable()) events_.join(); }
};

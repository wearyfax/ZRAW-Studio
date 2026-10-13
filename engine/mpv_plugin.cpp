// zraw.dll: an mpv C plugin that plays ZRAW clips in a regular mpv. It handles true RAW (Z CAM
// firmware before 0.94) and HEVC ZRAW (later firmware; E2 1.0.9 names them .ZRAW, see hevczraw.hpp).
// Install by copying zraw.dll into mpv's scripts folder (%APPDATA%\mpv\scripts). Needs an mpv built
// with C plugin support, and vo=gpu-next for the 4K preview.
//
// Before mpv opens any file, the on_load hook checks whether it is a ZRAW clip (reads only the moov
// atom). If it is, mpv is pointed at "zraw://<path>", a custom stream of decoded frames (see
// frameserver.hpp), with file-local options for the raw frame format, the GPU debayer shader, the
// clip's own audio, and debanding off. Every other file plays as it would without the plugin.
//
// While a ZRAW clip plays: Ctrl+Up/Down exposure 0.5 stop (Ctrl+Shift 0.25), Ctrl+Right/Left warmer
// and cooler (Ctrl+Shift finer), Ctrl+0 reset, Ctrl+H 4K GPU / half-resolution CPU preview.
// The starting color matrix and exposure come from script-opts\zraw.conf (see load_conf below).
// HEVC ZRAW clips play through "zrawhevc://<path>", which presents the clip descrambled as a plain
// QuickTime file. They keep mpv's own keys, including Ctrl+H, which toggles hardware (GPU) and
// software (CPU) decoding.
#define MPV_CPLUGIN_DYNAMIC_SYM   // mpv fills in the API function pointers when it loads the plugin
#include <mpv/client.h>
#include <mpv/stream_cb.h>
#include <cmath>
#include "frameserver.hpp"

namespace
{
    mpv_handle* g_mpv = nullptr;
    std::mutex g_m;
    std::shared_ptr<PreviewSource> g_current;   // ZRAW clip being loaded or played, null for other files
    std::string g_current_path;                  // UTF-8, as mpv gave it
    std::shared_ptr<HevcZraw> g_hevc;            // HEVC ZRAW clip being loaded or played
    PreviewSettings g_settings;                  // kept across files, so a look carries over while browsing
    std::filesystem::path g_shader;

    // Defaults from mpv's script-opts\zraw.conf, which ZRAW Studio's Player tab writes:
    //   matrix=zrawstudio | zcamdng | camera | videosuite      exposure=<stops>
    // Read as each clip loads; when the file has changed, its values replace the look carried over
    // from the previous clip. Ctrl+0 goes back to the default exposure.
    std::filesystem::path g_conf;
    std::filesystem::file_time_type g_conf_time;
    double g_default_exposure = 0;

    void load_conf()   // g_m held
    {
        std::error_code ec;
        auto t = std::filesystem::last_write_time(g_conf, ec);
        if (ec || t == g_conf_time) return;
        g_conf_time = t;
        std::ifstream in(g_conf);
        for (std::string line; std::getline(in, line);)
        {
            line = line.substr(0, line.find('#'));
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto trim = [](std::string s) { s.erase(0, s.find_first_not_of(" \t\r")); s.erase(s.find_last_not_of(" \t\r") + 1); return s; };
            std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
            if (key == "exposure") g_settings.exposure = g_default_exposure = atof(value.c_str());
            else if (key == "matrix")
                for (int i = 0; i < 4; ++i) if (value == kPreviewMatrixNames[i]) g_settings.matrix = i;
        }
    }

    int decode_threads() { return (int)std::max(2u, std::min(8u, std::thread::hardware_concurrency() / 2)); }

    std::string get_string(const char* name)
    {
        char* v = mpv_get_property_string(g_mpv, name);
        std::string s = v ? v : "";
        if (v) mpv_free(v);
        return s;
    }

    void set_local(const char* option, const std::string& value)
    {
        mpv_set_property_string(g_mpv, (std::string("file-local-options/") + option).c_str(), value.c_str());
    }

    // Sets a file-local list option to `items`, optionally after the option's current entries
    void set_local_list(const char* option, std::vector<std::string> items, bool keep_current)
    {
        if (keep_current)
        {
            mpv_node cur;
            if (mpv_get_property(g_mpv, option, MPV_FORMAT_NODE, &cur) >= 0)
            {
                if (cur.format == MPV_FORMAT_NODE_ARRAY)
                    for (int i = 0; i < cur.u.list->num; ++i)
                        if (cur.u.list->values[i].format == MPV_FORMAT_STRING) items.insert(items.end() - 1, cur.u.list->values[i].u.string);
                mpv_free_node_contents(&cur);
            }
        }
        std::vector<mpv_node> nodes(items.size());
        for (size_t i = 0; i < items.size(); ++i) { nodes[i].format = MPV_FORMAT_STRING; nodes[i].u.string = (char*)items[i].c_str(); }
        mpv_node_list list = {(int)nodes.size(), nodes.data(), nullptr};
        mpv_node node;
        node.format = MPV_FORMAT_NODE_ARRAY;
        node.u.list = &list;
        mpv_set_property(g_mpv, (std::string("file-local-options/") + option).c_str(), MPV_FORMAT_NODE, &node);
    }

    void command(std::initializer_list<const char*> args)
    {
        std::vector<const char*> v(args);
        v.push_back(nullptr);
        mpv_command(g_mpv, v.data());
    }

    int open_stream(void*, char* uri, mpv_stream_cb_info* info)
    {
        std::string path = uri + 7;   // after "zraw://"
        std::shared_ptr<PreviewSource> src;
        {
            std::lock_guard<std::mutex> l(g_m);
            if (g_current && g_current_path == path) src = g_current;
        }
        if (!src)   // mpv reopened the stream on its own (a reload): open the clip again
        {
            src = std::make_shared<PreviewSource>();
            src->clip = std::make_unique<ZrawClip>();
            std::string err;
            if (!src->clip->Open(std::filesystem::u8path(path), err)) return MPV_ERROR_LOADING_FAILED;
            std::lock_guard<std::mutex> l(g_m);
            src->server = std::make_unique<FrameServer>(*src->clip, g_settings, decode_threads());
        }
        FillPreviewStream(src, info);
        return 0;
    }

    int open_hevc_stream(void*, char* uri, mpv_stream_cb_info* info)
    {
        std::string path = uri + 11;   // after "zrawhevc://"
        std::shared_ptr<HevcZraw> z;
        {
            std::lock_guard<std::mutex> l(g_m);
            if (g_hevc && g_current_path == path) z = g_hevc;
        }
        if (!z)
        {
            z = std::make_shared<HevcZraw>();
            std::string err;
            if (!z->Open(std::filesystem::u8path(path), err)) return MPV_ERROR_LOADING_FAILED;
        }
        FillHevcStream(z, info);
        return 0;
    }

    // on_load: decide whether this file is ours
    void on_load()
    {
        std::string path = get_string("path");
        std::filesystem::path p = std::filesystem::u8path(path);
        std::shared_ptr<PreviewSource> src;
        std::shared_ptr<HevcZraw> hevc;
        std::string err;
        if (_wcsicmp(p.extension().c_str(), L".mov") == 0 || _wcsicmp(p.extension().c_str(), L".zraw") == 0)
        {
            MovZraw z;
            if (ParseZrawMov(p, z, err) && z.version == 0x12EA78D2)
            {
                src = std::make_shared<PreviewSource>();
                src->clip = std::make_unique<ZrawClip>();
                if (!src->clip->Open(p, err)) src.reset();
            }
            else if (z.version == 0x45A32DEF)
            {
                hevc = std::make_shared<HevcZraw>();
                if (!hevc->Open(p, err))
                {
                    command({"print-text", ("zraw: " + p.filename().u8string() + ": " + err).c_str()});
                    hevc.reset();
                }
            }
        }
        PreviewSettings settings;
        {
            std::lock_guard<std::mutex> l(g_m);
            load_conf();
            if (src) src->server = std::make_unique<FrameServer>(*src->clip, g_settings, decode_threads());
            g_current = src;
            g_hevc = hevc;
            g_current_path = path;
            settings = g_settings;
        }
        if (!src) command({"disable-section", "zraw"});   // mpv's own keys, including Ctrl+H for hardware decoding
        if (hevc)
        {
            // Plain HEVC once descrambled: mpv demuxes and decodes it like any camera file, audio included
            mpv_set_property_string(g_mpv, "stream-open-filename", ("zrawhevc://" + path).c_str());
            set_local("force-media-title", p.filename().u8string() + " (ZRAW HEVC)");
            command({"print-text", ("zraw: " + p.filename().u8string() + ", HEVC ZRAW (Ctrl+H switches GPU / CPU decoding)").c_str()});
        }
        if (!src) return;

        auto& server = *src->server;
        char fps[32];
        snprintf(fps, sizeof(fps), "%.6f", src->clip->fps);
        mpv_set_property_string(g_mpv, "stream-open-filename", ("zraw://" + path).c_str());
        set_local("demuxer", "rawvideo");
        set_local("demuxer-rawvideo-mp-format", server.Format());
        set_local("demuxer-rawvideo-w", std::to_string(server.Width()));
        set_local("demuxer-rawvideo-h", std::to_string(server.Height()));
        set_local("demuxer-rawvideo-fps", fps);
        set_local("deband", "no");   // would smooth the raw mosaic before our shader sees it
        set_local("force-media-title", p.filename().u8string() + " (ZRAW)");
        set_local_list("audio-files", {path}, false);
        if (server.FullRes())
        {
            set_local_list("glsl-shaders", {g_shader.u8string()}, true);   // after any shaders mpv already has configured
            set_local("glsl-shader-opts", ShaderParams(settings, server.Matrix()));
            set_local("video-aspect-override", std::to_string(src->clip->width) + ":" + std::to_string(src->clip->height));
        }
        command({"enable-section", "zraw", "allow-hide-cursor+allow-vo-dragging"});
        std::string note = "zraw: " + p.filename().u8string() + (server.FullRes() ? ", 4K preview debayered on the GPU" : ", half-resolution preview") +
                           ", " + std::to_string(server.DefectCount()) + " defective photosites repaired";
        command({"print-text", note.c_str()});
    }

    void on_message(mpv_event_client_message* msg)
    {
        if (msg->num_args < 1) return;
        std::string what = msg->args[0];
        double v = msg->num_args > 1 ? atof(msg->args[1]) : 0;
        std::shared_ptr<PreviewSource> src;
        PreviewSettings s;
        std::string path;
        {
            std::lock_guard<std::mutex> l(g_m);
            src = g_current;
            path = g_current_path;
            if (what == "exposure") g_settings.exposure = v == 0 ? g_default_exposure : g_settings.exposure + v;
            else if (what == "warmth") { g_settings.wb_red *= std::pow(2.0, v); g_settings.wb_blue /= std::pow(2.0, v); }
            else if (what == "resolution") g_settings.full_res = !g_settings.full_res;
            else return;
            s = g_settings;
        }
        if (!src) return;

        if (what == "resolution")
        {
            // Different frame format: reload the clip where it is
            std::string t = get_string("time-pos");
            std::string opts = "start=" + (t.empty() ? std::string("0") : t);
            command({"loadfile", path.c_str(), "replace", "0", opts.c_str()});
            return;
        }
        if (src->server->FullRes())
            mpv_set_property_string(g_mpv, "glsl-shader-opts", ShaderParams(s, src->server->Matrix()).c_str());   // instant, no re-decode
        else
        {
            // The look is baked into half-resolution frames: re-render, then re-read the frame on screen
            std::string frame = get_string("estimated-frame-number");
            src->server->SetSettings(s);
            command({"drop-buffers"});
            if (!frame.empty())
            {
                char t[48];
                snprintf(t, sizeof(t), "%.9f", atoi(frame.c_str()) / src->clip->fps);
                command({"seek", t, "absolute+exact"});
            }
        }
        char text[96];
        snprintf(text, sizeof(text), "Exposure %+.2f stops   WB red x%.2f blue x%.2f", s.exposure, s.wb_red, s.wb_blue);
        command({"show-text", text});
    }
}

extern "C" __declspec(dllexport) int mpv_open_cplugin(mpv_handle* handle)
{
    g_mpv = handle;
    g_shader = WriteDebayerShaderFile();
    const char* expand[] = {"expand-path", "~~/script-opts/zraw.conf", nullptr};   // mpv's config folder, portable or not
    mpv_node conf;
    if (mpv_command_ret(g_mpv, expand, &conf) >= 0)
    {
        if (conf.format == MPV_FORMAT_STRING) g_conf = std::filesystem::u8path(conf.u.string);
        mpv_free_node_contents(&conf);
    }
    mpv_stream_cb_add_ro(g_mpv, "zraw", nullptr, open_stream);
    mpv_stream_cb_add_ro(g_mpv, "zrawhevc", nullptr, open_hevc_stream);
    mpv_hook_add(g_mpv, 0, "on_load", 50);

    // Key section, switched on only while a ZRAW clip plays ("force" wins over input.conf)
    std::string me = mpv_client_name(g_mpv);
    std::string keys;
    const char* binds[][2] = {
        {"Ctrl+UP", "exposure 0.5"}, {"Ctrl+DOWN", "exposure -0.5"},
        {"Ctrl+Shift+UP", "exposure 0.25"}, {"Ctrl+Shift+DOWN", "exposure -0.25"},
        {"Ctrl+0", "exposure 0"}, {"Ctrl+RIGHT", "warmth 0.1"}, {"Ctrl+LEFT", "warmth -0.1"},
        {"Ctrl+Shift+RIGHT", "warmth 0.05"}, {"Ctrl+Shift+LEFT", "warmth -0.05"}, {"Ctrl+h", "resolution"}};
    for (auto& b : binds) keys += std::string(b[0]) + " script-message-to " + me + " " + b[1] + "\n";
    command({"define-section", "zraw", keys.c_str(), "force"});

    while (true)
    {
        mpv_event* e = mpv_wait_event(g_mpv, -1);
        if (e->event_id == MPV_EVENT_SHUTDOWN) break;
        if (e->event_id == MPV_EVENT_HOOK)
        {
            auto hook = (mpv_event_hook*)e->data;
            if (!strcmp(hook->name, "on_load")) on_load();
            mpv_hook_continue(g_mpv, hook->id);
        }
        else if (e->event_id == MPV_EVENT_CLIENT_MESSAGE)
            on_message((mpv_event_client_message*)e->data);
    }
    std::lock_guard<std::mutex> l(g_m);
    g_current.reset();
    g_hevc.reset();
    return 0;
}

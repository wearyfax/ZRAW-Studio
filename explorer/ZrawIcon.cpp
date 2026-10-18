// ZrawIcon.dll: Explorer add-ons for .MOV and .ZRAW, registered per user from ZRAW Studio's Tagger tab.
//  - Icon handler: clips whose video track is ZRAW (stsd fourcc 'zraw') get the ZRAW icon;
//    every other .MOV gets its normal association icon.
//  - Thumbnail handler: ZRAW clips get a thumbnail decoded from the raw frame ~10% into the clip;
//    every other .MOV is handed to Windows' own Media Foundation thumbnailer, so H.265 is unchanged.
//  - Preview handler (Explorer's Preview pane): the same frame at 1920x1080 with a caption; other
//    .MOV files show Windows' thumbnail frame, since nothing else previews .MOV here.
// Runs inside Explorer / the thumbnail host, so it never throws and fails toward "normal behavior".
#define NOMINMAX   // hevczraw.hpp uses std::min
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <thumbcache.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wincodec.h>
#include <math.h>
#include <new>
#include <vector>
#include <string>
#include "../engine/zraw.hpp"
#include "../engine/hevc_frame.hpp"   // HEVC ZRAW: descrambled and decoded by Media Foundation
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

// Also in app/IconHandler.cs
static const CLSID CLSID_ZrawIcon  = { 0xcdb31174, 0x80bf, 0x4a2f, { 0x9b, 0xcb, 0xba, 0x05, 0xb0, 0xf2, 0x26, 0x83 } }; // {CDB31174-80BF-4A2F-9BCB-BA05B0F22683}
static const CLSID CLSID_ZrawThumb = { 0xd2b1645e, 0xfe53, 0x419c, { 0xa5, 0x7a, 0xcb, 0x81, 0x33, 0x6f, 0x8a, 0x26 } }; // {D2B1645E-FE53-419C-A57A-CB81336F8A26}
static const CLSID CLSID_ZrawPreview = { 0x0c3c2943, 0x9325, 0x417b, { 0xbb, 0x2a, 0xc8, 0x50, 0x0e, 0x5e, 0x0d, 0xa3 } }; // {0C3C2943-9325-417B-BB2A-C8500E5E0DA3}
static HMODULE g_mod;
static LONG g_objects;

#ifdef ZRAWICON_LOG // debug builds only: appends every call to %TEMP%\ZrawIcon.log
static void Log(const wchar_t* fmt, ...)
{
    wchar_t buf[2048], path[MAX_PATH]; va_list a; va_start(a, fmt);
    int n = wvnsprintfW(buf, 2000, fmt, a); va_end(a);
    if (n < 0) return;
    StringCchCatW(buf, ARRAYSIZE(buf), L"\r\n");
    GetTempPathW(MAX_PATH, path); StringCchCatW(path, MAX_PATH, L"ZrawIcon.log");
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w; WriteFile(f, buf, lstrlenW(buf) * 2, &w, NULL); CloseHandle(f);
}
#else
#define Log(...) ((void)0)
#endif

// ---------------------------------------------------------------- MOV parsing

struct Source   // random-access reads from a file handle or an IStream
{
    UINT64 size = 0;
    virtual bool Read(UINT64 off, void* buf, DWORD n) = 0;
};

struct FileSource : Source
{
    HANDLE f;
    explicit FileSource(HANDLE h) : f(h) { LARGE_INTEGER l; size = GetFileSizeEx(h, &l) ? (UINT64)l.QuadPart : 0; }
    bool Read(UINT64 off, void* buf, DWORD n) override
    {
        LARGE_INTEGER at; at.QuadPart = (LONGLONG)off; DWORD got = 0;
        return SetFilePointerEx(f, at, NULL, FILE_BEGIN) && ReadFile(f, buf, n, &got, NULL) && got == n;
    }
};

struct StreamSource : Source
{
    IStream* s;
    explicit StreamSource(IStream* st) : s(st) { STATSTG ss; size = SUCCEEDED(s->Stat(&ss, STATFLAG_NONAME)) ? ss.cbSize.QuadPart : 0; }
    bool Read(UINT64 off, void* buf, DWORD n) override
    {
        LARGE_INTEGER at; at.QuadPart = (LONGLONG)off;
        if (FAILED(s->Seek(at, STREAM_SEEK_SET, NULL))) return false;
        for (DWORD done = 0; done < n;)
        {
            ULONG got = 0;
            if (FAILED(s->Read((BYTE*)buf + done, n - done, &got)) || !got) return false;
            done += got;
        }
        return true;
    }
};

static UINT64 BE(const BYTE* b, int n) { UINT64 v = 0; for (int i = 0; i < n; i++) v = (v << 8) | b[i]; return v; }

// Skips over top-level atoms (mdat is gigabytes; Z CAM writes moov after it) and loads moov.
static bool ReadMoov(Source& src, std::vector<BYTE>& moov)
{
    BYTE h[16];
    UINT64 pos = 0, end = src.size;
    for (int atoms = 0; atoms < 64 && pos + 8 <= end; atoms++) // 64 top-level atoms is plenty for camera files
    {
        DWORD want = end - pos >= 16 ? 16 : 8;
        if (!src.Read(pos, h, want)) return false;
        UINT64 size = BE(h, 4), hdr = 8;
        if (size == 1) { if (want < 16) return false; size = BE(h + 8, 8); hdr = 16; }
        else if (size == 0) size = end - pos;
        if (size < hdr) return false;
        if (!memcmp(h + 4, "moov", 4))
        {
            if (size > (64u << 20) || pos + size > end) return false;
            moov.resize((size_t)(size - hdr));
            return src.Read(pos + hdr, moov.data(), (DWORD)moov.size());
        }
        pos += size;
    }
    return false;
}

struct Box { const BYTE* p = nullptr; size_t n = 0; };   // atom payload
struct Track { bool zraw = false; Box stsz, stco, co64, stsc; };

// moov > trak > mdia > minf > stbl > (stsd, stsz, stco/co64, stsc). A real file nests four deep; the depth limit
// keeps a crafted file of nested boxes from overflowing the stack inside explorer.exe, which no catch can stop.
static void Walk(const BYTE* b, size_t p, size_t end, std::vector<Track>& tracks, int depth = 0)
{
    if (depth > 8) return;
    while (p + 8 <= end)
    {
        UINT64 size = BE(b + p, 4); size_t hdr = 8;
        if (size == 1) { if (p + 16 > end) return; size = BE(b + p + 8, 8); hdr = 16; }
        else if (size == 0) size = end - p;
        if (size < hdr || size > end - p) return;
        const BYTE* t = b + p + 4;
        Box box; box.p = b + p + hdr; box.n = (size_t)size - hdr;
        if (!memcmp(t, "trak", 4)) { tracks.emplace_back(); Walk(b, p + hdr, p + (size_t)size, tracks, depth + 1); }
        else if (!memcmp(t, "mdia", 4) || !memcmp(t, "minf", 4) || !memcmp(t, "stbl", 4)) Walk(b, p + hdr, p + (size_t)size, tracks, depth + 1);
        else if (!tracks.empty())
        {
            Track& tr = tracks.back();
            // stsd payload: version/flags, entry count, then the first entry's size and fourcc
            if (!memcmp(t, "stsd", 4) && box.n >= 16 && !memcmp(box.p + 12, "zraw", 4)) tr.zraw = true;
            else if (!memcmp(t, "stsz", 4)) tr.stsz = box;
            else if (!memcmp(t, "stco", 4)) tr.stco = box;
            else if (!memcmp(t, "co64", 4)) tr.co64 = box;
            else if (!memcmp(t, "stsc", 4)) tr.stsc = box;
        }
        p += (size_t)size;
    }
}

static bool Tracks(Source& src, std::vector<BYTE>& moov, std::vector<Track>& tracks)
{
    if (!ReadMoov(src, moov)) return false;
    Walk(moov.data(), 0, moov.size(), tracks);
    return true;
}

static UINT32 SampleCount(const Track& t) { return t.stsz.n >= 12 ? (UINT32)BE(t.stsz.p + 8, 4) : 0; }

// File offset and size of sample n, via sample-to-chunk (stsc), chunk offsets and sample sizes.
static bool FindSample(const Track& t, UINT32 n, UINT64& off, UINT32& size)
{
    const Box& sz = t.stsz;
    const Box& sc = t.stsc;
    const Box& co = t.co64.p ? t.co64 : t.stco;
    const int cw = t.co64.p ? 8 : 4;
    if (sz.n < 12 || sc.n < 8 || co.n < 8) return false;
    UINT32 fixed = (UINT32)BE(sz.p + 4, 4), count = (UINT32)BE(sz.p + 8, 4);
    if (n >= count || (!fixed && sz.n < 12 + 4 * (size_t)count)) return false;
    auto sizeOf = [&](UINT32 i) { return fixed ? fixed : (UINT32)BE(sz.p + 12 + 4 * (size_t)i, 4); };
    UINT32 chunks = (UINT32)BE(co.p + 4, 4), runs = (UINT32)BE(sc.p + 4, 4);
    if (co.n < 8 + (size_t)cw * chunks || sc.n < 8 + 12 * (size_t)runs) return false;

    UINT64 base = 0;   // first sample number of the current run
    for (UINT32 r = 0; r < runs; r++)
    {
        UINT32 first = (UINT32)BE(sc.p + 8 + 12 * r, 4) - 1, per = (UINT32)BE(sc.p + 12 + 12 * r, 4);
        UINT32 next = r + 1 < runs ? (UINT32)BE(sc.p + 8 + 12 * (r + 1), 4) - 1 : chunks;
        if (!per || next < first) return false;
        UINT64 inRun = (UINT64)(next - first) * per;
        if (n < base + inRun)
        {
            UINT32 chunk = first + (UINT32)((n - base) / per), skip = (UINT32)((n - base) % per);
            if (chunk >= chunks) return false;
            off = BE(co.p + 8 + (size_t)cw * chunk, cw);
            for (UINT32 i = n - skip; i < n; i++) off += sizeOf(i);
            size = sizeOf(n);
            return true;
        }
        base += inRun;
    }
    return false;
}

// ---------------------------------------------------------------- ZRAW preview

// Camera-to-sRGB the way dcraw builds it from a DNG ColorMatrix: rgb_cam = inverse(cam_xyz * xyz_rgb),
// with the rows of cam_rgb normalized so white stays white after white balance.
static void CamToSrgb(float out[3][3])
{
    // The fixed ColorMatrix2 (D65) for the E2, as zraw-parser writes it
    const double cam_xyz[3][3] = { { 0.6770, -0.1895, -0.0744 }, { -0.5232, 1.3145, 0.2303 }, { -0.1664, 0.2691, 0.5703 } };
    const double xyz_rgb[3][3] = { { 0.412453, 0.357580, 0.180423 }, { 0.212671, 0.715160, 0.072169 }, { 0.019334, 0.119193, 0.950227 } };
    double m[3][3];
    for (int i = 0; i < 3; i++)
    {
        double sum = 0;
        for (int j = 0; j < 3; j++) { m[i][j] = 0; for (int k = 0; k < 3; k++) m[i][j] += cam_xyz[i][k] * xyz_rgb[k][j]; sum += m[i][j]; }
        for (int j = 0; j < 3; j++) m[i][j] /= sum;
    }
    double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
        {
            int a = (j + 1) % 3, b = (j + 2) % 3, c = (i + 1) % 3, d = (i + 2) % 3;
            out[i][j] = (float)((m[a][c] * m[b][d] - m[a][d] * m[b][c]) / det);   // adjugate / det
        }
}

static float Srgb(float v) { v = v < 0 ? 0 : v > 1 ? 1 : v; return v <= 0.0031308f ? 12.92f * v : 1.055f * powf(v, 1 / 2.4f) - 0.055f; }

static HBITMAP Dib(int w, int h, void** bits)
{
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;   // top-down
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, bits, NULL, 0);
}

struct FrameInfo { int width = 0, height = 0, bits = 0; UINT32 index = 0, count = 0; bool hevc = false; };

// Decodes one raw frame and box-filters its RGGB quads straight down to the thumbnail size.
static HBITMAP RenderZraw(Source& src, const Track& t, UINT cx, FrameInfo* info = nullptr)
{
    UINT32 count = SampleCount(t), size = 0;
    UINT64 off = 0;
    if (!count || !FindSample(t, count / 10, off, size) || size > (256u << 20)) { Log(L"no sample: count=%u", count); return NULL; }
    std::vector<uint8_t> data((size_t)size + 16, 0);   // the decoder reads 8 bytes past the end
    if (!src.Read(off, data.data(), size)) { Log(L"read failed at %I64u size %u", off, size); return NULL; }
    ZrawFrame f; std::string err;
    if (!ParseZrawFrame(data.data(), size, f, err)) { Log(L"parse failed: %S", err.c_str()); return NULL; }
    const int W = (int)f.width, H = (int)f.height, qw = W / 2, qh = H / 2;
    if (info) { info->width = W; info->height = H; info->bits = f.bits(); info->index = count / 10; info->count = count; }
    std::vector<uint16_t> cfa((size_t)W * H);
    DecodeZrawFrame(f, cfa.data());

    const int big = qw > qh ? qw : qh;
    const int ow = cx >= (UINT)big ? qw : (int)((INT64)qw * cx / big), oh = cx >= (UINT)big ? qh : (int)((INT64)qh * cx / big);
    if (ow < 1 || oh < 1) return NULL;
    std::vector<float> acc((size_t)ow * oh * 3, 0.f);
    std::vector<int> n((size_t)ow * oh, 0);
    const float white = (float)((1 << f.bits()) - 1);
    const float blk[4] = { (float)f.black[0], (float)f.black[1], (float)f.black[2], (float)f.black[3] };
    for (int qy = 0; qy < qh; qy++)
    {
        const uint16_t* r0 = &cfa[(size_t)(2 * qy) * W];
        const uint16_t* r1 = r0 + W;
        float* arow = &acc[(size_t)(qy * oh / qh) * ow * 3];
        int* nrow = &n[(size_t)(qy * oh / qh) * ow];
        for (int qx = 0; qx < qw; qx++)
        {
            int ox = qx * ow / qw;
            float* a = arow + 3 * ox;
            a[0] += r0[2 * qx] - blk[0];
            a[1] += (r0[2 * qx + 1] - blk[1] + r1[2 * qx] - blk[2]) * 0.5f;
            a[2] += r1[2 * qx + 1] - blk[3];
            nrow[ox]++;
        }
    }

    // White balance from the frame's own AWB gains (the numbers zrawdng writes as AsShotNeutral).
    float wr = f.awb_r && f.awb_g ? (float)f.awb_r / f.awb_g : 1.f, wb = f.awb_b && f.awb_g ? (float)f.awb_b / f.awb_g : 1.f;
    float m[3][3]; CamToSrgb(m);
    double lum = 0;
    for (size_t i = 0; i < n.size(); i++)
    {
        float* a = &acc[3 * i];
        float k = n[i] ? 1.f / (n[i] * (white - blk[0])) : 0.f;
        float c[3] = { a[0] * k * wr, a[1] * k, a[2] * k * wb };
        for (int j = 0; j < 3; j++) c[j] = c[j] < 0 ? 0 : c[j] > 1 ? 1 : c[j];   // clip before the matrix so blown highlights stay neutral
        for (int j = 0; j < 3; j++) a[j] = m[j][0] * c[0] + m[j][1] * c[1] + m[j][2] * c[2];
        lum += 0.2126 * a[0] + 0.7152 * a[1] + 0.0722 * a[2];
    }
    // Mean-based auto exposure to 18% gray; a histogram-based exposure would handle high-key shots better
    double mean = lum / n.size();
    float gain = mean > 1e-6 ? (float)(0.18 / mean) : 1.f;
    gain = gain < 0.5f ? 0.5f : gain > 32.f ? 32.f : gain;

    void* bits = NULL;
    HBITMAP hbm = Dib(ow, oh, &bits);
    if (!hbm) return NULL;
    BYTE* px = (BYTE*)bits;
    for (size_t i = 0; i < n.size(); i++, px += 4)
    {
        const float* a = &acc[3 * i];
        px[2] = (BYTE)(Srgb(a[0] * gain) * 255 + 0.5f);
        px[1] = (BYTE)(Srgb(a[1] * gain) * 255 + 0.5f);
        px[0] = (BYTE)(Srgb(a[2] * gain) * 255 + 0.5f);
        px[3] = 255;
    }
    return hbm;
}

// HEVC ZRAW (later firmware): the frame 10% in, decoded by Windows' HEVC decoder, at most 1920 wide
static HBITMAP RenderHevc(Source& src, UINT cx, FrameInfo* info = nullptr)
{
    HevcZraw z;
    std::string err;
    if (!z.Open([&src](uint64_t off, void* buf, size_t n) { return n <= 0xFFFFFFFFu && src.Read(off, buf, (DWORD)n); }, src.size, err))
    {
        Log(L"HEVC open failed: %S", err.c_str());
        return NULL;
    }
    const int qw = z.width / 2, qh = z.height / 2, big = qw > qh ? qw : qh;
    const int ow = cx >= (UINT)big ? qw : (int)((INT64)qw * cx / big), oh = cx >= (UINT)big ? qh : (int)((INT64)qh * cx / big);
    if (ow < 1 || oh < 1) return NULL;
    const int n = z.frames / 10;
    std::vector<uint8_t> rgb((size_t)ow * oh * 3);
    if (!RenderHevcZrawFrame(z, n, ow, oh, rgb.data(), err)) { Log(L"HEVC render failed: %S", err.c_str()); return NULL; }
    if (info) { info->width = z.width; info->height = z.height; info->bits = 10; info->index = n; info->count = z.frames; info->hevc = true; }
    void* bits = NULL;
    HBITMAP hbm = Dib(ow, oh, &bits);
    if (!hbm) return NULL;
    BYTE* px = (BYTE*)bits;
    for (size_t i = 0; i < (size_t)ow * oh; i++, px += 4) { px[2] = rgb[3 * i]; px[1] = rgb[3 * i + 1]; px[0] = rgb[3 * i + 2]; px[3] = 255; }
    return hbm;
}

// Not ZRAW: ask the .MOV property handler (Media Foundation) for its thumbnail, which is what
// Explorer uses when no thumbnail handler is registered, and scale it with WIC.
static HRESULT Delegate(IStream* s, UINT cx, HBITMAP* out)
{
    wchar_t clsid[64]; DWORD cb = sizeof(clsid); CLSID handler;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertyHandlers\\.mov",
                     NULL, RRF_RT_REG_SZ, NULL, clsid, &cb) != ERROR_SUCCESS || FAILED(CLSIDFromString(clsid, &handler))) return E_FAIL;
    LARGE_INTEGER zero = {};
    s->Seek(zero, STREAM_SEEK_SET, NULL);

    HRESULT hr;
    IInitializeWithStream* iws = NULL; IPropertyStore* ps = NULL; IWICImagingFactory* wic = NULL;
    IWICBitmapDecoder* dec = NULL; IWICBitmapFrameDecode* frame = NULL; IWICBitmapScaler* sc = NULL; IWICFormatConverter* cv = NULL;
    PROPVARIANT v; PropVariantInit(&v);
    UINT w = 0, h = 0, ow = 0, oh = 0;
    void* bits = NULL;
    if (FAILED(hr = CoCreateInstance(handler, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&iws)))) goto done;
    if (FAILED(hr = iws->Initialize(s, STGM_READ)) || FAILED(hr = iws->QueryInterface(IID_PPV_ARGS(&ps)))) goto done;
    if (FAILED(hr = ps->GetValue(PKEY_ThumbnailStream, &v))) goto done;
    if (v.vt != VT_STREAM) { hr = E_FAIL; goto done; }
    if (FAILED(hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) goto done;
    if (FAILED(hr = wic->CreateDecoderFromStream(v.pStream, NULL, WICDecodeMetadataCacheOnDemand, &dec))) goto done;
    if (FAILED(hr = dec->GetFrame(0, &frame)) || FAILED(hr = frame->GetSize(&w, &h))) goto done;
    if (!w || !h) { hr = E_FAIL; goto done; }
    if (w >= h) { ow = w > cx ? cx : w; oh = (UINT)((UINT64)h * ow / w); }
    else { oh = h > cx ? cx : h; ow = (UINT)((UINT64)w * oh / h); }
    if (!ow) ow = 1;
    if (!oh) oh = 1;
    if (FAILED(hr = wic->CreateBitmapScaler(&sc)) || FAILED(hr = sc->Initialize(frame, ow, oh, WICBitmapInterpolationModeFant))) goto done;
    if (FAILED(hr = wic->CreateFormatConverter(&cv)) ||
        FAILED(hr = cv->Initialize(sc, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom))) goto done;
    *out = Dib((int)ow, (int)oh, &bits);
    if (!*out) { hr = E_OUTOFMEMORY; goto done; }
    if (FAILED(hr = cv->CopyPixels(NULL, ow * 4, ow * oh * 4, (BYTE*)bits))) { DeleteObject(*out); *out = NULL; }
done:
    PropVariantClear(&v);
    if (cv) cv->Release();
    if (sc) sc->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (wic) wic->Release();
    if (ps) ps->Release();
    if (iws) iws->Release();
    return hr;
}

// ---------------------------------------------------------------- COM classes

static bool IsZraw(const wchar_t* path)
{
    DWORD a = GetFileAttributesW(path);
    // Never touch cloud placeholders: reading them would trigger a download from inside Explorer.
    if (a == INVALID_FILE_ATTRIBUTES || (a & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE |
        FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS))) return false;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool z = false;
    try
    {
        FileSource src(f);
        std::vector<BYTE> moov;
        std::vector<Track> tracks;
        if (Tracks(src, moov, tracks)) for (auto& t : tracks) z = z || t.zraw;
    }
    catch (...) {}
    CloseHandle(f);
    return z;
}

class IconHandler : public IExtractIconW, public IPersistFile
{
    LONG refs = 1;
    wchar_t path[2048] = L"";
public:
    IconHandler() { InterlockedIncrement(&g_objects); }
    ~IconHandler() { InterlockedDecrement(&g_objects); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (riid == IID_IUnknown || riid == IID_IExtractIconW) *ppv = static_cast<IExtractIconW*>(this);
        else if (riid == IID_IPersistFile || riid == IID_IPersist) *ppv = static_cast<IPersistFile*>(this);
        else { *ppv = NULL; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&refs); }
    IFACEMETHODIMP_(ULONG) Release() { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return r; }

    IFACEMETHODIMP GetIconLocation(UINT inFlags, PWSTR loc, UINT cch, int* index, UINT* flags)
    {
        HRESULT hr = Location(loc, cch, index, flags);
        Log(L"GetIconLocation flags=0x%x %s -> hr=0x%x %s,%d", inFlags, path, hr, hr == S_OK ? loc : L"", *index);
        return hr;
    }
    HRESULT Location(PWSTR loc, UINT cch, int* index, UINT* flags)
    {
        *flags = GIL_PERINSTANCE;
        *index = 0;
        if (IsZraw(path)) return GetModuleFileNameW(g_mod, loc, cch) ? S_OK : S_FALSE;
        // Not ZRAW: hand back whatever icon .mov normally has (mpv, VLC, Windows default...).
        wchar_t raw[MAX_PATH * 2], icon[MAX_PATH * 2];
        DWORD n = ARRAYSIZE(raw);
        if (FAILED(AssocQueryStringW(0, ASSOCSTR_DEFAULTICON, L".mov", NULL, raw, &n)) || raw[0] == L'%' && raw[1] == L'1') return S_FALSE;
        if (!ExpandEnvironmentStringsW(raw, icon, ARRAYSIZE(icon))) return S_FALSE;
        *index = PathParseIconLocationW(icon);
        PathUnquoteSpacesW(icon);
        return SUCCEEDED(StringCchCopyW(loc, cch, icon)) ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Extract(PCWSTR, UINT, HICON*, HICON*, UINT) { return S_FALSE; } // shell extracts from the location

    IFACEMETHODIMP GetClassID(CLSID* c) { *c = CLSID_ZrawIcon; return S_OK; }
    IFACEMETHODIMP IsDirty() { return S_FALSE; }
    IFACEMETHODIMP Load(LPCOLESTR name, DWORD) { return StringCchCopyW(path, ARRAYSIZE(path), name); }
    IFACEMETHODIMP Save(LPCOLESTR, BOOL) { return E_NOTIMPL; }
    IFACEMETHODIMP SaveCompleted(LPCOLESTR) { return E_NOTIMPL; }
    IFACEMETHODIMP GetCurFile(LPOLESTR*) { return E_NOTIMPL; }
};

class ThumbHandler : public IInitializeWithStream, public IThumbnailProvider
{
    LONG refs = 1;
    IStream* stream = NULL;
public:
    ThumbHandler() { InterlockedIncrement(&g_objects); }
    ~ThumbHandler() { if (stream) stream->Release(); InterlockedDecrement(&g_objects); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (riid == IID_IUnknown || riid == IID_IInitializeWithStream) *ppv = static_cast<IInitializeWithStream*>(this);
        else if (riid == IID_IThumbnailProvider) *ppv = static_cast<IThumbnailProvider*>(this);
        else { *ppv = NULL; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&refs); }
    IFACEMETHODIMP_(ULONG) Release() { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return r; }

    IFACEMETHODIMP Initialize(IStream* s, DWORD)
    {
        Log(L"Thumb Initialize");
        if (stream) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        stream = s; stream->AddRef();
        return S_OK;
    }

    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* out, WTS_ALPHATYPE* alpha)
    {
        *out = NULL; *alpha = WTSAT_RGB;
        if (!stream) return E_UNEXPECTED;
        try
        {
            DWORD t0 = GetTickCount();
            StreamSource src(stream);
            std::vector<BYTE> moov;
            std::vector<Track> tracks;
            bool parsed = Tracks(src, moov, tracks);
            Log(L"GetThumbnail cx=%u stream size=%I64u moov=%d tracks=%d", cx, src.size, parsed, (int)tracks.size());
            for (auto& t : tracks)
                if (t.zraw)
                {
                    *out = RenderZraw(src, t, cx);
                    if (!*out) *out = RenderHevc(src, cx);   // not a RAW frame: HEVC ZRAW
                    Log(L"GetThumbnail ZRAW cx=%u -> %s in %lu ms", cx, *out ? L"ok" : L"failed", GetTickCount() - t0);
                    return *out ? S_OK : E_FAIL;
                }
            HRESULT hr = Delegate(stream, cx, out);
            Log(L"GetThumbnail other cx=%u -> hr=0x%x in %lu ms", cx, hr, GetTickCount() - t0);
            return hr;
        }
        catch (...) { if (*out) { DeleteObject(*out); *out = NULL; } return E_OUTOFMEMORY; }
    }
};

// Explorer's Preview pane. Runs in prevhost.exe; draws one still frame, scaled to fit, with a caption.
class PreviewHandler : public IPreviewHandler, public IInitializeWithStream, public IObjectWithSite, public IOleWindow, public IPreviewHandlerVisuals
{
    LONG refs = 1;
    IStream* stream = NULL;
    IUnknown* site = NULL;
    HWND parent = NULL, wnd = NULL;
    RECT rc = {};
    HBITMAP bmp = NULL;
    int bw = 0, bh = 0;
    std::wstring caption;
    COLORREF bg = RGB(0, 0, 0), fg = RGB(200, 200, 200);
    HFONT font = NULL;

    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
        PreviewHandler* self = (PreviewHandler*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (self && m == WM_PAINT) { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); self->Paint(h, dc); EndPaint(h, &ps); return 0; }
        if (self && m == WM_PRINTCLIENT) { self->Paint(h, (HDC)w); return 0; }
        if (m == WM_ERASEBKGND) return 1;
        return DefWindowProcW(h, m, w, l);
    }

    void Paint(HWND h, HDC dc)   // double-buffered so resizing the pane doesn't flicker
    {
        RECT r; GetClientRect(h, &r);
        if (r.right <= 0 || r.bottom <= 0) return;
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP buf = CreateCompatibleBitmap(dc, r.right, r.bottom);
        HGDIOBJ oldBuf = SelectObject(mem, buf);
        HBRUSH br = CreateSolidBrush(bg); FillRect(mem, &r, br); DeleteObject(br);
        const int pad = 8, text = caption.empty() ? 0 : 28;
        int aw = r.right - 2 * pad, ah = r.bottom - 2 * pad - text;
        if (bmp && aw > 0 && ah > 0)
        {
            int dw = aw, dh = (int)((INT64)bh * aw / bw);
            if (dh > ah) { dh = ah; dw = (int)((INT64)bw * ah / bh); }
            HDC src = CreateCompatibleDC(dc);
            HGDIOBJ oldSrc = SelectObject(src, bmp);
            SetStretchBltMode(mem, HALFTONE); SetBrushOrgEx(mem, 0, 0, NULL);
            StretchBlt(mem, (r.right - dw) / 2, pad + (ah - dh) / 2, dw, dh, src, 0, 0, bw, bh, SRCCOPY);
            SelectObject(src, oldSrc); DeleteDC(src);
        }
        if (text)
        {
            RECT tr = { pad, r.bottom - pad - text, r.right - pad, r.bottom - pad };
            HGDIOBJ oldFont = SelectObject(mem, font ? font : GetStockObject(DEFAULT_GUI_FONT));
            SetBkMode(mem, TRANSPARENT); ::SetTextColor(mem, fg);
            DrawTextW(mem, caption.c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(mem, oldFont);
        }
        BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBuf); DeleteObject(buf); DeleteDC(mem);
    }

    void Place() { if (wnd) SetWindowPos(wnd, NULL, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE); }
    void Redraw() { if (wnd) InvalidateRect(wnd, NULL, FALSE); }

public:
    PreviewHandler() { InterlockedIncrement(&g_objects); }
    ~PreviewHandler()
    {
        Unload();
        if (font) DeleteObject(font);
        if (site) site->Release();
        InterlockedDecrement(&g_objects);
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (riid == IID_IUnknown || riid == IID_IPreviewHandler) *ppv = static_cast<IPreviewHandler*>(this);
        else if (riid == IID_IInitializeWithStream) *ppv = static_cast<IInitializeWithStream*>(this);
        else if (riid == IID_IObjectWithSite) *ppv = static_cast<IObjectWithSite*>(this);
        else if (riid == IID_IOleWindow) *ppv = static_cast<IOleWindow*>(this);
        else if (riid == IID_IPreviewHandlerVisuals) *ppv = static_cast<IPreviewHandlerVisuals*>(this);
        else { *ppv = NULL; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&refs); }
    IFACEMETHODIMP_(ULONG) Release() { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return r; }

    IFACEMETHODIMP Initialize(IStream* s, DWORD)
    {
        if (stream) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        stream = s; stream->AddRef();
        return S_OK;
    }

    IFACEMETHODIMP SetWindow(HWND h, const RECT* r)
    {
        if (!h || !r) return E_INVALIDARG;
        parent = h; rc = *r;
        if (wnd) { SetParent(wnd, parent); Place(); }
        return S_OK;
    }
    IFACEMETHODIMP SetRect(const RECT* r)
    {
        if (!r) return E_INVALIDARG;
        rc = *r; Place(); Redraw();
        return S_OK;
    }

    IFACEMETHODIMP DoPreview()
    {
        if (!stream || !parent) return E_UNEXPECTED;
        if (wnd) return S_OK;
        try
        {
            DWORD t0 = GetTickCount();
            StreamSource src(stream);
            std::vector<BYTE> moov;
            std::vector<Track> tracks;
            Tracks(src, moov, tracks);
            bool zraw = false;
            for (auto& t : tracks)
                if (t.zraw)
                {
                    zraw = true;
                    FrameInfo fi;
                    bmp = RenderZraw(src, t, 1920, &fi);   // 1920 = one pixel per RGGB quad of a 4K frame
                    if (!bmp) bmp = RenderHevc(src, 1920, &fi);
                    wchar_t buf[200];
                    StringCchPrintfW(buf, ARRAYSIZE(buf), L"ZRAW  ·  %d × %d  ·  %d-bit %s  ·  frame %u of %u",
                                     fi.width, fi.height, fi.bits, fi.hevc ? L"HEVC" : L"raw", fi.index + 1, fi.count);
                    caption = buf;
                    break;
                }
            if (!zraw) Delegate(stream, 1920, &bmp);
            Log(L"DoPreview %s -> %s in %lu ms", zraw ? L"ZRAW" : L"other", bmp ? L"ok" : L"failed", GetTickCount() - t0);
        }
        catch (...) { if (bmp) { DeleteObject(bmp); bmp = NULL; } }
        if (!bmp) return E_FAIL;   // Explorer then shows "No preview available"
        BITMAP bm; GetObject(bmp, sizeof(bm), &bm); bw = bm.bmWidth; bh = bm.bmHeight;

        WNDCLASSW wc = {};
        wc.lpfnWndProc = WndProc; wc.hInstance = g_mod; wc.lpszClassName = L"ZrawTaggerPreview"; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassW(&wc);   // fails harmlessly when already registered
        wnd = CreateWindowExW(0, wc.lpszClassName, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                              rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, parent, NULL, g_mod, this);
        return wnd ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }

    IFACEMETHODIMP Unload()
    {
        if (wnd) { DestroyWindow(wnd); wnd = NULL; }
        if (bmp) { DeleteObject(bmp); bmp = NULL; }
        if (stream) { stream->Release(); stream = NULL; }
        caption.clear();
        return S_OK;
    }

    IFACEMETHODIMP SetFocus() { if (wnd) ::SetFocus(wnd); return S_OK; }
    IFACEMETHODIMP QueryFocus(HWND* h) { *h = GetFocus(); return *h ? S_OK : HRESULT_FROM_WIN32(GetLastError()); }
    IFACEMETHODIMP TranslateAccelerator(MSG* msg)
    {
        HRESULT hr = S_FALSE;
        IPreviewHandlerFrame* frame = NULL;
        if (site && SUCCEEDED(site->QueryInterface(IID_PPV_ARGS(&frame)))) { hr = frame->TranslateAccelerator(msg); frame->Release(); }
        return hr;
    }

    IFACEMETHODIMP SetSite(IUnknown* s) { if (site) site->Release(); site = s; if (site) site->AddRef(); return S_OK; }
    IFACEMETHODIMP GetSite(REFIID riid, void** ppv) { *ppv = NULL; return site ? site->QueryInterface(riid, ppv) : E_FAIL; }

    IFACEMETHODIMP GetWindow(HWND* h) { *h = wnd; return wnd ? S_OK : E_FAIL; }
    IFACEMETHODIMP ContextSensitiveHelp(BOOL) { return E_NOTIMPL; }

    IFACEMETHODIMP SetBackgroundColor(COLORREF c) { bg = c; Redraw(); return S_OK; }
    IFACEMETHODIMP SetFont(const LOGFONTW* lf) { if (font) DeleteObject(font); font = lf ? CreateFontIndirectW(lf) : NULL; Redraw(); return S_OK; }
    IFACEMETHODIMP SetTextColor(COLORREF c) { fg = c; Redraw(); return S_OK; }
};

template <class T> class Factory : public IClassFactory
{
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (riid == IID_IUnknown || riid == IID_IClassFactory) { *ppv = this; return S_OK; }
        *ppv = NULL; return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() { return 2; }   // static object
    IFACEMETHODIMP_(ULONG) Release() { return 1; }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv)
    {
        *ppv = NULL;
        if (outer) return CLASS_E_NOAGGREGATION;
        T* h = new (std::nothrow) T();
        if (!h) return E_OUTOFMEMORY;
        HRESULT hr = h->QueryInterface(riid, ppv);
        h->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) { if (lock) InterlockedIncrement(&g_objects); else InterlockedDecrement(&g_objects); return S_OK; }
};
static Factory<IconHandler> g_iconFactory;
static Factory<ThumbHandler> g_thumbFactory;
static Factory<PreviewHandler> g_previewFactory;

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv)
{
    if (clsid == CLSID_ZrawIcon) return g_iconFactory.QueryInterface(riid, ppv);
    if (clsid == CLSID_ZrawThumb) return g_thumbFactory.QueryInterface(riid, ppv);
    if (clsid == CLSID_ZrawPreview) return g_previewFactory.QueryInterface(riid, ppv);
    *ppv = NULL;
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow() { return g_objects ? S_FALSE : S_OK; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_mod = h; DisableThreadLibraryCalls(h);
#ifdef ZRAWICON_LOG
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH); Log(L"loaded into %s (pid %lu)", exe, GetCurrentProcessId());
#endif
    }
    return TRUE;
}

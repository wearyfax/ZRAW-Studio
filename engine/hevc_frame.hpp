// One HEVC ZRAW frame as an RGB picture, decoded by Windows' Media Foundation through the HEVC Video
// Extension (the one behind Windows' H.265 thumbnails). Media Foundation reads the plain QuickTime
// file HevcZraw presents through a small IStream, seeks to the keyframe before the frame, decodes up
// to it, then converts to RGB and scales with its video processor. zrawcore.dll and the Explorer
// add-on use it for thumbnails; it needs no libmpv.
#pragma once
#include <string>
#include <vector>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include "hevczraw.hpp"
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace hevc_frame_detail
{
    // Read-only IStream over HevcZraw's plain file
    class VirtualStream : public IStream
    {
        LONG refs_ = 1;
        HevcZraw& z_;
        uint64_t pos_ = 0;
    public:
        explicit VirtualStream(HevcZraw& z) : z_(z) {}
        IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv)
        {
            if (riid == IID_IUnknown || riid == IID_IStream || riid == IID_ISequentialStream) { *ppv = static_cast<IStream*>(this); AddRef(); return S_OK; }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        IFACEMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&refs_); }
        IFACEMETHODIMP_(ULONG) Release() { LONG r = InterlockedDecrement(&refs_); if (!r) delete this; return r; }
        IFACEMETHODIMP Read(void* buf, ULONG n, ULONG* got)
        {
            ULONG done = 0;
            while (done < n)
            {
                size_t k = z_.Read(pos_, (uint8_t*)buf + done, n - done);
                if (!k) break;
                done += (ULONG)k;
                pos_ += k;
            }
            if (got) *got = done;
            return done == n ? S_OK : S_FALSE;
        }
        IFACEMETHODIMP Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* now)
        {
            int64_t base = origin == STREAM_SEEK_SET ? 0 : origin == STREAM_SEEK_CUR ? (int64_t)pos_ : (int64_t)z_.Size();
            int64_t p = base + move.QuadPart;
            if (p < 0) return STG_E_INVALIDFUNCTION;
            pos_ = (uint64_t)p;
            if (now) now->QuadPart = pos_;
            return S_OK;
        }
        IFACEMETHODIMP Stat(STATSTG* st, DWORD)
        {
            *st = {};
            st->type = STGTY_STREAM;
            st->cbSize.QuadPart = z_.Size();
            st->grfMode = STGM_READ;
            return S_OK;
        }
        IFACEMETHODIMP Write(const void*, ULONG, ULONG*) { return STG_E_ACCESSDENIED; }
        IFACEMETHODIMP SetSize(ULARGE_INTEGER) { return STG_E_ACCESSDENIED; }
        IFACEMETHODIMP CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) { return E_NOTIMPL; }
        IFACEMETHODIMP Commit(DWORD) { return S_OK; }
        IFACEMETHODIMP Revert() { return E_NOTIMPL; }
        IFACEMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) { return STG_E_INVALIDFUNCTION; }
        IFACEMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) { return STG_E_INVALIDFUNCTION; }
        IFACEMETHODIMP Clone(IStream**) { return E_NOTIMPL; }
    };

    template <class T> struct Ptr   // just enough of a COM smart pointer
    {
        T* p = nullptr;
        ~Ptr() { if (p) p->Release(); }
        T** operator&() { return &p; }
        T* operator->() { return p; }
        operator T*() { return p; }
    };
}

// Decodes `frame` and writes it as top-down 8-bit RGB (3 bytes per pixel, out_w * 3 per row), scaled
// to out_w x out_h. Any thread; initializes COM and Media Foundation for the call.
inline bool RenderHevcZrawFrame(HevcZraw& z, int frame, int out_w, int out_h, uint8_t* rgb, std::string& err)
{
    using namespace hevc_frame_detail;
    if (frame < 0 || frame >= z.frames || z.fps <= 0) { err = "frame out of range"; return false; }
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // RPC_E_CHANGED_MODE on an STA thread is fine
    // Started once and left running: MFShutdown costs about 0.3 s per call
    static const HRESULT started = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(started)) { if (SUCCEEDED(co)) CoUninitialize(); err = "Media Foundation is not available"; return false; }
    bool ok = false;
    HRESULT hr;
    {
        Ptr<IStream> stream;
        stream.p = new VirtualStream(z);
        Ptr<IMFByteStream> bytes;
        Ptr<IMFAttributes> battr, attr;
        Ptr<IMFSourceReader> reader;
        Ptr<IMFMediaType> type;
        const DWORD video = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
        if (FAILED(hr = MFCreateMFByteStreamOnStream(stream, &bytes))) { err = "MFCreateMFByteStreamOnStream failed"; goto done; }
        if (SUCCEEDED(bytes->QueryInterface(IID_PPV_ARGS(&battr)))) battr->SetString(MF_BYTESTREAM_CONTENT_TYPE, L"video/quicktime");
        MFCreateAttributes(&attr, 1);
        attr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);   // YUV 10-bit to RGB32, and scaling
        if (FAILED(hr = MFCreateSourceReaderFromByteStream(bytes, attr, &reader)))
        {
            err = "Windows cannot open the clip (is the HEVC Video Extension installed?)";
            goto done;
        }
        reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader->SetStreamSelection(video, TRUE);
        MFCreateMediaType(&type);
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        MFSetAttributeSize(type, MF_MT_FRAME_SIZE, (UINT32)out_w, (UINT32)out_h);
        if (FAILED(hr = reader->SetCurrentMediaType(video, nullptr, type))) { err = "Windows has no HEVC decoder for this clip (is the HEVC Video Extension installed?)"; goto done; }

        // Frame n starts at n / fps; seeking lands on the keyframe before it, then decode forward
        const LONGLONG target = (LONGLONG)(frame / z.fps * 1e7 + 0.5), half = (LONGLONG)(0.5e7 / z.fps);
        PROPVARIANT at;
        PropVariantInit(&at);
        at.vt = VT_I8;
        at.hVal.QuadPart = target;
        reader->SetCurrentPosition(GUID_NULL, at);
        for (int guard = 0; guard < 4 * 300; ++guard)
        {
            DWORD flags = 0;
            LONGLONG t = 0;
            Ptr<IMFSample> sample;
            if (FAILED(hr = reader->ReadSample(video, 0, nullptr, &flags, &t, &sample))) { err = "HEVC decoding failed"; break; }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { err = "reached the end before the frame"; break; }
            if (!sample || t + half < target) continue;
            Ptr<IMFMediaBuffer> buf;
            if (FAILED(sample->ConvertToContiguousBuffer(&buf))) { err = "no picture"; break; }
            Ptr<IMF2DBuffer> b2;
            BYTE* line0 = nullptr;
            LONG pitch = 0;
            BYTE* raw = nullptr;
            DWORD len = 0;
            bool locked2d = SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&line0, &pitch));
            if (!locked2d)
            {
                if (FAILED(buf->Lock(&raw, nullptr, &len)) || len < (DWORD)out_w * 4 * out_h) { err = "short picture"; break; }
                line0 = raw;
                pitch = out_w * 4;   // assumes the default top-down stride when there is no 2D buffer
            }
            for (int y = 0; y < out_h; ++y)
            {
                const BYTE* s = line0 + (ptrdiff_t)pitch * y;
                uint8_t* d = rgb + (size_t)y * out_w * 3;
                for (int x = 0; x < out_w; ++x) { d[3 * x] = s[4 * x + 2]; d[3 * x + 1] = s[4 * x + 1]; d[3 * x + 2] = s[4 * x]; }
            }
            if (locked2d) b2->Unlock2D(); else buf->Unlock();
            ok = true;
            break;
        }
    }
done:
    if (SUCCEEDED(co)) CoUninitialize();
    return ok;
}

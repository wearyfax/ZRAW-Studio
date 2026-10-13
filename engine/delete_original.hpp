// EXPERIMENTAL: removes a source clip after it has been converted (zrawdng convert --delete-originals).
// Not tested against real footage. Every check below must pass, or the clip is kept.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")

namespace delete_original_detail
{
    namespace fs = std::filesystem;

    // Flushes a file's cached writes to the drive, so a crash or an unplugged drive cannot lose them.
    inline bool flush_to_disk(const fs::path& p)
    {
        HANDLE h = CreateFileW(p.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        bool ok = FlushFileBuffers(h) != 0;
        CloseHandle(h);
        return ok;
    }

    // Recycle Bin delete. FOF_WANTNUKEWARNING makes Windows ask before permanently deleting a file
    // that is too large for the Recycle Bin, instead of silently destroying it.
    inline bool recycle(const fs::path& p)
    {
        std::wstring from = p.wstring();
        from.push_back(L'\0');   // pFrom is a double-null-terminated list
        SHFILEOPSTRUCTW op = {};
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOF_WANTNUKEWARNING;
        return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
    }
}

// Deletes `clip` only if its conversion is complete and on disk:
//   - the clip still has the size it had before conversion (nothing was appending to it),
//   - every DNG exists with exactly the byte count zrawdng wrote (written[i]), flushed to the drive,
//   - the audio/timecode sidecar exists and is flushed.
// Without `permanent`, the clip goes to the Recycle Bin; drives without one (SD cards, card readers,
// network shares) are refused rather than silently deleted.
inline bool DeleteOriginalAfterConversion(const std::filesystem::path& clip, const std::filesystem::path& sidecar, const std::vector<std::filesystem::path>& dngs,
                                          const std::vector<uint64_t>& written, uint64_t clip_size_before,
                                          bool permanent, std::string& msg)
{
    using namespace delete_original_detail;
    std::error_code ec;
    const fs::path src = fs::absolute(clip, ec);

    uint64_t now = fs::file_size(src, ec);
    if (ec || now != clip_size_before) { msg = "source clip changed size during conversion"; return false; }

    if (dngs.empty() || dngs.size() != written.size()) { msg = "no frames to check"; return false; }
    for (size_t i = 0; i < dngs.size(); ++i)
    {
        uint64_t size = fs::file_size(dngs[i], ec);
        if (ec || written[i] == 0 || size != written[i]) { msg = "output missing or wrong size: " + dngs[i].filename().string(); return false; }
        if (!flush_to_disk(dngs[i])) { msg = "could not flush " + dngs[i].filename().string() + " to disk"; return false; }
    }
    if (fs::file_size(sidecar, ec) == 0 || ec || !flush_to_disk(sidecar)) { msg = "audio/timecode sidecar missing or not flushed"; return false; }

    if (permanent)
    {
        if (!DeleteFileW(src.wstring().c_str())) { msg = "delete failed (error " + std::to_string(GetLastError()) + ")"; return false; }
    }
    else
    {
        std::wstring root = src.root_path().wstring();
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) { msg = "this drive has no Recycle Bin; kept the original (add --permanent to delete it anyway)"; return false; }
        if (!recycle(src)) { msg = "Recycle Bin delete failed or was canceled"; return false; }
    }
    if (fs::exists(src, ec)) { msg = "original still exists after delete"; return false; }
    msg = permanent ? "deleted original permanently" : "moved original to the Recycle Bin";
    return true;
}

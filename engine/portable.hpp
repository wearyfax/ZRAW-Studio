// The MSVC intrinsics and positional file reads the engine uses, for the Linux (GCC) build. With MSVC
// this only includes <intrin.h>; the Windows code keeps its own Win32 file calls.
#pragma once
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
inline unsigned char _BitScanReverse(unsigned long* index, unsigned long mask) { *index = 63 - __builtin_clzl(mask); return mask != 0; }
inline unsigned __lzcnt(unsigned x) { return x ? __builtin_clz(x) : 32; }
#define _tzcnt_u32 __builtin_ctz      // the callers never pass 0
#define _tzcnt_u64 __builtin_ctzll
#define _byteswap_ulong __builtin_bswap32
#endif

#ifndef _WIN32
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// A file opened for reading at any offset (pread), safe to share between threads
class PosixFile
{
    int fd_ = -1;
public:
    explicit PosixFile(const std::filesystem::path& p) : fd_(::open(p.c_str(), O_RDONLY | O_CLOEXEC)) {}
    PosixFile(const PosixFile&) = delete;
    PosixFile& operator=(const PosixFile&) = delete;
    ~PosixFile() { if (fd_ >= 0) ::close(fd_); }
    bool ok() const { return fd_ >= 0; }
    uint64_t size() const { struct stat st; return fd_ >= 0 && fstat(fd_, &st) == 0 ? (uint64_t)st.st_size : 0; }
    void sequential() const { if (fd_ >= 0) posix_fadvise(fd_, 0, 0, POSIX_FADV_SEQUENTIAL); }   // more read-ahead
    // Exactly n bytes at `offset`, or false
    bool read_at(uint64_t offset, void* buf, size_t n) const
    {
        for (size_t got = 0; got < n;)
        {
            const ssize_t r = pread(fd_, (char*)buf + got, n - got, (off_t)(offset + got));
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return false;
            got += (size_t)r;
        }
        return true;
    }
};
#endif

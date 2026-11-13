// SPDX-License-Identifier: MIT
// See raw_metallib.h.
#include "raw_metallib.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include "MetallibSlice.h"

namespace raw {
namespace {

constexpr size_t kHeaderBytes = 0x58, kFunctionListAt = 24;

template <class T> bool readAt(const uint8_t *file, size_t size, size_t at, T &v)
{
    if (at > size || sizeof v > size - at)
        return false;
    memcpy(&v, file + at, sizeof v);
    return true;
}

// One function entry's tag list in [at, end): fourcc, u16 length, bytes, up to ENDT
bool entry(const uint8_t *file, size_t at, size_t end, std::map<std::string, AirHash> &out)
{
    std::string name;
    AirHash hash{};
    bool hashed = false;
    while (at + 4 <= end && memcmp(file + at, "ENDT", 4)) {
        uint16_t length;
        if (!readAt(file, end, at + 4, length) || length > end - at - 6)
            return false;
        const uint8_t *data = file + at + 6;
        if (!memcmp(file + at, "NAME", 4))
            name.assign(reinterpret_cast<const char *>(data), strnlen(reinterpret_cast<const char *>(data), length));
        else if (!memcmp(file + at, "HASH", 4) && length == hash.size()) {
            memcpy(hash.data(), data, hash.size());
            hashed = true;
        }
        at += 6 + length;
    }
    if (at + 4 > end || name.empty() || !hashed)
        return false;
    out[name] = hash;
    return true;
}

}  // namespace

// The slice the IPSW's Metal loads (MetallibSlice.h), which the frontend hands the runtime and the pack's tools key by
bool metallibAirHashes(const uint8_t *whole, size_t wholeBytes, std::map<std::string, AirHash> &out)
{
    out.clear();
    size_t sliceAt = 0, size = 0;
    if (!switch_metal::airSlice(whole, wholeBytes, sliceAt, size))
        return false;
    const uint8_t *file = whole + sliceAt;
    uint64_t listAt = 0, listBytes = 0;
    uint32_t count = 0;
    if (size < kHeaderBytes || memcmp(file, "MTLB", 4) || !readAt(file, size, kFunctionListAt, listAt) ||
        !readAt(file, size, kFunctionListAt + 8, listBytes) || !readAt(file, size, size_t(listAt), count) ||
        listAt > size || listBytes > size - listAt - 4)
        return false;
    const size_t end = size_t(listAt + 4 + listBytes);
    size_t at = size_t(listAt + 4);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t bytes;
        if (!readAt(file, end, at, bytes) || bytes < 8 || bytes > end - at || !entry(file, at + 4, at + bytes, out)) {
            out.clear();
            return false;
        }
        at += bytes;
    }
    return true;
}

// Mapped, not read: only the pages of the fat header, the slice's header and its function list are touched
bool metallibAirHashes(const std::string &path, std::map<std::string, AirHash> &out)
{
    out.clear();
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat st;
    void *mapped = fstat(fd, &st) == 0 && st.st_size > 0
                       ? mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0)
                       : MAP_FAILED;
    close(fd);
    if (mapped == MAP_FAILED)
        return false;
    const bool ok = metallibAirHashes(static_cast<const uint8_t *>(mapped), size_t(st.st_size), out);
    munmap(mapped, size_t(st.st_size));
    return ok;
}

}  // namespace raw

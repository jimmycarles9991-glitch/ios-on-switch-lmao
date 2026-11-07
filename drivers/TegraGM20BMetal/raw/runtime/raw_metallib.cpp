// SPDX-License-Identifier: MIT
// See raw_metallib.h.
#include "raw_metallib.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace raw {
namespace {

constexpr size_t kHeaderBytes = 0x58, kFunctionListAt = 24;
constexpr uint32_t kUniversal = 0xCAFEBABE, kAir64 = 0x01000017;
constexpr uint32_t kMostSlices = 16;

uint32_t bigEndian(const uint8_t *w)
{
    return uint32_t(w[0]) << 24 | uint32_t(w[1]) << 16 | uint32_t(w[2]) << 8 | w[3];
}

// A universal metallib's AIR64 slice, as Metal picks it (the frameworks ship one beside an Apple GPU binary): [*offset, + *size)
// of the file; `head` holds its first 8 + 20 x slices bytes. A thin metallib is its own slice.
bool airSlice(const uint8_t *head, size_t headBytes, uint64_t fileBytes, uint64_t &offset, uint64_t &size)
{
    if (headBytes < 8 || bigEndian(head) != kUniversal) {
        offset = 0;
        size = fileBytes;
        return true;
    }
    const uint32_t count = bigEndian(head + 4);
    for (uint32_t i = 0; i < count && i < kMostSlices && 8 + 20 * (i + 1) <= headBytes; i++)
        if (bigEndian(head + 8 + 20 * i) == kAir64) {
            offset = bigEndian(head + 8 + 20 * i + 8);
            size = bigEndian(head + 8 + 20 * i + 12);
            return offset <= fileBytes && size <= fileBytes - offset;
        }
    return false;
}

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

bool metallibAirHashes(const uint8_t *whole, size_t wholeBytes, std::map<std::string, AirHash> &out)
{
    out.clear();
    uint64_t sliceAt = 0, sliceBytes = 0;
    if (!airSlice(whole, std::min<size_t>(wholeBytes, 8 + 20 * kMostSlices), wholeBytes, sliceAt, sliceBytes))
        return false;
    const uint8_t *file = whole + sliceAt;
    const size_t size = size_t(sliceBytes);
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

bool metallibAirHashes(const std::string &path, std::map<std::string, AirHash> &out)
{
    out.clear();
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat st;
    uint8_t head[8 + 20 * kMostSlices] = {}, header[kHeaderBytes];
    uint64_t sliceAt = 0, sliceBytes = 0, listAt = 0, listBytes = 0;
    bool ok = fstat(fd, &st) == 0;
    const ssize_t headBytes = ok ? pread(fd, head, sizeof head, 0) : -1;
    ok = headBytes > 0 && airSlice(head, size_t(headBytes), uint64_t(st.st_size), sliceAt, sliceBytes) &&
         pread(fd, header, sizeof header, off_t(sliceAt)) == ssize_t(sizeof header) && !memcmp(header, "MTLB", 4);
    if (ok) {
        memcpy(&listAt, header + kFunctionListAt, 8);
        memcpy(&listBytes, header + kFunctionListAt + 8, 8);
        ok = listAt >= kHeaderBytes && listAt < sliceBytes && listBytes <= sliceBytes - listAt - 4;
    }
    std::vector<uint8_t> slice;
    if (ok) {
        // the header and the function list, at their offsets in the slice (the parser takes the slice's offsets)
        slice.resize(size_t(listAt + 4 + listBytes));
        memcpy(slice.data(), header, sizeof header);
        const size_t want = size_t(4 + listBytes);
        ok = pread(fd, slice.data() + listAt, want, off_t(sliceAt + listAt)) == ssize_t(want);
    }
    close(fd);
    return ok && metallibAirHashes(slice.data(), slice.size(), out);
}

}  // namespace raw

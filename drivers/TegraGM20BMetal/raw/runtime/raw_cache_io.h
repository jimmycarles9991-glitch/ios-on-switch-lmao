// SPDX-License-Identifier: MIT
// The record cache's entry bytes (raw_cache.h): little-endian values, counted texts and lists, and every entry's head (its kind
// and full key) and FNV-1a tail, read back with a bounds check on each part.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "raw_cache.h"

namespace raw {

constexpr char kEntryMagic[8] = {'S', 'W', 'R', 'A', 'W', 'R', 'E', 'C'};
constexpr uint32_t kEntryFormat = 17;
constexpr uint32_t kKindRecord = 1, kKindRefusal = 2, kKindKernel = 3, kKindKernelRefusal = 4;

class Writer {
public:
    std::string out;

    void bytes(const void *p, size_t n)
    {
        out.append(static_cast<const char *>(p), n);
    }

    template <class T> void value(T v)
    {
        bytes(&v, sizeof v);
    }

    void text(const std::string &s)
    {
        value(uint32_t(s.size()));
        bytes(s.data(), s.size());
    }

    template <class T> void list(const std::vector<T> &v)
    {
        value(uint32_t(v.size()));
        bytes(v.data(), v.size() * sizeof(T));
    }
};

class Reader {
public:
    explicit Reader(const std::string &in) : in_(in)
    {}

    bool bytes(void *p, size_t n)
    {
        if (n > in_.size() - at_)
            return ok_ = false;
        memcpy(p, in_.data() + at_, n);
        at_ += n;
        return true;
    }

    template <class T> T value()
    {
        T v{};
        bytes(&v, sizeof v);
        return v;
    }

    std::string text()
    {
        const uint32_t n = value<uint32_t>();
        if (!ok_ || n > in_.size() - at_)
            return ok_ = false, std::string();
        std::string s = in_.substr(at_, n);
        at_ += n;
        return s;
    }

    template <class T> std::vector<T> list()
    {
        const uint32_t n = value<uint32_t>();
        if (!ok_ || n > (in_.size() - at_) / sizeof(T))
            return ok_ = false, std::vector<T>();
        std::vector<T> v(n);
        bytes(v.data(), n * sizeof(T));
        return v;
    }

    bool ok() const
    {
        return ok_;
    }

    size_t at() const
    {
        return at_;
    }

private:
    const std::string &in_;
    size_t at_ = 0;
    bool ok_ = true;
};

void writeEntryHead(Writer &w, const CacheKey &k, uint32_t kind);
// False: not an entry of `k` (its tail's sum, its head or its key differ)
bool readEntryHead(const std::string &bytes, Reader &r, const CacheKey &k, uint32_t &kind);

}  // namespace raw

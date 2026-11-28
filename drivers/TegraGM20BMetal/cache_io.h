// SPDX-License-Identifier: MIT
// The record cache's entry bytes (gm20b_cache.h): little-endian values, counted texts and lists, and every entry's head (its kind
// and full key), read back with a bounds check on each part. An entry ends where its last part does: its writer published
// it whole (a file renamed in, or a store entry below its durable end), so nothing checks its bytes again.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include "gm20b_cache.h"

namespace gm20b {

constexpr char kEntryMagic[8] = {'S', 'W', 'R', 'A', 'W', 'R', 'E', 'C'};
constexpr uint32_t kEntryFormat = 20;
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
    explicit Reader(std::string_view in) : in_(in)
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

    std::string_view textView()
    {
        const uint32_t n = value<uint32_t>();
        if (!ok_ || n > in_.size() - at_)
            return ok_ = false, std::string_view();
        const std::string_view s = in_.substr(at_, n);
        at_ += n;
        return s;
    }

    std::string text()
    {
        return std::string(textView());
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
    std::string_view in_;
    size_t at_ = 0;
    bool ok_ = true;
};

void writeEntryHead(Writer &w, const CacheKey &k, uint32_t kind);
// False: not an entry of `k` (its head or its key differ)
bool readEntryHead(std::string_view bytes, Reader &r, const CacheKey &k, uint32_t &kind);

}  // namespace gm20b

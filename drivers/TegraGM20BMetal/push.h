// SPDX-License-Identifier: MIT
// r7-raw: GM20B push-buffer writer. Method headers are NVIDIA's host FIFO format (as Mesa's nv_push.h writes them): bits 31:29 op
// (1 INC, 3 NON_INC, 4 IMMD, 5 ONE_INC), 28:16 count or immediate data, 15:13 subchannel, 11:0 method >> 2.
#pragma once
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace gm20b {

enum : uint32_t {
    SUBC_3D = 0,
    SUBC_COMPUTE = 1,
    SUBC_M2MF = 2,
    SUBC_2D = 3,
    SUBC_COPY = 4
};

struct Push {
    uint32_t *at = nullptr, *end = nullptr;

    static constexpr uint32_t hdr(uint32_t op, uint32_t subc, uint32_t mthd, uint32_t count)
    {
        return op << 29 | count << 16 | subc << 13 | mthd >> 2;
    }

    // one method, one value: immediate when it fits 13 bits
    void m(uint32_t mthd, uint32_t v, uint32_t subc = SUBC_3D)
    {
        if (v < 0x2000) {
            *at++ = hdr(4, subc, mthd, v);
        } else {
            *at++ = hdr(1, subc, mthd, 1);
            *at++ = v;
        }
    }

    // consecutive methods from mthd
    void inc(uint32_t mthd, std::initializer_list<uint32_t> v, uint32_t subc = SUBC_3D)
    {
        *at++ = hdr(1, subc, mthd, uint32_t(v.size()));
        for (uint32_t x : v)
            *at++ = x;
    }

    void incArray(uint32_t mthd, const uint32_t *v, uint32_t n, uint32_t subc = SUBC_3D)
    {
        *at++ = hdr(1, subc, mthd, n);
        memcpy(at, v, n * 4);
        at += n;
    }

    // n values to one method (LOAD_CONSTANT_BUFFER)
    void nonInc(uint32_t mthd, const uint32_t *v, uint32_t n, uint32_t subc = SUBC_3D)
    {
        *at++ = hdr(3, subc, mthd, n);
        memcpy(at, v, n * 4);
        at += n;
    }

    void raw(const uint32_t *v, uint32_t n)
    {
        memcpy(at, v, n * 4);
        at += n;
    }

    size_t room() const
    {
        return size_t(end - at);
    }
};

// The value the last write of `method` on `subc` in [words, words + count) leaves it, `absent` where none writes it
inline uint32_t lastWritten(const uint32_t *words, size_t count, uint32_t method, uint32_t absent,
                            uint32_t subc = SUBC_3D)
{
    uint32_t value = absent;
    for (size_t i = 0; i < count;) {
        const uint32_t hdr = words[i], op = hdr >> 29, arg = hdr >> 16 & 0x1fff, mthd = (hdr & 0xfff) << 2;
        const bool mine = (hdr >> 13 & 7) == subc;
        if (op == 4) {
            if (mine && mthd == method)
                value = arg;
            i += 1;
            continue;
        }
        for (uint32_t k = 0; k < arg && i + 1 + k < count; k++) {
            const uint32_t at = op == 1 ? mthd + 4 * k : op == 5 && k ? mthd + 4 : mthd;
            if (mine && at == method && (op == 1 || op == 3 || op == 5))
                value = words[i + 1 + k];
        }
        i += op == 1 || op == 3 || op == 5 ? 1 + arg : 1;
    }
    return value;
}

}  // namespace gm20b

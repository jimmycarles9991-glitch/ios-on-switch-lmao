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

}  // namespace gm20b

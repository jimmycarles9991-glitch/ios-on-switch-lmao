#include "stub_split.h"

#include "../a64x/a64x.h"

#include <stdbool.h>

int64_t a64x_branch_offset(uint32_t w, bool *is)
{
    *is = true;
    if ((w & 0x7C000000u) == 0x14000000u)
        return (int64_t)(int32_t)((w & 0x3FFFFFFu) << 6) >> 4;
    if ((w & 0x7E000000u) == 0x34000000u)
        return (int64_t)(int32_t)(((w >> 5) & 0x7FFFFu) << 13) >> 11;
    if ((w & 0xFF000010u) == 0x54000000u)
        return (int64_t)(int32_t)(((w >> 5) & 0x7FFFFu) << 13) >> 11;
    if ((w & 0x7E000000u) == 0x36000000u)
        return (int64_t)(int32_t)(((w >> 5) & 0x3FFFu) << 18) >> 16;
    *is = false;
    return 0;
}

static uint32_t with_offset(uint32_t w, int64_t off, bool *ok)
{
    *ok = true;
    if ((w & 0x7C000000u) == 0x14000000u) {
        if (off < -(1LL << 27) || off >= (1LL << 27)) {
            *ok = false;
            return 0;
        }
        return (w & 0xFC000000u) | (uint32_t)((off >> 2) & 0x3FFFFFF);
    }
    if ((w & 0x7E000000u) == 0x34000000u || (w & 0xFF000010u) == 0x54000000u) {
        if (off < -(1LL << 20) || off >= (1LL << 20)) {
            *ok = false;
            return 0;
        }
        return (w & 0xFF00001Fu) | (uint32_t)(((off >> 2) & 0x7FFFF) << 5);
    }
    if ((w & 0x7E000000u) == 0x36000000u) {
        if (off < -(1LL << 15) || off >= (1LL << 15)) {
            *ok = false;
            return 0;
        }
        return (w & 0xFFF8001Fu) | (uint32_t)(((off >> 2) & 0x3FFF) << 5);
    }
    *ok = false;
    return 0;
}

int a64x_stub_split(const uint32_t *st, int n, uint64_t laid, const uint64_t *addr, const uint64_t *links, int nlinks,
                    struct a64x_placed *out)
{
    int nout = 0, li = 0;
    for (int j = 0; j < n; j++) {
        bool is_branch, ok = true;
        int64_t off = a64x_branch_offset(st[j], &is_branch);
        uint32_t w = st[j];
        if (is_branch) {
            uint64_t target = laid + 4 * (uint64_t)j + (uint64_t)off;
            if (target >= laid && target < laid + 4 * (uint64_t)n)
                target = addr[(target - laid) / 4];
            w = with_offset(st[j], (int64_t)(target - addr[j]), &ok);
        }
        if (!ok)
            return -1;
        out[nout++] = (struct a64x_placed){addr[j], w};
        if (li < nlinks && j + 1 < n && addr[j + 1] != addr[j] + 4) {
            uint32_t b = a64x_branch(links[li], addr[j + 1]);
            if (!b)
                return -1;
            out[nout++] = (struct a64x_placed){links[li], b};
            li++;
        }
    }
    return nout;
}

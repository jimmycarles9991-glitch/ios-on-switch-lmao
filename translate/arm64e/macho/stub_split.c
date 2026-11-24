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

static bool acquire_release(uint32_t w)
{
    switch (a64x_classify(w)) {
    case A64X_LSE_LDOP:
    case A64X_LSE_SWP:
        return ((w >> 22) & 3) == 3;
    case A64X_LSE_CAS:
    case A64X_LSE_CASP:
        return ((w >> 22) & 1) && ((w >> 15) & 1);
    default:
        return false;
    }
}

static bool store_release_exclusive(uint32_t w)
{
    return (w & 0x3FE08000u) == 0x08008000u || (w & 0xBFE08000u) == 0x88208000u;
}

/* DMB or DSB over the outer-shareable, inner-shareable or full system domain, loads and stores */
static bool full_barrier(uint32_t w)
{
    uint32_t crm = (w >> 8) & 0xF;
    return (w & 0xFFFFF0DFu) == 0xD503309Fu && (crm == 0x3 || crm == 0xB || crm == 0xF);
}

bool a64x_stub_ordered(uint32_t was, uint64_t site, uint32_t now,
                       bool (*word_at)(const void *ctx, uint64_t at, uint32_t *w), const void *ctx)
{
    if (!acquire_release(was))
        return true;
    bool bl = (now & 0xFC000000u) == 0x94000000u, stored = false;
    if (!bl && (now & 0xFC000000u) != 0x14000000u)
        return false;
    uint64_t pc = site + (uint64_t)((int64_t)(int32_t)(now << 6) >> 4);
    for (int step = 0; step < 4 * A64X_STUB_MAX; step++) {
        uint32_t w;
        if (!word_at(ctx, pc, &w))
            return false;
        bool b = (w & 0xFC000000u) == 0x14000000u;
        uint64_t t = pc + (uint64_t)((int64_t)(int32_t)(w << 6) >> 4);
        if (bl ? w == 0xD65F03C0u : b && t == site + 4)
            return false;
        if (stored && full_barrier(w))
            return true;
        stored |= store_release_exclusive(w);
        pc = b ? t : pc + 4;
    }
    return false;
}

// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// The caller owns a zeroed pointer table, serializes access, and releases every
// resident page. residentLimit counts all seed and subsequently written pages;
// the separately bounded pointer table and request scratch are not included.
struct RamSparseDisk {
    uint8_t **pages;
    uint64_t bytes, resident, residentLimit;

    const uint8_t *find(uint64_t i) const
    {
        return pages && i < bytes / 4096 ? pages[i] : nullptr;
    }
};

static uint32_t ramU32(const uint8_t *p)
{
    uint32_t v = 0;
    memcpy(&v, p, 4);
    return v;
}

static uint64_t ramU64(const uint8_t *p)
{
    uint64_t v = 0;
    memcpy(&v, p, 8);
    return v;
}

static bool ramPackedLayout(const uint8_t *p, uint64_t n, uint64_t *logical)
{
    if (logical)
        *logical = 0;
    if (!logical || !p || n < 32 || n > 0x10000000 || memcmp(p, "DBRDISK1", 8) || ramU32(p + 8) != 1 ||
        ramU32(p + 12) != 4096)
        return false;
    const uint64_t size = ramU64(p + 16);
    const uint64_t count = ramU64(p + 24);
    uint64_t end = 0;
    if (!size || size > (1ULL << 40) || (size & 4095) || count > 65536 || count > (n - 32) / 16)
        return false;
    uint64_t cursor = 32 + count * 16;
    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t at = ramU64(p + 32 + i * 16);
        const uint64_t len = ramU64(p + 40 + i * 16);
        if (!len || ((at | len) & 4095) || at < end || at > size || len > size - at || len > n - cursor)
            return false;
        end = at + len;
        cursor += len;
    }
    if (cursor != n)
        return false;
    *logical = size;
    return true;
}

static bool ramSparseRoom(const RamSparseDisk &d, uint64_t pages)
{
    return d.resident <= d.residentLimit && pages <= (d.residentLimit - d.resident) / 4096;
}

template <class Ops> static bool ramSparseImport(RamSparseDisk &d, const uint8_t *p, uint64_t n, Ops &ops)
{
    uint64_t size = 0;
    if (!d.pages || !ramPackedLayout(p, n, &size) || d.bytes != size || d.resident)
        return false;
    const uint64_t count = ramU64(p + 24);
    uint64_t cursor = 32 + count * 16;
    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t at = ramU64(p + 32 + i * 16);
        const uint64_t len = ramU64(p + 40 + i * 16);
        for (uint64_t j = 0; j < len; j += 4096) {
            const uint8_t *src = p + cursor + j;
            uint8_t any = 0;
            for (unsigned k = 0; k < 4096; ++k)
                any |= src[k];
            if (!any)
                continue;
            if (!ramSparseRoom(d, 1))
                return false;
            uint8_t *page = ops.allocate();
            if (!page)
                return false;
            memcpy(page, src, 4096);
            d.pages[(at + j) / 4096] = page;
            d.resident += 4096;
        }
        cursor += len;
    }
    return true;
}

// Short read copies report a truthful prefix. This compatibility helper also
// supports bounded prefix writes; the native driver uses the atomic helper below.
template <class Ops>
static uint64_t ramSparseIO(RamSparseDisk &d, bool write, uint64_t offset, uint64_t length, Ops &ops, bool &noMemory)
{
    static const uint8_t zero[4096] = {};
    noMemory = false;
    if (!d.pages || !length || offset > d.bytes || length > d.bytes - offset)
        return 0;
    uint64_t done = 0;
    while (done < length) {
        const uint64_t at = offset + done;
        const uint64_t index = at / 4096;
        const uint64_t skip = at & 4095;
        uint64_t n = 4096 - skip;
        if (n > length - done)
            n = length - done;
        uint8_t *page = d.pages[index];
        if (write && !page) {
            if (!ramSparseRoom(d, 1)) {
                noMemory = true;
                break;
            }
            page = ops.allocate();
            if (!page) {
                noMemory = true;
                break;
            }
            d.pages[index] = page;
            d.resident += 4096;
        }
        const uint64_t actual = write ? ops.read(done, page + skip, n) : ops.copy(done, (page ? page : zero) + skip, n);
        if (actual > n)
            break;
        done += actual;
        if (actual != n)
            break;
    }
    return done;
}

// Commit a complete <=64 KiB request already copied into private scratch. All
// new pages are obtained before changing any disk byte. OOM/cap failure frees
// temporary pages and leaves the disk exactly unchanged. No rollback allocation.
template <class Ops>
static bool ramSparseWriteAtomic(RamSparseDisk &d, uint64_t offset, const uint8_t *input, uint64_t length, Ops &ops)
{
    if (!d.pages || !input || !length || length > 65536 || offset > d.bytes || length > d.bytes - offset)
        return false;
    const uint64_t first = offset / 4096;
    const uint64_t last = (offset + length - 1) / 4096;
    uint64_t missing = 0;
    for (uint64_t i = first; i <= last; ++i)
        missing += d.pages[i] == nullptr;
    if (!ramSparseRoom(d, missing))
        return false;
    // NOLINTNEXTLINE(misc-const-correctness): the pages become d.pages' writable storage (a template: the check sees no use)
    uint8_t *fresh[17] = {};
    uint64_t indices[17] = {};
    uint64_t count = 0;
    for (uint64_t i = first; i <= last; ++i) {
        if (!d.pages[i]) {
            uint8_t *page = ops.allocate();
            if (!page) {
                for (uint64_t j = 0; j < count; ++j)
                    ops.release(fresh[j]);
                return false;
            }
            fresh[count] = page;
            indices[count++] = i;
        }
    }
    for (uint64_t j = 0; j < count; ++j)
        d.pages[indices[j]] = fresh[j];
    d.resident += count * 4096;
    uint64_t done = 0;
    while (done < length) {
        const uint64_t at = offset + done;
        uint64_t n = 4096 - (at & 4095);
        if (n > length - done)
            n = length - done;
        memcpy(d.pages[at / 4096] + (at & 4095), input + done, n);
        done += n;
    }
    return true;
}

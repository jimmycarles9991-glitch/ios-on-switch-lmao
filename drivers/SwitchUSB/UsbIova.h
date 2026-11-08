// SPDX-License-Identifier: BSD-2-Clause-Patent
// The XUSB host's SMMU address window (UsbSmmuMapper.cpp): 4 KiB IOVA pages handed out first-fit from a rover, so a
// steady stream of transfers reuses the window front to back instead of searching from its start each time. No IOKit:
// test/usb_iova_test.cpp drives the same code.
#pragma once
#include <stdint.h>

struct UsbIovaWindow {
    static constexpr uint32_t kPage = 4096;
    uint8_t *used = nullptr;  // one byte per page
    uint32_t pages = 0, rover = 0, free = 0;
    uint64_t base = 0;

    void init(uint8_t *map, uint32_t count, uint64_t iova)
    {
        used = map;
        pages = free = count;
        rover = 0;
        base = iova;
        for (uint32_t i = 0; i < count; ++i)
            used[i] = 0;
    }

    // count pages, the first `alignPages`-aligned (a power of two); the window index, or -1 when no run is free.
    int64_t take(uint32_t count, uint32_t alignPages)
    {
        if (!count || count > free)
            return -1;
        const uint32_t align = alignPages ? alignPages : 1;
        for (uint32_t pass = 0; pass < 2; ++pass) {
            uint32_t start = pass ? 0 : rover;
            const uint32_t limit = pass ? rover + count : pages;
            for (start = (start + align - 1) & ~(align - 1); start + count <= pages && start < limit;) {
                uint32_t run = 0;
                while (run < count && !used[start + run])
                    ++run;
                if (run == count) {
                    for (uint32_t i = 0; i < count; ++i)
                        used[start + i] = 1;
                    free -= count;
                    rover = start + count == pages ? 0 : start + count;
                    return start;
                }
                start = (start + run + 1 + align - 1) & ~(align - 1);
            }
        }
        return -1;
    }

    void give(uint32_t index, uint32_t count)
    {
        for (uint32_t i = 0; i < count && index + i < pages; ++i) {
            if (used[index + i]) {
                used[index + i] = 0;
                ++free;
            }
        }
    }

    uint64_t iova(uint32_t index) const
    {
        return base + static_cast<uint64_t>(index) * kPage;
    }

    bool contains(uint64_t address) const
    {
        return address >= base && address < base + static_cast<uint64_t>(pages) * kPage;
    }

    uint32_t index(uint64_t address) const
    {
        return static_cast<uint32_t>((address - base) / kPage);
    }

    // Pages a mapping of `length` bytes needs when its first byte sits `offset` bytes into a page.
    static uint32_t pagesFor(uint64_t offset, uint64_t length)
    {
        return static_cast<uint32_t>(((offset & (kPage - 1)) + length + kPage - 1) / kPage);
    }
};

// A buffer of `length` bytes whose physical pieces `segment(done, &bytes)` gives in order (0: none) maps as consecutive
// IOVA pages when every break between pieces falls on a page boundary; firstPa is its first byte's address.
template <class Segment>
inline bool usb_iova_page_aligned(Segment segment, uint64_t length, uint64_t limit, uint64_t *firstPa)
{
    constexpr uint64_t page = UsbIovaWindow::kPage;
    *firstPa = 0;
    for (uint64_t done = 0; done < length;) {
        uint64_t bytes = 0;
        const uint64_t pa = segment(done, &bytes);
        if (!pa || !bytes || pa + bytes > limit)
            return false;
        const uint64_t take = bytes < length - done ? bytes : length - done;
        if (!done) {
            *firstPa = pa;
        } else if (pa & (page - 1)) {
            return false;
        }
        if (done + take < length && ((pa + take) & (page - 1)))
            return false;
        done += take;
    }
    return true;
}

// The pages of such a buffer in IOVA order: mapPage(index, physical page) for each of `pages`; false when the pieces
// need more pages than that or mapPage refuses.
template <class Segment, class MapPage>
inline bool usb_iova_map_pages(Segment segment, uint64_t length, uint32_t pages, MapPage mapPage, uint32_t *mapped)
{
    constexpr uint64_t page = UsbIovaWindow::kPage;
    *mapped = 0;
    for (uint64_t done = 0; done < length;) {
        uint64_t bytes = 0;
        const uint64_t pa = segment(done, &bytes);
        const uint64_t take = bytes < length - done ? bytes : length - done;
        if (!pa || !take)
            return false;
        for (uint64_t at = pa & ~(page - 1); at < pa + take; at += page) {
            if (*mapped >= pages || !mapPage(*mapped, at))
                return false;
            ++*mapped;
        }
        done += take;
    }
    return *mapped == pages;
}

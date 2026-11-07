// UsbIova.h (UsbSmmuMapper.cpp) on the host: the IOVA window never hands a page out
// twice, respects alignment and finds room after wrapping; a scattered buffer whose pieces meet on page boundaries maps
// so that every byte's IOVA translates to that byte's physical address; one whose pieces break inside a page is refused
// (UsbDmaMapper.cpp bounces it).
#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <random>
#include <vector>

#include "../UsbIova.h"

namespace {

constexpr uint64_t kPage = UsbIovaWindow::kPage;

struct Buffer {
    std::vector<std::pair<uint64_t, uint64_t>> pieces;  // physical address, bytes
    uint64_t length = 0;

    auto segment() const
    {
        return [this](uint64_t done, uint64_t *bytes) -> uint64_t {
            for (const auto &[pa, size] : pieces) {
                if (done < size) {
                    *bytes = size - done;
                    return pa + done;
                }
                done -= size;
            }
            *bytes = 0;
            return 0;
        };
    }

    uint64_t paOf(uint64_t byte) const
    {
        for (const auto &[pa, size] : pieces) {
            if (byte < size)
                return pa + byte;
            byte -= size;
        }
        return 0;
    }
};

// Pieces meet on page boundaries (aligned) or, when !aligned, one break lands inside a page.
Buffer randomBuffer(std::mt19937_64 &rng, bool aligned)
{
    Buffer b;
    const int pieces = 1 + static_cast<int>(rng() % 6);
    for (int i = 0; i < pieces; ++i) {
        const uint64_t base = 0x8000'0000ull + (rng() % 100000) * kPage;
        const uint64_t start = i == 0 ? rng() % kPage : 0;
        uint64_t end = (1 + rng() % 8) * kPage;
        if (i == pieces - 1)
            end -= rng() % kPage;
        if (end <= start)
            end = start + 1;
        b.pieces.push_back({base + start, end - start});
    }
    if (!aligned && pieces > 1)
        b.pieces[1].first += 64;
    for (const auto &piece : b.pieces)
        b.length += piece.second;
    return b;
}

TEST(UsbIova, ScatteredPagesTranslateByteForByte)
{
    std::mt19937_64 rng(5);
    for (int layout = 0; layout < 2000; ++layout) {
        const Buffer b = randomBuffer(rng, true);
        uint64_t firstPa = 0;
        ASSERT_TRUE(usb_iova_page_aligned(b.segment(), b.length, 1ull << 34, &firstPa)) << layout;
        ASSERT_EQ(firstPa, b.pieces[0].first);
        const uint32_t pages = UsbIovaWindow::pagesFor(firstPa, b.length);
        std::map<uint32_t, uint64_t> table;
        uint32_t mapped = 0;
        ASSERT_TRUE(usb_iova_map_pages(
            b.segment(), b.length, pages,
            [&](uint32_t index, uint64_t pa) {
                EXPECT_EQ(pa % kPage, 0u);
                return table.emplace(index, pa).second;
            },
            &mapped));
        ASSERT_EQ(mapped, pages);
        const uint64_t inPage = firstPa % kPage;
        for (uint64_t byte = 0; byte < b.length; byte += 1 + rng() % 997) {
            const uint64_t iova = inPage + byte;
            ASSERT_EQ(table.at(static_cast<uint32_t>(iova / kPage)) + iova % kPage, b.paOf(byte))
                << "layout " << layout << " byte " << byte;
        }
    }
}

TEST(UsbIova, BreaksInsideAPageAreRefused)
{
    std::mt19937_64 rng(7);
    for (int layout = 0; layout < 500; ++layout) {
        const Buffer b = randomBuffer(rng, false);
        uint64_t firstPa = 0;
        if (b.pieces.size() > 1)
            EXPECT_FALSE(usb_iova_page_aligned(b.segment(), b.length, 1ull << 34, &firstPa)) << layout;
    }
    Buffer high;
    high.pieces = {{(1ull << 34) - kPage, 2 * kPage}};
    high.length = 2 * kPage;
    uint64_t firstPa = 0;
    EXPECT_FALSE(usb_iova_page_aligned(high.segment(), high.length, 1ull << 34, &firstPa)) << "past the SMMU's reach";
}

TEST(UsbIova, WindowNeverHandsAPageOutTwice)
{
    std::vector<uint8_t> map(1024);
    UsbIovaWindow window;
    window.init(map.data(), 1024, 1ull << 20);
    std::vector<int> owner(1024, -1);
    std::map<int, std::pair<uint32_t, uint32_t>> live;
    std::mt19937_64 rng(11);
    int id = 0;
    for (int step = 0; step < 20000; ++step) {
        if (live.empty() || rng() % 3) {
            const uint32_t count = 1 + static_cast<uint32_t>(rng() % 40);
            const uint32_t align = 1u << (rng() % 4);
            const int64_t index = window.take(count, align);
            if (index < 0)
                continue;
            ASSERT_EQ(index % align, 0);
            for (uint32_t i = 0; i < count; ++i) {
                ASSERT_EQ(owner[index + i], -1) << "page " << index + i << " handed out twice";
                owner[index + i] = id;
            }
            live[id++] = {static_cast<uint32_t>(index), count};
        } else {
            auto it = live.begin();
            std::advance(it, static_cast<long>(rng() % live.size()));
            window.give(it->second.first, it->second.second);
            for (uint32_t i = 0; i < it->second.second; ++i)
                owner[it->second.first + i] = -1;
            live.erase(it);
        }
        uint32_t used = 0;
        for (int o : owner)
            used += o >= 0;
        ASSERT_EQ(window.free, 1024u - used);
    }
}

TEST(UsbIova, WrapsAndFillsExactly)
{
    std::vector<uint8_t> map(16);
    UsbIovaWindow window;
    window.init(map.data(), 16, 0x100000);
    EXPECT_EQ(window.take(10, 1), 0);
    EXPECT_EQ(window.take(6, 1), 10);
    EXPECT_EQ(window.take(1, 1), -1) << "full";
    window.give(0, 4);
    EXPECT_EQ(window.take(4, 1), 0) << "found after wrapping";
    window.give(4, 6);
    EXPECT_EQ(window.take(4, 4), 4);
    EXPECT_EQ(window.iova(4), 0x100000u + 4 * kPage);
    EXPECT_TRUE(window.contains(0x100000u + 15 * kPage));
    EXPECT_FALSE(window.contains(0x100000u + 16 * kPage));
    EXPECT_EQ(UsbIovaWindow::pagesFor(4095, 2), 2u);
    EXPECT_EQ(UsbIovaWindow::pagesFor(0, 4096), 1u);
}

}  // namespace

// SwitchDcache.h against a model: which lines each operation touches, with which DC, and where the barriers fall.
#include "dcache_recorder.h"

#define SWITCH_DCACHE_PHYS_MODEL 1
#include "kits/SwitchDMA/SwitchDcache.h"
#include "kits/SwitchDMA/SwitchHwPage.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <string>
#include <vector>

namespace {
struct Event {
    std::string op;
    uintptr_t address;
    bool operator==(const Event &other) const = default;
};

std::vector<Event> take()
{
    const switch_dcache_event *recorded;
    const size_t count = switch_dcache_recorded(&recorded);
    std::vector<Event> out;
    for (size_t i = 0; i < count; ++i)
        out.push_back({recorded[i].op, recorded[i].address});
    switch_dcache_forget();
    return out;
}

constexpr uintptr_t kLine = SWITCH_DCACHE_LINE;

// Every line [va, va + length) touches, with the DC the operation owes it.
std::vector<Event> expected(switch_dcache_op op, uintptr_t va, size_t length)
{
    std::vector<Event> out;
    if (!length)
        return out;
    const uintptr_t end = va + length;
    for (uintptr_t line = va & ~(kLine - 1); line < end; line += kLine) {
        const bool whole = line >= va && line + kLine <= end;
        const char *dc = op == SWITCH_DCACHE_CLEAN              ? "cvac"
                         : op == SWITCH_DCACHE_CLEAN_INVALIDATE ? "civac"
                         : whole                                ? "ivac"
                                                                : "civac";
        out.push_back({dc, line});
    }
    return out;
}

// For the invalidate form, the partial last line comes before the interior (Linux's order); compare as sets of lines.
void sortByAddress(std::vector<Event> &events)
{
    std::sort(events.begin(), events.end(), [](const Event &a, const Event &b) {
        return a.address < b.address;
    });
}

static_assert(SWITCH_HW_PAGE_SIZE == 4096 && SWITCH_HW_PAGE_MASK == 0xfff && (1u << SWITCH_HW_PAGE_SHIFT) == 4096);

TEST(SwitchDcache, EmptyRangesTouchNothing)
{
    switch_dcache_clean_lines(reinterpret_cast<void *>(0x1003), 0);
    switch_dcache_clean_invalidate_lines(reinterpret_cast<void *>(0x1003), 0);
    switch_dcache_invalidate_lines(reinterpret_cast<void *>(0x1003), 0);
    EXPECT_TRUE(take().empty());
}

TEST(SwitchDcache, PlainFormsEndWithOneBarrier)
{
    switch_dcache_clean(reinterpret_cast<void *>(0x1000), 128);
    EXPECT_EQ(take(), (std::vector<Event>{{"cvac", 0x1000}, {"cvac", 0x1040}, {"dsb", 0}}));
    switch_dcache_clean_invalidate(reinterpret_cast<void *>(0x1010), 64);
    EXPECT_EQ(take(), (std::vector<Event>{{"civac", 0x1000}, {"civac", 0x1040}, {"dsb", 0}}));
}

TEST(SwitchDcache, InvalidateKeepsNeighboursOfPartialLines)
{
    switch_dcache_invalidate(reinterpret_cast<void *>(0x1010), 0x100);
    EXPECT_EQ(
        take(),
        (std::vector<Event>{
            {"civac", 0x1000}, {"civac", 0x1100}, {"ivac", 0x1040}, {"ivac", 0x1080}, {"ivac", 0x10c0}, {"dsb", 0}}));
    switch_dcache_invalidate(reinterpret_cast<void *>(0x1008), 8);
    EXPECT_EQ(take(), (std::vector<Event>{{"civac", 0x1000}, {"dsb", 0}}));
    switch_dcache_invalidate(reinterpret_cast<void *>(0x2000), 0x80);
    EXPECT_EQ(take(), (std::vector<Event>{{"ivac", 0x2000}, {"ivac", 0x2040}, {"dsb", 0}}));
}

TEST(SwitchDcache, EveryRangeMatchesTheModel)
{
    std::mt19937_64 random(1);
    for (int i = 0; i < 20000; ++i) {
        const uintptr_t va = 0x100000 + (random() % 0x3000);
        const size_t length = random() % 700;
        for (switch_dcache_op op : {SWITCH_DCACHE_CLEAN, SWITCH_DCACHE_CLEAN_INVALIDATE, SWITCH_DCACHE_INVALIDATE}) {
            switch_dcache_lines(op, reinterpret_cast<void *>(va), length);
            std::vector<Event> got = take(), want = expected(op, va, length);
            sortByAddress(got);
            ASSERT_EQ(got, want) << "op " << op << " va " << std::hex << va << " length " << std::dec << length;
        }
    }
}

TEST(SwitchDcache, SpansShareOneBarrier)
{
    const switch_dma_span spans[] = {{0x4000, 64}, {0x8020, 64}};
    switch_dcache_spans(SWITCH_DCACHE_CLEAN, spans, 2);
    EXPECT_EQ(take(), (std::vector<Event>{{"cvac", 0x4000}, {"cvac", 0x8000}, {"cvac", 0x8040}, {"dsb", 0}}));
}
}  // namespace

// The physical aperture: VA = PA + kAperture, linear only within 16 KiB chunks, like phystokv_range's PAGE_SIZE.
constexpr uint64_t kAperture = 0xfffffe0000000000ULL, kChunk = 0x4000;
std::vector<uint64_t> gLookups;

extern "C" uint64_t phystokv_range(uint64_t pa, size_t *max_length)
{
    gLookups.push_back(pa);
    const uint64_t left = kChunk - (pa & (kChunk - 1));
    if (*max_length > left)
        *max_length = left;
    return pa + kAperture;
}

// Managed RAM in the model: below 0x90000000.
extern "C" bool switch_dma_pa_managed(uint64_t pa)
{
    return pa < 0x90000000;
}

namespace {
TEST(SwitchDcache, PhysicalRangesWalkTheApertureChunkByChunk)
{
    gLookups.clear();
    switch_dcache_phys_lines(SWITCH_DCACHE_CLEAN, 0x80003fc0, 0x80);
    EXPECT_EQ(gLookups, (std::vector<uint64_t>{0x80003fc0, 0x80004000}));
    EXPECT_EQ(take(), (std::vector<Event>{{"cvac", 0x80003fc0 + kAperture}, {"cvac", 0x80004000 + kAperture}}));

    std::mt19937_64 random(2);
    for (int i = 0; i < 2000; ++i) {
        const uint64_t pa = 0x80000000 + random() % 0x10000, length = random() % 0x9000;
        switch_dcache_phys_lines(SWITCH_DCACHE_INVALIDATE, pa, length);
        std::vector<Event> got = take(), want = expected(SWITCH_DCACHE_INVALIDATE, pa + kAperture, length);
        sortByAddress(got);
        // A chunk boundary inside a line is impossible (chunks are line multiples), so the model holds unchanged.
        ASSERT_EQ(got, want) << std::hex << pa << " " << length;
    }
}

TEST(SwitchDcache, HardwarePagesSkipUnmanagedOnesAndTheirOffsets)
{
    const uint64_t pages[] = {0x80005123, 0x90000000, 0x80001000};
    switch_dcache_hw_pages(SWITCH_DCACHE_CLEAN_INVALIDATE, pages, 3);
    std::vector<Event> got = take(), want = expected(SWITCH_DCACHE_CLEAN_INVALIDATE, 0x80005000 + kAperture, 4096);
    const std::vector<Event> second = expected(SWITCH_DCACHE_CLEAN_INVALIDATE, 0x80001000 + kAperture, 4096);
    want.insert(want.end(), second.begin(), second.end());
    EXPECT_EQ(got, want);
}
}  // namespace

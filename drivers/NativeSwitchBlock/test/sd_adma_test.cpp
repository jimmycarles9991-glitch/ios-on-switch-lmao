// Host test of drivers/NativeSwitchBlock/SwitchAdma.h: the SD driver's ADMA2 descriptor tables, built from a
// request chain's physical segments. The driver's own uses of them are guarded by sd_adma_source_test.cpp.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <random>
#include <vector>

typedef uint64_t IOByteCount;
constexpr uint32_t kIOMemoryMapperNone = 0x800;

#include "../SwitchAdma.h"
#include "bazel/testing/require.h"

// A memory descriptor as getPhysicalSegment sees it: physical runs in order (a UPL's pages, a contiguous buffer).
struct Run {
    uint64_t pa, bytes;
};

struct Memory {
    std::vector<Run> runs;
    int missingAt = -1;  // run index getPhysicalSegment pretends is not there

    uint64_t getPhysicalSegment(IOByteCount offset, IOByteCount *length, uint32_t options)
    {
        REQUIRE(options == kIOMemoryMapperNone);
        for (size_t i = 0; i < runs.size(); ++i) {
            if (offset < runs[i].bytes) {
                if ((int)i == missingAt)
                    break;
                *length = runs[i].bytes - offset;
                return runs[i].pa + offset;
            }
            offset -= runs[i].bytes;
        }
        *length = 0;
        return 0;
    }
};

static bool anyPage(uint64_t)
{
    return true;
}

struct Table {
    std::vector<SwitchAdmaDescriptor> entries;
    SwitchAdmaTable t;

    explicit Table(uint32_t capacity = 1024, uint64_t limit = 1ull << 34) : entries(capacity + 1)
    {
        t = {entries.data(), capacity, limit, 0, 0, 0, 0};
        // A guard entry past the capacity: the builder must never write it.
        entries[capacity] = {0xdead, 0xbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef};
    }

    ~Table()
    {
        const SwitchAdmaDescriptor &guard = entries[t.capacity];
        EXPECT_TRUE(guard.attributes == 0xdead && guard.length == 0xbeef && guard.reserved == 0xdeadbeef)
            << "the builder wrote the guard entry past the table's capacity";
    }
};

// Every descriptor of a finished table: a valid transfer of whole lines, 1..kAdmaMaxLength bytes (never 0: Tegra's
// SDHCI_QUIRK_BROKEN_ADMA_ZEROLEN_DESC), reserved 0, End on the last one only; and the bytes it moves are exactly
// `expected` in order (the runs as given, in 64-byte lines).
static void checkTable(const SwitchAdmaTable &t, const std::vector<Run> &expected)
{
    std::vector<uint64_t> want, got;
    for (const Run &r : expected)
        for (uint64_t at = 0; at < r.bytes; at += kAdmaLine)
            want.push_back(r.pa + at);
    REQUIRE(t.count >= 1);
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < t.count; ++i) {
        const SwitchAdmaDescriptor &d = t.entries[i];
        REQUIRE(d.attributes == (i + 1 == t.count ? 0x23 : 0x21));
        REQUIRE(d.length >= 1 && d.length <= kAdmaMaxLength && d.length % kAdmaLine == 0);
        REQUIRE(d.reserved == 0);
        const uint64_t pa = switchAdmaAddress(d);
        REQUIRE(pa % kAdmaLine == 0 && pa + d.length <= t.limit);
        REQUIRE(pa >> 32 == (pa + d.length - 1) >> 32);  // never across a 4 GiB boundary
        for (uint64_t at = 0; at < d.length; at += kAdmaLine)
            got.push_back(pa + at);
        bytes += d.length;
    }
    REQUIRE(bytes == t.bytes);
    REQUIRE(got == want);
}

static void layout()
{
    // 128 bits, little-endian: attributes, length, address low, address high, reserved (L4T sdhci_adma2_64_desc + pad).
    SwitchAdmaDescriptor d = {0x23, 0xf000, 0x89abcdc0, 0x1, 0};
    uint8_t raw[16];
    memcpy(raw, &d, sizeof(raw));
    const uint8_t expect[16] = {0x23, 0, 0x00, 0xf0, 0xc0, 0xcd, 0xab, 0x89, 1, 0, 0, 0, 0, 0, 0, 0};
    REQUIRE(!memcmp(raw, expect, sizeof(raw)));
    REQUIRE(switchAdmaAddress(d) == 0x189abcdc0ull);
}

static void singlePages()
{
    Table low;
    REQUIRE(low.t.add(0x80001000, 0x1000, anyPage) == kAdmaOk && low.t.finish());
    REQUIRE(low.t.count == 1 && low.t.entries[0].addressHigh == 0 && low.t.entries[0].addressLow == 0x80001000);
    checkTable(low.t, {{0x80001000, 0x1000}});

    // Above 4 GiB: the high word carries bit 32 (this board's RAM runs to 0x17fffffff).
    Table high;
    REQUIRE(high.t.add(0x123456000ull, 0x1000, anyPage) == kAdmaOk && high.t.finish());
    REQUIRE(high.t.entries[0].addressHigh == 1 && high.t.entries[0].addressLow == 0x23456000);
    checkTable(high.t, {{0x123456000ull, 0x1000}});

    // Sub-page, line-aligned pieces (a 512-byte sector at a 64-byte offset) are fine.
    Table piece;
    REQUIRE(piece.t.add(0x80001040, 0x200, anyPage) == kAdmaOk && piece.t.finish());
    checkTable(piece.t, {{0x80001040, 0x200}});
}

static void splitsAtTheLengthCap()
{
    // 1 MiB contiguous: 17 full 60 KiB descriptors and 4 KiB.
    Table one;
    REQUIRE(one.t.add(0x100000000ull, 0x100000, anyPage) == kAdmaOk && one.t.finish());
    REQUIRE(one.t.count == 18);
    for (uint32_t i = 0; i < 17; ++i)
        REQUIRE(one.t.entries[i].length == kAdmaMaxLength);
    REQUIRE(one.t.entries[17].length == 0x1000);
    checkTable(one.t, {{0x100000000ull, 0x100000}});

    // The same MiB as 256 physically contiguous 4 KiB segments (a UPL page by page): the same 18 descriptors.
    Memory pages;
    for (uint64_t i = 0; i < 256; ++i)
        pages.runs.push_back({0x100000000ull + i * 0x1000, 0x1000});
    Table merged;
    REQUIRE(merged.t.addMemory(&pages, 0x100000, anyPage) == kAdmaOk && merged.t.finish());
    REQUIRE(merged.t.count == 18);
    checkTable(merged.t, {{0x100000000ull, 0x100000}});

    // Exactly at the cap, one line past it, twice it.
    Table exact, past, twice;
    REQUIRE(exact.t.add(0x90000000, kAdmaMaxLength, anyPage) == kAdmaOk && exact.t.finish() && exact.t.count == 1);
    REQUIRE(past.t.add(0x90000000, kAdmaMaxLength + 64, anyPage) == kAdmaOk && past.t.finish() && past.t.count == 2);
    REQUIRE(past.t.entries[1].length == 64);
    REQUIRE(twice.t.add(0x90000000, 2 * kAdmaMaxLength, anyPage) == kAdmaOk && twice.t.finish() && twice.t.count == 2);
    checkTable(past.t, {{0x90000000, kAdmaMaxLength + 64}});

    // A run that continues the previous one tops it up to the cap, then starts a new descriptor.
    Table topUp;
    REQUIRE(topUp.t.add(0x90000000, kAdmaMaxLength - 64, anyPage) == kAdmaOk);
    REQUIRE(topUp.t.add(0x90000000 + kAdmaMaxLength - 64, 128, anyPage) == kAdmaOk && topUp.t.finish());
    REQUIRE(topUp.t.count == 2 && topUp.t.entries[0].length == kAdmaMaxLength && topUp.t.entries[1].length == 64);
    checkTable(topUp.t, {{0x90000000, kAdmaMaxLength + 64}});

    // Pages in falling physical order never merge.
    Memory falling;
    for (uint64_t i = 0; i < 8; ++i)
        falling.runs.push_back({0x90008000 - i * 0x1000, 0x1000});
    Table fall;
    REQUIRE(fall.t.addMemory(&falling, 0x8000, anyPage) == kAdmaOk && fall.t.finish() && fall.t.count == 8);
    checkTable(fall.t, falling.runs);
}

static void refusals()
{
    // Not on a cache line, at either end: a read's invalidate would hit a neighbour.
    Table a, b, c;
    REQUIRE(a.t.add(0x80001020, 0x200, anyPage) == kAdmaAlign);
    REQUIRE(b.t.add(0x80001000, 0x1f0, anyPage) == kAdmaAlign);
    REQUIRE(c.t.add(0x80001000, 0, anyPage) == kAdmaMissing);

    // The address limit: up to it is fine, past it not (a run is never split across it).
    Table edge(1024, 1ull << 32), over(1024, 1ull << 32), above(1024, 1ull << 32), wide(1024, 1ull << 34);
    REQUIRE(edge.t.add(0xfffff000ull, 0x1000, anyPage) == kAdmaOk);
    REQUIRE(over.t.add(0xfffff000ull, 0x2000, anyPage) == kAdmaHigh && over.t.count == 0);
    REQUIRE(above.t.add(0x100000000ull, 0x1000, anyPage) == kAdmaHigh);
    REQUIRE(wide.t.add(0x17ffff000ull, 0x1000, anyPage) == kAdmaOk);

    // A page that is not cacheable RAM, anywhere in a run (also a run that only touches its first or last line).
    auto notPage3 = [](uint64_t page) {
        return page != 0x100003000ull;
    };
    Table run, firstLine, lastLine, before;
    REQUIRE(run.t.add(0x100000000ull, 0x8000, notPage3) == kAdmaMemory);
    REQUIRE(firstLine.t.add(0x100002fc0ull, 0x80, notPage3) == kAdmaMemory);
    REQUIRE(lastLine.t.add(0x100003fc0ull, 0x40, notPage3) == kAdmaMemory);
    REQUIRE(before.t.add(0x100002000ull, 0x1000, notPage3) == kAdmaOk);
    std::vector<uint64_t> asked;
    Table pages;
    REQUIRE(pages.t.add(0x100000040ull, 0x2000, [&](uint64_t page) {
        asked.push_back(page);
        return true;
    }) == kAdmaOk);
    REQUIRE((asked == std::vector<uint64_t>{0x100000000ull, 0x100001000ull, 0x100002000ull}));

    // The table's capacity: exactly full is fine, one more is not (and the guard entry stays untouched).
    Memory five;
    for (uint64_t i = 0; i < 5; ++i)
        five.runs.push_back({0x90000000 + i * 0x2000, 0x1000});
    Table four(4), fits(5);
    REQUIRE(four.t.addMemory(&five, 0x5000, anyPage) == kAdmaFull && four.t.count == 4);
    REQUIRE(fits.t.addMemory(&five, 0x5000, anyPage) == kAdmaOk && fits.t.finish() && fits.t.count == 5);
    // Splitting counts too: 2 x 60 KiB + 64 bytes needs 3.
    Table two(2);
    REQUIRE(two.t.add(0x90000000, 2 * kAdmaMaxLength + 64, anyPage) == kAdmaFull && two.t.count == 2);

    // A segment the descriptor cannot produce, or a descriptor shorter than the request.
    Memory hole = five;
    hole.missingAt = 2;
    Table h, s;
    REQUIRE(h.t.addMemory(&hole, 0x5000, anyPage) == kAdmaMissing);
    Memory shortMemory;
    shortMemory.runs = {{0x90000000, 0x1000}};
    REQUIRE(s.t.addMemory(&shortMemory, 0x2000, anyPage) == kAdmaMissing);

    // Nothing added: no table to finish.
    Table empty;
    REQUIRE(!empty.t.finish());
}

static void chains()
{
    // A merged command: three requests' memory back to back. The last request's run is longer than it needs (the
    // descriptor stops at the request's length). Only the final descriptor carries End.
    Memory first, second, third;
    first.runs = {{0x123400000ull, 0x1000}, {0x80020000, 0x1000}};
    second.runs = {{0x80021000, 0x200}};  // continues the first request's last page physically: merged
    third.runs = {{0x80050000, 0x10000}};
    Table t;
    REQUIRE(t.t.addMemory(&first, 0x2000, anyPage) == kAdmaOk);
    REQUIRE(t.t.addMemory(&second, 0x200, anyPage) == kAdmaOk);
    REQUIRE(t.t.addMemory(&third, 0x1000, anyPage) == kAdmaOk && t.t.finish());
    REQUIRE(t.t.count == 3 && t.t.bytes == 0x3200);
    checkTable(t.t, {{0x123400000ull, 0x1000}, {0x80020000, 0x1200}, {0x80050000, 0x1000}});

    // A table is reused: reset() starts the next command afresh.
    t.t.reset();
    REQUIRE(t.t.count == 0 && t.t.bytes == 0);
    REQUIRE(t.t.addMemory(&third, 0x200, anyPage) == kAdmaOk && t.t.finish());
    checkTable(t.t, {{0x80050000, 0x200}});
}

static void randomRuns()
{
    // Many random requests: line-aligned runs of random lengths anywhere below the limit, often physically continuing
    // the previous one. Every table must move exactly the runs' bytes in order, within every descriptor rule.
    std::mt19937_64 random(0x5d0adaa2);
    for (unsigned round = 0; round < 3000; ++round) {
        // Starts at least 16 MiB below the limit; 40 runs of at most 256 KiB stay below it even when they all continue.
        auto anywhere = [&] {
            return (random() % ((1ull << 34) / kAdmaLine - 0x40000)) * kAdmaLine;
        };
        std::vector<Run> runs;
        uint64_t pa = anywhere();
        const unsigned count = 1 + random() % 40;
        for (unsigned i = 0; i < count; ++i) {
            if (random() % 3)
                pa = anywhere();
            const uint64_t bytes = (1 + random() % (random() % 2 ? 64 : 4096)) * kAdmaLine;
            runs.push_back({pa, bytes});
            pa += bytes;
        }
        Memory memory;
        memory.runs = runs;
        uint64_t total = 0;
        for (const Run &r : runs)
            total += r.bytes;
        Table t(4096);
        REQUIRE(t.t.addMemory(&memory, total, anyPage) == kAdmaOk && t.t.finish());
        checkTable(t.t, runs);
    }
}

// r12-sdio: the bytes a table moves at or above 4 GiB (the proof of ADMA2 up
// there keys on them).
static void highBytes()
{
    Table t;
    REQUIRE(t.t.add(0xfffff000ull, 0x1000, anyPage) == kAdmaOk && t.t.high == 0);
    // Physically continuing across 4 GiB, still two descriptors: none crosses the
    // boundary.
    REQUIRE(t.t.add(0x100000000ull, 0x1000, anyPage) == kAdmaOk && t.t.high == 0x1000);
    REQUIRE(t.t.count == 2 && t.t.finish() && switchAdmaAddress(t.t.entries[1]) == 0x100000000ull);
    checkTable(t.t, {{0xfffff000ull, 0x2000}});
    Table straddle;
    REQUIRE(straddle.t.add(0xffffff00ull, 0x200, anyPage) == kAdmaOk && straddle.t.high == 0x100);
    REQUIRE(straddle.t.count == 2 && straddle.t.entries[0].length == 0x100 && straddle.t.entries[1].length == 0x100);
    // One run across it, and one across 8 GiB: split exactly there, full-length
    // descriptors otherwise.
    Table across;
    REQUIRE(across.t.add(0xffff0000ull, 0x20000, anyPage) == kAdmaOk && across.t.finish());
    REQUIRE(across.t.count == 4 && across.t.entries[0].length == kAdmaMaxLength &&
            across.t.entries[1].length == 0x1000);
    REQUIRE(switchAdmaAddress(across.t.entries[2]) == 0x100000000ull && across.t.entries[2].length == kAdmaMaxLength);
    checkTable(across.t, {{0xffff0000ull, 0x20000}});
    Table eight;
    REQUIRE(eight.t.add(0x1fffff000ull, 0x2000, anyPage) == kAdmaOk && eight.t.count == 2 && eight.t.high == 0x2000);
    Table low;
    REQUIRE(low.t.add(0x80000000ull, 0x100000, anyPage) == kAdmaOk && low.t.high == 0);
    Memory mixed;
    mixed.runs = {{0x123400000ull, 0x1000}, {0x80020000, 0x1000}, {0x17ffff000ull, 0x1000}};
    Table m;
    REQUIRE(m.t.addMemory(&mixed, 0x3000, anyPage) == kAdmaOk && m.t.high == 0x2000 && m.t.bytes == 0x3000);
    m.t.reset();
    REQUIRE(m.t.high == 0 && m.t.bytes == 0 && m.t.count == 0);
    // A refused run adds nothing.
    Table refused(1024, 1ull << 32);
    REQUIRE(refused.t.add(0x100000000ull, 0x1000, anyPage) == kAdmaHigh && refused.t.high == 0);
}

// r12-sdio: a read chain's table with refused segments staged through the bounce buffer (SwitchAdmaStaging).
using Staging = SwitchAdmaStaging<Memory>;
constexpr uint64_t kBounce = 0x90000000ull, kBounceSize = 1 << 20;

// Every descriptor of a staged table follows checkTable's rules; the bytes it moves are the chain's, in order.
static void checkStagedTable(const SwitchAdmaTable &t, uint64_t chainBytes)
{
    REQUIRE(t.count >= 1 && t.bytes == chainBytes);
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < t.count; ++i) {
        const SwitchAdmaDescriptor &d = t.entries[i];
        REQUIRE(d.attributes == (i + 1 == t.count ? 0x23 : 0x21));
        REQUIRE(d.length >= 1 && d.length <= kAdmaMaxLength && d.length % kAdmaLine == 0 && d.reserved == 0);
        const uint64_t pa = switchAdmaAddress(d);
        REQUIRE(pa % kAdmaLine == 0 && pa + d.length <= t.limit && pa >> 32 == (pa + d.length - 1) >> 32);
        bytes += d.length;
    }
    REQUIRE(bytes == chainBytes);
}

static void stagingCases()
{
    std::vector<Staging::Stage> stages(64);
    // Nothing refused: the same table addMemory builds, no stages.
    {
        Memory a, b;
        a.runs = {{0x80001000, 0x1000}, {0x80005000, 0x1000}};
        b.runs = {{0x80006000, 0x2000}};
        Table t(1024, 1ull << 32), plain(1024, 1ull << 32);
        Staging st;
        st.reset(&t.t, kBounce, stages.data(), 64);
        REQUIRE(!st.addMemory(&a, 0x2000, anyPage) && !st.addMemory(&b, 0x2000, anyPage) && st.finish());
        REQUIRE(st.count == 0 && st.staged == 0 && t.t.count == 2);
        REQUIRE(!plain.t.addMemory(&a, 0x2000, anyPage) && !plain.t.addMemory(&b, 0x2000, anyPage) && plain.t.finish());
        checkTable(t.t, {{0x80001000, 0x1000}, {0x80005000, 0x3000}});
    }
    // Pages above the limit: only they are staged, each at its chain offset in the bounce buffer; neighbours that are
    // staged together form one run (one descriptor), and one request's consecutive segments one stage.
    {
        Memory a;
        a.runs = {{0x80001000, 0x1000}, {0x123400000ull, 0x1000}, {0x123406000ull, 0x1000}, {0x80009000, 0x1000}};
        Table t(1024, 1ull << 32);
        Staging st;
        st.reset(&t.t, kBounce, stages.data(), 64);
        REQUIRE(!st.addMemory(&a, 0x4000, anyPage) && st.finish());
        REQUIRE(st.count == 1 && stages[0].memory == &a && stages[0].offset == 0x1000 && stages[0].at == 0x1000);
        REQUIRE(stages[0].length == 0x2000 && st.staged == 0x2000 && st.refused[kAdmaHigh] == 2);
        checkTable(t.t, {{0x80001000, 0x1000}, {kBounce + 0x1000, 0x2000}, {0x80009000, 0x1000}});
        REQUIRE(t.t.high == 0);
    }
    // A segment off a cache line is staged, and so is the next one while the chain offset is off a line too; straight
    // again from the next line-aligned offset.
    {
        Memory a;
        a.runs = {{0x80001010, 0x64}, {0x80002000, 0x19c}, {0x80003000, 0x200}};  // 100 + 412 bytes, then a sector
        Table t(1024, 1ull << 32);
        Staging st;
        st.reset(&t.t, kBounce, stages.data(), 64);
        REQUIRE(!st.addMemory(&a, 0x400, anyPage) && st.finish());
        REQUIRE(st.count == 1 && stages[0].at == 0 && stages[0].length == 0x200 && st.refused[kAdmaAlign] == 2);
        checkTable(t.t, {{kBounce, 0x200}, {0x80003000, 0x200}});
    }
    // Requests' runs that continue each other in the bounce buffer merge into one descriptor across requests, but stay
    // separate stages (different memory).
    {
        Memory a, b;
        a.runs = {{0x123400000ull, 0x1000}};
        b.runs = {{0x123500000ull, 0x1000}, {0x80008000, 0x1000}};
        Table t(1024, 1ull << 32);
        Staging st;
        st.reset(&t.t, kBounce, stages.data(), 64);
        REQUIRE(!st.addMemory(&a, 0x1000, anyPage) && !st.addMemory(&b, 0x2000, anyPage) && st.finish());
        REQUIRE(st.count == 2 && stages[1].memory == &b && stages[1].offset == 0 && stages[1].at == 0x1000);
        checkTable(t.t, {{kBounce, 0x2000}, {0x80008000, 0x1000}});
    }
    // All staged: one run over the whole chain (split at the length cap only).
    {
        Memory a;
        a.runs = {{0x123400000ull, 0x20000}};
        Table t(1024, 1ull << 32);
        Staging st;
        st.reset(&t.t, kBounce, stages.data(), 64);
        REQUIRE(!st.addMemory(&a, 0x20000, anyPage) && st.finish() && st.count == 1);
        checkTable(t.t, {{kBounce, 0x20000}});
    }
    // Too many separate stages, a table too small for the runs, a missing segment: no table at all.
    {
        Memory a;
        for (uint64_t i = 0; i < 6; ++i)
            a.runs.push_back({0x123400000ull + i * 0x2000, 0x1000});
        Table t(1024, 1ull << 32);
        Staging st;
        std::vector<Staging::Stage> few(3);
        st.reset(&t.t, kBounce, few.data(), 3);
        REQUIRE(st.addMemory(&a, 0x6000, anyPage) == kAdmaOk);  // one request's segments coalesce into one stage
        Memory b, c, d, e;
        b.runs = {{0x123500000ull, 0x1000}};
        c.runs = {{0x80000000ull, 0x1000}};
        d.runs = {{0x123600000ull, 0x1000}};
        e.runs = {{0x123700000ull, 0x1000}};
        REQUIRE(st.addMemory(&b, 0x1000, anyPage) == kAdmaOk && st.addMemory(&c, 0x1000, anyPage) == kAdmaOk);
        REQUIRE(st.addMemory(&d, 0x1000, anyPage) == kAdmaOk && st.addMemory(&e, 0x1000, anyPage) == kAdmaFull);
        Memory alternating;
        for (uint64_t i = 0; i < 8; ++i)
            alternating.runs.push_back({(i % 2 ? 0x80000000ull : 0x123400000ull) + i * 0x2000, 0x1000});
        Table small(3, 1ull << 32);
        Staging tight;
        tight.reset(&small.t, kBounce, stages.data(), 64);
        REQUIRE(tight.addMemory(&alternating, 0x8000, anyPage) == kAdmaFull);
        Memory hole = alternating;
        hole.missingAt = 3;
        Table h(1024, 1ull << 32);
        Staging holed;
        holed.reset(&h.t, kBounce, stages.data(), 64);
        REQUIRE(holed.addMemory(&hole, 0x8000, anyPage) == kAdmaMissing);
    }
}

// Random read chains, a random mix of refused pages and odd segments: the table, a simulated DMA through it, then the
// stages copied out must leave every request holding exactly the card's bytes, and nothing else touched.
static void stagingRandom()
{
    std::mt19937_64 random(0x57a9ed);
    std::vector<Staging::Stage> stages(256);
    uint64_t stagedTotal = 0, straightTotal = 0;
    for (unsigned round = 0; round < 300; ++round) {
        // Requests of whole sectors, each made of physical runs: page-sized, sub-page, odd-sized, low or high.
        std::vector<Memory> requests(1 + random() % 6);
        std::vector<uint64_t> lengths;
        uint64_t chain = 0;
        uint64_t nextLow = 0x80000000ull + (random() % 1024) * 0x10000, nextHigh = 0x120000000ull;
        for (Memory &m : requests) {
            const uint64_t sectors = 1 + random() % (random() % 8 ? 8 : 64);
            uint64_t left = sectors * 512;
            while (left) {
                uint64_t n = random() % 3 ? 0x1000 : (random() % 2 ? 0x200 : 1 + random() % 0x400);
                if (n > left)
                    n = left;
                const bool up = random() % 3 == 0, odd = random() % 7 == 0;
                uint64_t &next = up ? nextHigh : nextLow;
                if (random() % 3)
                    next += 0x1000 * (1 + random() % 4);
                const uint64_t pa = next + (odd ? 8 * (1 + random() % 7) : 0);
                m.runs.push_back({pa, n});
                next = (pa + n + 0xfff) & ~0xfffull;
                left -= n;
            }
            lengths.push_back(sectors * 512);
            chain += sectors * 512;
        }
        if (chain > kBounceSize)
            continue;
        const uint64_t refusedPage = random() % 4 ? ~0ull : nextLow - 0x1000;
        auto pageOk = [&](uint64_t page) {
            return page != refusedPage;
        };
        Table t(1024, 1ull << 32);
        Staging st;
        st.reset(&t.t, kBounce, stages.data(), 256);
        bool built = true;
        for (size_t i = 0; i < requests.size() && built; ++i)
            built = !st.addMemory(&requests[i], lengths[i], pageOk);
        if (!built)
            continue;  // more stages than the array: the driver bounces the whole chain
        REQUIRE(st.finish());
        checkStagedTable(t.t, chain);
        stagedTotal += st.staged;
        straightTotal += chain - st.staged;
        // The card's bytes, moved by the "engine" to wherever the table says, then the stages copied out.
        std::vector<uint8_t> card(chain);
        for (uint8_t &b : card)
            b = uint8_t(random());
        std::unordered_map<uint64_t, uint8_t> phys;
        uint64_t at = 0;
        for (uint32_t i = 0; i < t.t.count; ++i) {
            const uint64_t pa = switchAdmaAddress(t.t.entries[i]);
            for (uint64_t k = 0; k < t.t.entries[i].length; ++k)
                phys[pa + k] = card[at + k];
            at += t.t.entries[i].length;
        }
        for (uint32_t i = 0; i < st.count; ++i) {
            const Staging::Stage &s = stages[i];
            REQUIRE(s.at + s.length <= chain);
            for (uint64_t k = 0; k < s.length;) {
                IOByteCount segment = 0;
                const uint64_t pa = s.memory->getPhysicalSegment(s.offset + k, &segment, kIOMemoryMapperNone);
                REQUIRE(pa && segment);
                for (uint64_t j = 0; j < segment && k < s.length; ++j, ++k)
                    phys[pa + j] = phys.at(kBounce + s.at + k);
            }
        }
        // Every request byte is the card's; every byte written is a request's or the bounce buffer's first `chain`.
        uint64_t offset = 0;
        std::unordered_map<uint64_t, bool> own;
        for (size_t i = 0; i < requests.size(); ++i) {
            for (uint64_t k = 0; k < lengths[i];) {
                IOByteCount segment = 0;
                const uint64_t pa = requests[i].getPhysicalSegment(k, &segment, kIOMemoryMapperNone);
                REQUIRE(pa && segment);
                for (uint64_t j = 0; j < segment && k < lengths[i]; ++j, ++k) {
                    REQUIRE(phys.count(pa + j) && phys[pa + j] == card[offset + k]);
                    own[pa + j] = true;
                }
            }
            offset += lengths[i];
        }
        for (const auto &[pa, value] : phys) {
            (void)value;
            REQUIRE(own.count(pa) || (pa >= kBounce && pa < kBounce + chain));
        }
    }
    REQUIRE(stagedTotal > 0 && straightTotal > 0);  // both kinds were exercised
}

// r14-sdcq: a read chain with gaps: each gap one descriptor into the sink, between the requests' own descriptors; staged
// runs keep their place in the chain (their bounce offsets count the gaps), and a run open before a gap closes there.
static void sinkCases()
{
    constexpr uint64_t kSink = 0x88000000ull;
    std::vector<Staging::Stage> stages(64);
    // Straight requests with a 4 KiB and a 512-byte gap.
    {
        Memory a, b, c;
        a.runs = {{0x80001000, 0x1000}};
        b.runs = {{0x80003000, 0x2000}};
        c.runs = {{0x80008000, 0x200}};
        Table t(1024, 1ull << 32);
        Staging st;
        st.reset(&t.t, kBounce, stages.data(), 64);
        REQUIRE(!st.addMemory(&a, 0x1000, anyPage) && !st.addSink(kSink, 0x1000) && !st.addMemory(&b, 0x2000, anyPage));
        REQUIRE(!st.addSink(kSink, 0x200) && !st.addMemory(&c, 0x200, anyPage) && st.finish());
        REQUIRE(st.count == 0 && st.at == 0x4400);
        checkTable(t.t,
                   {{0x80001000, 0x1000}, {kSink, 0x1000}, {0x80003000, 0x2000}, {kSink, 0x200}, {0x80008000, 0x200}});
        checkStagedTable(t.t, 0x4400);
    }
    // A staged run before a gap ends at the gap; a staged segment after it sits at its chain offset (gap included).
    {
        Memory a, b;
        a.runs = {{0x123400000ull, 0x1000}};
        b.runs = {{0x123500000ull, 0x1000}};
        Table t(1024, 1ull << 32);
        Staging st;
        st.reset(&t.t, kBounce, stages.data(), 64);
        REQUIRE(!st.addMemory(&a, 0x1000, anyPage) && !st.addSink(kSink, 0x800) && !st.addMemory(&b, 0x1000, anyPage));
        REQUIRE(st.finish() && st.count == 2 && stages[0].at == 0 && stages[1].at == 0x1800 && st.staged == 0x2000);
        checkTable(t.t, {{kBounce, 0x1000}, {kSink, 0x800}, {kBounce + 0x1800, 0x1000}});
    }
    // Plain tables (no staging) take the sink the same way, and two gaps never merge into one descriptor.
    {
        Memory a, b, c;
        a.runs = {{0x80001000, 0x200}};
        b.runs = {{0x80002000, 0x200}};
        c.runs = {{0x80003000, 0x200}};
        Table t(1024, 1ull << 32);
        REQUIRE(!t.t.addMemory(&a, 0x200, anyPage) && !t.t.add(kSink, 0x400, anyPage) &&
                !t.t.addMemory(&b, 0x200, anyPage));
        REQUIRE(!t.t.add(kSink, 0x400, anyPage) && !t.t.addMemory(&c, 0x200, anyPage) && t.t.finish());
        REQUIRE(t.t.count == 5);
        checkTable(t.t,
                   {{0x80001000, 0x200}, {kSink, 0x400}, {0x80002000, 0x200}, {kSink, 0x400}, {0x80003000, 0x200}});
    }
    // The sink and staged runs never share a descriptor with a request's pages, whatever is next to them in physical
    // memory: a client run ending right where the sink starts, the longest gap followed by a client page right after the
    // sink, a staged run in the bounce buffer with a client page right below it and one right after it.
    {
        Memory a, b;
        a.runs = {{kSink - 0x1000, 0x1000}};
        b.runs = {{kSink + 0x8000, 0x1000}};
        Table t(1024, 1ull << 32);
        Staging st;
        st.reset(&t.t, kBounce, stages.data(), 64);
        REQUIRE(!st.addMemory(&a, 0x1000, anyPage) && !st.addSink(kSink, 0x8000) && !st.addMemory(&b, 0x1000, anyPage));
        REQUIRE(st.finish() && t.t.count == 3);
        checkTable(t.t, {{kSink - 0x1000, 0x1000}, {kSink, 0x8000}, {kSink + 0x8000, 0x1000}});
        Table plain(1024, 1ull << 32);
        REQUIRE(!plain.t.addMemory(&a, 0x1000, anyPage) && !plain.t.addSealed(kSink, 0x8000));
        REQUIRE(!plain.t.addMemory(&b, 0x1000, anyPage) && plain.t.finish() && plain.t.count == 3);
        // Ordinary contiguous memory still merges after a sealed descriptor's neighbour.
        Memory c;
        c.runs = {{kSink + 0x9000, 0x1000}};
        REQUIRE(!plain.t.addMemory(&c, 0x1000, anyPage) && plain.t.count == 3);
    }
    {
        Memory below, high, above;
        below.runs = {{kBounce - 0x1000, 0x1000}};
        high.runs = {{0x123400000ull, 0x1000}};
        above.runs = {{kBounce + 0x2000, 0x1000}};  // where the run (chain offset 0x1000, 0x1000 long) ends
        Table t(1024, 1ull << 32);
        Staging st;
        st.reset(&t.t, kBounce, stages.data(), 64);
        REQUIRE(!st.addMemory(&below, 0x1000, anyPage) && !st.addMemory(&high, 0x1000, anyPage));
        REQUIRE(!st.addMemory(&above, 0x1000, anyPage) && st.finish() && t.t.count == 3);
        checkTable(t.t, {{kBounce - 0x1000, 0x1000}, {kBounce + 0x1000, 0x1000}, {kBounce + 0x2000, 0x1000}});
    }
    // A full table refuses the sink like any other descriptor.
    {
        Memory a;
        a.runs = {{0x80001000, 0x200}};
        Table t(1, 1ull << 32);
        Staging st;
        st.reset(&t.t, kBounce, stages.data(), 64);
        REQUIRE(!st.addMemory(&a, 0x200, anyPage) && st.addSink(kSink, 0x200) == kAdmaFull);
    }
}

TEST(SdAdma, DescriptorLayout)
{
    layout();
}

TEST(SdAdma, SinglePages)
{
    singlePages();
}

TEST(SdAdma, SplitsAtTheLengthCap)
{
    splitsAtTheLengthCap();
}

TEST(SdAdma, Refusals)
{
    refusals();
}

TEST(SdAdma, Chains)
{
    chains();
}

TEST(SdAdma, RandomRuns)
{
    randomRuns();
}

TEST(SdAdma, HighBytes)
{
    highBytes();
}

TEST(SdAdma, StagingCases)
{
    stagingCases();
}

TEST(SdAdma, StagingRandom)
{
    stagingRandom();
}

TEST(SdAdma, SinkCases)
{
    sinkCases();
}

TEST(SdAdma, OverlapFastPathAndClassifiedOncePerPage)
{
    constexpr uint64_t kSink = 0x80200000;
    constexpr uint64_t kSinkBytes = 0x8000;
    {
        Table a, b;
        a.t.reset();
        b.t.reset();
        for (uint32_t i = 0; i < 64; ++i) {
            REQUIRE(a.t.add(0x90000000ull + i * 0x2000, 0x1000, anyPage) == kAdmaOk);
            REQUIRE(b.t.add(0xa0000000ull + i * 0x2000, 0x1000, anyPage) == kAdmaOk);
        }
        REQUIRE(a.t.addSealed(kSink, kSinkBytes) == kAdmaOk);
        REQUIRE(b.t.addSealed(kSink, kSinkBytes) == kAdmaOk);
        EXPECT_TRUE(switchAdmaTablesOverlap(a.t, b.t));
        EXPECT_FALSE(switchAdmaTablesOverlap(a.t, b.t, kSink, kSinkBytes));
    }
    {
        Table a, b;
        a.t.reset();
        b.t.reset();
        for (uint32_t i = 0; i < 40; ++i) {
            REQUIRE(a.t.add(0x90000000ull + (39 - i) * 0x4000, 0x1000, anyPage) == kAdmaOk);
            REQUIRE(b.t.add(0x90002000ull + i * 0x4000, 0x1000, anyPage) == kAdmaOk);
        }
        EXPECT_FALSE(switchAdmaTablesOverlap(a.t, b.t));
        REQUIRE(b.t.add(0x90000000ull + 17 * 0x4000 + 0x800, 0x200, anyPage) == kAdmaOk);
        EXPECT_TRUE(switchAdmaTablesOverlap(a.t, b.t));
    }
    {
        Memory m;
        m.runs = {{0x90000000ull, 0x4000}};
        std::unordered_map<uint64_t, int> calls;
        auto kind = [&](uint64_t page) -> uint8_t {
            ++calls[page];
            return (page == 0x90001000ull || page == 0x90002000ull) ? 2 : 1;
        };
        Table t;
        t.t.reset();
        uint64_t uncachedBits[16] = {};
        bool hadUncached = false;
        REQUIRE(t.t.addMemoryClassified(&m, 0x4000, kind, uncachedBits, 16, &hadUncached) == kAdmaOk);
        REQUIRE(t.t.finish());
        EXPECT_TRUE(hadUncached);
        EXPECT_EQ(t.t.count, 3u);
        EXPECT_EQ((uncachedBits[0] & (1ULL << 0)), 0ULL);
        EXPECT_NE((uncachedBits[0] & (1ULL << 1)), 0ULL);
        EXPECT_EQ((uncachedBits[0] & (1ULL << 2)), 0ULL);
        for (uint64_t p = 0x90000000ull; p < 0x90004000ull; p += 0x1000)
            EXPECT_EQ(calls[p], 1);
    }
}

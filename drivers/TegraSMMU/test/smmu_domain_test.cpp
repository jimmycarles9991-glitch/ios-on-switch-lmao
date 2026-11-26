// drivers/TegraSMMU/TegraSMMUDomain.h on the host: entry encodings, table hand-out, identity maps, page maps, translation,
// limits and dirty tracking; and the tables TegraVIC builds with it, byte for byte against TegraVIC's own original
// algorithm, so VIC's address space is unchanged.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../TegraSMMUDomain.h"

namespace {
__attribute__((format(printf, 1, 2))) std::string message(const char *format, ...)
{
    char text[512];
    va_list ap = nullptr;
    va_start(ap, format);
    vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    return text;
}

#define CHECK(cond, ...) EXPECT_TRUE(cond) << message(__VA_ARGS__)

constexpr uint32_t R = 1u << 31, W = 1u << 30, NS = 1u << 29, NEXT = 1u << 28;

struct Buffer {
    std::vector<uint32_t> words;
    uint64_t pa;

    Buffer(uint32_t pages, uint64_t at) : words(static_cast<size_t>(pages) * 1024, 0xdeadbeef), pa(at)
    {}

    uint32_t *va()
    {
        return words.data();
    }
};

// TegraVIC's original tables (TegraVIC.cpp at eae4371: smmuSetUp, pte, identity), on its own buffer.
struct OldVic {
    static constexpr uint64_t kWindowBase = 0x10000000ULL, kWindowBytes = 128ULL << 20;
    static constexpr uint32_t kWindows = 2, kWindowTables = static_cast<uint32_t>(kWindowBytes >> 22),
                              kIdentityTables = 4;
    Buffer b;
    uint32_t identityUsed = 0;

    explicit OldVic(uint64_t pa) : b(1 + kWindows * kWindowTables + kIdentityTables, pa)
    {
        std::ranges::fill(b.words, 0);  // contiguous() zeroes the buffer
    }

    uint32_t *table(uint32_t i)
    {
        return b.va() + static_cast<size_t>(i) * 1024;
    }

    uint64_t tablePa(uint32_t i) const
    {
        return b.pa + static_cast<uint64_t>(i) * 4096;
    }

    uint32_t *pte(uint64_t iova)
    {
        uint32_t *pd = table(0);
        const uint32_t pdi = static_cast<uint32_t>(iova >> 22);
        if (!(pd[pdi] & NEXT)) {
            if (identityUsed == kIdentityTables)
                return nullptr;
            const uint32_t index = 1 + kWindows * kWindowTables + identityUsed++;
            pd[pdi] = static_cast<uint32_t>(tablePa(index) >> 12) | R | W | NS | NEXT;
        }
        const uint64_t index = ((static_cast<uint64_t>(pd[pdi] & 0x3fffff) << 12) - b.pa) / 4096;
        return &table(static_cast<uint32_t>(index))[(iova >> 12) & 1023];
    }

    bool identity(uint64_t pa, uint64_t size, bool writable)
    {
        for (uint64_t at = pa; at < pa + size; at += 4096) {
            uint32_t *p = pte(at);
            if (!p)
                return false;
            *p = static_cast<uint32_t>(at >> 12) | R | NS | (writable ? W : 0);
        }
        return true;
    }

    bool setUp(uint64_t fwPa, uint64_t fwSize, uint64_t arenaPa, uint64_t arenaSize)
    {
        uint32_t *pd = table(0);
        for (uint32_t w = 0; w < kWindows; w++) {
            for (uint32_t t = 0; t < kWindowTables; t++) {
                const uint64_t iova = kWindowBase + w * kWindowBytes + (static_cast<uint64_t>(t) << 22);
                pd[iova >> 22] = static_cast<uint32_t>(tablePa(1 + w * kWindowTables + t) >> 12) | R | W | NS | NEXT;
            }
        }
        return identity(fwPa, fwSize, false) && identity(arenaPa, arenaSize, true);
    }
};

// TegraVIC's tables on the shared domain (TegraVIC.cpp smmuSetUp).
bool newVic(Buffer &b, uint64_t fwPa, uint64_t fwSize, uint64_t arenaPa, uint64_t arenaSize)
{
    TegraSMMUDomain d;
    if (!d.init(b.va(), b.pa, static_cast<uint32_t>(b.words.size() / 1024)))
        return false;
    for (uint32_t w = 0; w < OldVic::kWindows; w++) {
        if (!d.reserve(OldVic::kWindowBase + w * OldVic::kWindowBytes, OldVic::kWindowBytes))
            return false;
    }
    return d.identity(fwPa, fwSize, false) && d.identity(arenaPa, arenaSize, true);
}

void vicTablesUnchanged()
{
    struct Case {
        uint64_t tables, fw, fwSize, arena, arenaSize;
    } const cases[] = {
        {0xe1000000, 0xe1200000, 0x14000, 0xe1300000, 1 << 20},  // all in one 4 MiB section
        {0xf0000000, 0xf03fe000, 0x14000, 0xf07ff000, 1 << 20},  // firmware and arena across section boundaries
        {0x90000000, 0xa0000000, 0x40000, 0xb0000000, 1 << 20},  // far apart
    };

    for (const Case &c : cases) {
        OldVic old(c.tables);
        Buffer now(1 + OldVic::kWindows * OldVic::kWindowTables + OldVic::kIdentityTables, c.tables);
        std::ranges::fill(now.words, 0);  // contiguous() zeroes VIC's buffer too
        const bool a = old.setUp(c.fw, c.fwSize, c.arena, c.arenaSize);
        const bool b = newVic(now, c.fw, c.fwSize, c.arena, c.arenaSize);
        CHECK(a && b, "VIC set-up failed (old %d new %d) for tables %#llx", a, b, (unsigned long long)c.tables);
        CHECK(old.b.words == now.words, "VIC's tables differ from the old algorithm's for tables at %#llx",
              (unsigned long long)c.tables);
    }
    OldVic old(0xe1000000);
    Buffer now(1 + OldVic::kWindows * OldVic::kWindowTables + OldVic::kIdentityTables, 0xe1000000);
    const bool a = old.setUp(0x90000000, 0x14000, 0xa0000000, 20ULL << 20);
    const bool b = newVic(now, 0x90000000, 0x14000, 0xa0000000, 20ULL << 20);
    CHECK(!a && !b, "a set-up past the identity tables must fail both ways (old %d new %d)", a, b);
}

void basics()
{
    Buffer b(8, 0xe2000000);
    TegraSMMUDomain d;
    CHECK(!d.init(b.va(), 0xe2000800, 8), "unaligned table buffer accepted");
    CHECK(!d.init(b.va(), 0xfffff000, 8), "table buffer past 4 GiB accepted");
    CHECK(d.init(b.va(), b.pa, 8), "init");
    for (uint32_t i = 0; i < 1024; ++i)
        CHECK(b.words[i] == 0, "directory entry %u not cleared", i);
    CHECK(d.ptb() == ((uint32_t)(b.pa >> 12) | R | W | NS), "PTB %#x", d.ptb());
    CHECK(d.translate(0x10000000) == UINT64_MAX, "unmapped address translates");

    CHECK(d.identity(0xf5a00000, 0x3c0000, false), "identity of the boot framebuffer");
    CHECK(d.translate(0xf5a00000) == 0xf5a00000 && d.translate(0xf5dbfff8) == 0xf5dbfff8, "identity translation");
    CHECK(d.translate(0xf5dc0000) == UINT64_MAX, "identity ran past its end");
    CHECK(*d.pte(0xf5a00000, false) == (0xf5a00u | R | NS), "read-only entry %#x", *d.pte(0xf5a00000, false));
    CHECK(d.identity(0xe3000000, 4096, true) && *d.pte(0xe3000000, false) == (0xe3000u | R | W | NS), "writable entry");

    uint64_t pages[5] = {0x81234000, 0x9abc0000, 0x1fffff000ULL & 0xfffff000, 0xc0001000, 0x80002000};
    CHECK(d.map(0x10000000, pages, 5, false), "map");
    for (uint32_t i = 0; i < 5; ++i)
        CHECK(d.translate(0x10000000 + i * 4096ULL + 0x123) == pages[i] + 0x123, "page %u translates wrong", i);
    d.unmap(0x10000000, 5);
    CHECK(d.translate(0x10000000) == UINT64_MAX && d.translate(0x10004000) == UINT64_MAX, "unmap left entries");

    CHECK(!d.pte(1ULL << 32, true), "address past 4 GiB accepted");
    uint32_t got = 0;
    for (uint64_t at = 0x40000000; at < 0x80000000 && d.reserve(at, 4096); at += 4ULL << 20)
        ++got;
    // directory + 2 (the framebuffer spans two 4 MiB sections) + 1 (0xe3000000) + 1 (the slot) = 5 used before, 3 left
    CHECK(d.used == d.pages && got == 3, "tables handed out %u (used %u of %u)", got, d.used, d.pages);
    CHECK(!d.identity(0x70000000, 4096, true), "a map with no table left succeeded");

    Buffer c(4, 0xe4000000);
    TegraSMMUDomain e;
    uintptr_t low = 0;
    uintptr_t high = 0;
    e.init(c.va(), c.pa, 4);
    e.takeDirty(&low, &high);
    CHECK(!e.takeDirty(&low, &high), "dirty after take");
    e.identity(0x88000000, 8192, true);
    CHECK(e.takeDirty(&low, &high), "identity not dirty");
    const uintptr_t pde = reinterpret_cast<uintptr_t>(&c.words[0x88000000u >> 22]);
    const uintptr_t pte = reinterpret_cast<uintptr_t>(e.pte(0x88001000, false));
    CHECK(low <= pde && high >= pde + 4 && low <= pte && high >= pte + 4, "dirty range misses the entries");

    Buffer f(1 + 32 + 1, 0xe5000000);
    TegraSMMUDomain g;
    g.init(f.va(), f.pa, 1 + 32 + 1);
    CHECK(g.reserve(0x10000000, 128ULL << 20), "reserve a window");
    for (uint32_t t = 0; t < 32; ++t) {
        CHECK(f.words[(0x10000000u >> 22) + t] == (((uint32_t)(f.pa >> 12) + 1 + t) | R | W | NS | NEXT),
              "window table %u not consecutive", t);
    }
}

#define CHECK_CHANGE(c, iovaFrom, iovaTo, dir, ...)                                                               \
    CHECK((c).iovaLow == (iovaFrom) && (c).iovaHigh == (iovaTo) && (c).directory == (dir),                        \
          "%s: IOVAs [%#llx, %#llx) directory %d", message(__VA_ARGS__).c_str(), (unsigned long long)(c).iovaLow, \
          (unsigned long long)(c).iovaHigh, (c).directory)

// What each change records for the flush (takeChange): the table bytes it wrote and their physical address, the IOVAs
// whose page entries it wrote and whether it wrote a directory entry; takeDirty takes all of it.
void changes()
{
    Buffer b(1 + 32 + 3, 0xe6000000);
    TegraSMMUDomain d;
    TegraSMMUDomain::Change c{};
    CHECK(d.init(b.va(), b.pa, 1 + 32 + 3) && d.takeChange(&c) && c.directory, "init writes the directory");
    CHECK(!d.takeChange(&c), "a change taken twice");
    CHECK(d.reserve(0x10000000, 128ull << 20) && d.takeChange(&c) && c.directory, "reserve writes the directory");

    const uint64_t pages[3] = {0x81000000, 0x81001000, 0x81002000};
    CHECK(d.map(0x10003000, pages, 3, true) && d.takeChange(&c), "map");
    CHECK_CHANGE(c, 0x10003000ull, 0x10006000ull, false, "map");
    CHECK(c.low == reinterpret_cast<uintptr_t>(d.pte(0x10003000, false)) && c.high - c.low == 12 &&
              c.pa == b.pa + 4096 + 3 * 4,
          "map: table bytes at %#llx, %llu of them", (unsigned long long)c.pa, (unsigned long long)(c.high - c.low));

    d.unmap(0x10004000, 1);
    CHECK(d.takeChange(&c), "unmap");
    CHECK_CHANGE(c, 0x10004000ull, 0x10005000ull, false, "unmap");
    CHECK(c.pa == b.pa + 4096 + 4 * 4 && c.high - c.low == 4, "unmap: table bytes at %#llx", (unsigned long long)c.pa);
    d.unmap(0x10004000, 1);
    CHECK(!d.takeChange(&c), "unmapping an unmapped page changed something");

    CHECK(d.map(0x103ff000, pages, 3, false) && d.takeChange(&c), "map across a section");
    CHECK_CHANGE(c, 0x103ff000ull, 0x10402000ull, false, "map across a section");
    CHECK(c.pa == b.pa + 4096 + 1023 * 4 && c.high - c.low == 12, "map across a section: table bytes at %#llx",
          (unsigned long long)c.pa);

    CHECK(d.identity(0x88001800, 0x1000, false) && d.takeChange(&c), "identity in a section without a table");
    CHECK_CHANGE(c, 0x88001000ull, 0x88003000ull, true, "identity in a new section");
    CHECK(d.identity(0x88004000, 4096, true) && d.takeChange(&c), "identity in its table");
    CHECK_CHANGE(c, 0x88004000ull, 0x88005000ull, false, "identity in its table");
    CHECK(c.pa == b.pa + 33 * 4096 + 4 * 4, "identity: table bytes at %#llx", (unsigned long long)c.pa);

    uintptr_t low = 0, high = 0;
    CHECK(d.identity(0x8c000000, 4096, true) && d.takeDirty(&low, &high), "identity, taken by takeDirty");
    CHECK(!d.takeChange(&c), "takeDirty left a change behind");
    CHECK(d.map(0x10020000, pages, 1, false) && d.takeChange(&c), "map after takeDirty");
    CHECK_CHANGE(c, 0x10020000ull, 0x10021000ull, false, "map after takeDirty");
}
}  // namespace

TEST(TegraSMMUDomain, Basics)
{
    basics();
}

// The IOVAs and table bytes a flush needs, and whether the directory changed, after map, unmap and identity.
TEST(TegraSMMUDomain, Changes)
{
    changes();
}

// The tables TegraVIC builds on the shared domain, byte for byte as its original algorithm built them.
TEST(TegraSMMUDomain, VicTablesUnchanged)
{
    vicTablesUnchanged();
}

// tablesFor counts what reserve() installs in an empty domain: every 4 MiB span the range touches.
TEST(TegraSMMUDomain, TablesForIsWhatReserveInstalls)
{
    struct Range {
        uint64_t iova, bytes;
    };

    for (const Range r : {Range{1ULL << 20, 64ULL << 20}, Range{0, 4ULL << 20}, Range{(4ULL << 20) - 1, 2},
                          Range{8ULL << 20, 1}, Range{0x80000000, 0x80000000}}) {
        const uint32_t count = TegraSMMUDomain::tablesFor(r.iova, r.bytes);
        std::vector<uint32_t> tables(static_cast<size_t>(count + 1) * TegraSMMUDomain::kEntries);
        TegraSMMUDomain domain;
        ASSERT_TRUE(domain.init(tables.data(), 0x9000'0000, count + 1));
        EXPECT_TRUE(domain.reserve(r.iova, r.bytes)) << r.iova << " + " << r.bytes;
        EXPECT_EQ(domain.used - 1, count) << r.iova << " + " << r.bytes;
    }
    EXPECT_EQ(TegraSMMUDomain::tablesFor(1ULL << 20, 64ULL << 20), 17u) << "a window from 1 MiB";
    EXPECT_EQ(TegraSMMUDomain::tablesFor(1ULL << 20, 0), 0u);
}

// A 4 MiB section (L4T tegra-smmu.c: a directory entry without NEXT): one entry maps the whole span, offsets and all; it
// needs an aligned IOVA and target and an empty entry, and no table is ever installed over it.
TEST(TegraSMMUDomain, SectionsMapFourMiB)
{
    std::vector<uint32_t> tables(size_t{2} * TegraSMMUDomain::kEntries);
    TegraSMMUDomain d;
    ASSERT_TRUE(d.init(tables.data(), 0x9000'0000, 2));
    ASSERT_TRUE(d.section(0x0040'0000, 0x1'0040'0000, true));
    EXPECT_EQ(tables[1], 0x10'0400u | TegraSMMUDomain::kRead | TegraSMMUDomain::kWrite | TegraSMMUDomain::kNonSecure);
    for (const uint64_t offset : {0ULL, 0x1234ULL, 0x3f'ffffULL})
        EXPECT_EQ(d.translate(0x0040'0000 + offset), 0x1'0040'0000 + offset);
    EXPECT_EQ(d.translate(0x0080'0000), UINT64_MAX);
    EXPECT_FALSE(d.section(0x0040'0000, 0x8000'0000, true)) << "over a section";
    EXPECT_FALSE(d.section(0x0080'1000, 0x8000'0000, true)) << "IOVA not at 4 MiB";
    EXPECT_FALSE(d.section(0x0080'0000, 0x8000'1000, true)) << "target not at 4 MiB";
    EXPECT_FALSE(d.pte(0x0040'1000, true)) << "a table over a section";
    EXPECT_EQ(d.used, 1u);
    ASSERT_TRUE(d.reserve(0x0080'0000, 4096));
    EXPECT_FALSE(d.section(0x0080'0000, 0x8000'0000, true)) << "over a table";
}

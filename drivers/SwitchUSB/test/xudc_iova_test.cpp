// XudcIova.h (SwitchXUDCSmmu.cpp) on the host, over the device controller's static map (usb_static_map_test): every
// request byte's fixed IOVA translates back to it through the map's tables, and the planner over fixed IOVAs bounces
// nothing for 4 GiB, so a page above 4 GiB never bounces and never takes a window page. Then the window: over random
// layouts (whole pages, mbuf-like ranges at odd offsets) every request byte's window IOVA translates through the mapped
// pages to that byte, a mapping bounces nothing at a break on a page boundary or at a 64 KiB line in one run that fits
// in one, and what is left is what a mapping cannot change (a break inside a page, an OUT cache-line edge, a 64 KiB
// line inside a packet of a longer request at another offset). The page-table writes a mapping costs are counted
// against the bytes the copies it replaces would move.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <vector>

#include "../UsbStaticMap.h"
#include "../XudcIova.h"
#include "../XudcIsoch.h"

namespace {

using Runs = std::vector<xudc::Run>;
constexpr uint64_t kPage = xudc::kIovaPage;
constexpr uint64_t kGiB4 = 1ULL << 32;
// The banks every committed ledger has (kernel/switch/include/switch/ram.h SWITCH_RAM_BANK*).
constexpr UsbStaticMap kSwitch = UsbStaticMap::device({0x8000'0000, 0xffd0'0000, 0x1'0000'0000, 0x1'8000'0000});
constexpr uint64_t kBase = 0xffd0'0000;  // the Switch's window

struct Space {
    std::vector<uint32_t> tables;
    TegraSMMUDomain domain;

    explicit Space(const UsbStaticMap &map) : tables(static_cast<size_t>(1 + map.tables()) * TegraSMMUDomain::kEntries)
    {
        EXPECT_TRUE(domain.init(tables.data(), 0x9000'0000, 1 + map.tables()));
        EXPECT_TRUE(map.install(domain));
    }
};

const Space &switchSpace()
{
    static const Space space(kSwitch);
    return space;
}

struct Mapping {
    std::map<uint64_t, uint64_t> pages;  // IOVA page -> physical page
    uint32_t count = 0;

    explicit Mapping(const Runs &runs)
        : count(xudc::runPages(runs.data(), static_cast<uint32_t>(runs.size()), [this](uint32_t i, uint64_t pa) {
              pages[kBase + i * kPage] = pa;
          }))
    {}

    uint64_t translate(uint64_t iova) const
    {
        const auto it = pages.find(iova & ~(kPage - 1));
        return it == pages.end() ? ~0ULL : it->second + (iova & (kPage - 1));
    }
};

Runs iovaRuns(const Runs &runs)
{
    Runs out(runs.size());
    out.resize(xudc::iovaRuns(runs.data(), static_cast<uint32_t>(runs.size()), kBase, out.data()));
    return out;
}

Runs fixedRuns(const Runs &runs)
{
    Runs out(runs.size());
    out.resize(xudc::fixedRuns(
        runs.data(), static_cast<uint32_t>(runs.size()),
        [](uint64_t pa, uint64_t length) {
            return kSwitch.iova(pa, length);
        },
        out.data()));
    return out;
}

// The address of request byte `offset` in `runs`.
uint64_t at(const Runs &runs, uint64_t offset)
{
    for (const xudc::Run &r : runs) {
        if (offset < r.length)
            return r.pa + offset;
        offset -= r.length;
    }
    return ~0ULL;
}

uint64_t total(const Runs &runs)
{
    uint64_t sum = 0;
    for (const xudc::Run &r : runs)
        sum += r.length;
    return sum;
}

Runs randomRuns(std::mt19937_64 &rng)
{
    Runs runs;
    const int kind = static_cast<int>(rng() % 3);
    const unsigned count = 1 + static_cast<unsigned>(rng() % 12);
    uint64_t next = 0x8000'0000 + (rng() % 65536) * kPage;
    for (unsigned i = 0; i < count; ++i) {
        const bool high = rng() % 4 == 0;
        const uint64_t base = high ? 0x1'0000'0000 + (rng() % 65536) * kPage : 0x8000'0000 + (rng() % 65536) * kPage;
        if (kind == 0) {  // whole pages, now and then contiguous
            next = rng() % 2 ? next + kPage : base;
            runs.push_back({next, static_cast<uint32_t>(kPage)});
        } else if (kind == 1) {  // mbufs: 2 KiB clusters and odd ranges
            const uint32_t length = rng() % 2 ? 2048 : 1 + static_cast<uint32_t>(rng() % 3000);
            runs.push_back({base + (rng() % 2 ? 0 : 2048) + rng() % 64, length});
        } else {  // one buffer at a page, up to 96 KiB
            runs.push_back({base, 512 * (1 + static_cast<uint32_t>(rng() % 192))});
            break;
        }
    }
    Runs merged;
    for (const xudc::Run &r : runs) {
        if (!merged.empty() && merged.back().pa + merged.back().length == r.pa) {
            merged.back().length += r.length;
        } else {
            merged.push_back(r);
        }
    }
    return merged;
}

uint32_t plan(const Runs &runs, uint32_t mps, bool out, xudc::PlanStats *why)
{
    std::vector<xudc::Piece> pieces(1021);
    *why = {};
    return xudc::plan(runs.data(), static_cast<uint32_t>(runs.size()), static_cast<uint32_t>(total(runs)), mps, out,
                      kGiB4, pieces.data(), 1021, why);
}

}  // namespace

// Every byte of a request, low and high pages alike, at its fixed IOVA, translates through the tables to its own.
TEST(XudcIova, FixedRunsTranslateThroughTheTables)
{
    const TegraSMMUDomain &d = switchSpace().domain;
    std::mt19937_64 rng(3);  // NOLINT(bugprone-random-generator-seed): a fixed seed, so a failure reproduces
    for (int c = 0; c < 2000; ++c) {
        const Runs runs = randomRuns(rng);
        const Runs fixed = fixedRuns(runs);
        ASSERT_FALSE(fixed.empty());
        ASSERT_LE(fixed.size(), runs.size());
        ASSERT_EQ(total(fixed), total(runs));
        for (uint64_t offset = 0; offset < total(runs); offset += 1 + rng() % 97)
            ASSERT_EQ(d.translate(at(fixed, offset)), at(runs, offset)) << "case " << c << " byte " << offset;
    }
    const Runs outside = {{0x8000'0000, 4096}, {0x1'8000'0000, 4096}};
    EXPECT_EQ(xudc::fixedRuns(
                  outside.data(), 2,
                  [](uint64_t pa, uint64_t n) {
                      return kSwitch.iova(pa, n);
                  },
                  Runs(2).data()),
              0u)
        << "a run outside RAM fails the request";
}

// The high bank ends at IOVA 0x80000000, where the low bank starts: runs that end the one and start the other are one
// run, and still translate byte for byte.
TEST(XudcIova, RunsAcrossTheBankSeamAreOneRun)
{
    const Runs runs = {{0x1'7fff'f000, 4096}, {0x8000'0000, 4096}};
    const Runs fixed = fixedRuns(runs);
    ASSERT_EQ(fixed.size(), 1u);
    EXPECT_EQ(fixed[0].pa, 0x7fff'f000u);
    EXPECT_EQ(fixed[0].length, 8192u);
    for (uint64_t offset = 0; offset < 8192; offset += 511)
        EXPECT_EQ(switchSpace().domain.translate(at(fixed, offset)), at(runs, offset));
}

// The SMMU's point: a page above 4 GiB is reached direct. Over fixed IOVAs nothing bounces for 4 GiB and nothing bounces
// that did not with the SMMU off; a request in the high bank is planned, and takes the window, exactly as the same
// layout in the low bank would; a lone high buffer gives the window nothing to do, in bulk and isochronous transfers
// alike.
TEST(XudcIova, HighPagesNeverBounceOrTakeTheWindow)
{
    const Runs usbmux = {{0x1'2345'0000, 32768}};
    const Runs fixed = fixedRuns(usbmux);
    ASSERT_EQ(fixed.size(), 1u);
    EXPECT_EQ(fixed[0].pa, 0x2345'0000u);
    EXPECT_FALSE(xudc::remapHelps(fixed.data(), 1));
    for (const bool out : {false, true}) {
        xudc::PlanStats physical = {};
        xudc::PlanStats now = {};
        ASSERT_NE(plan(usbmux, 512, out, &physical), 0u);
        EXPECT_EQ(physical.high, 32768u / 512) << "the SMMU off: every packet through the pool";
        ASSERT_NE(plan(fixed, 512, out, &now), 0u);
        EXPECT_EQ(now.bounceBytes, 0u) << (out ? "OUT" : "IN");
    }
    xudc::Piece frame = {};
    ASSERT_TRUE(xudc::planFrame(fixed.data(), 1, 1024, 512, false, kGiB4, &frame));
    EXPECT_FALSE(frame.bounce);
    EXPECT_EQ(frame.pa, 0x2345'0400u);

    std::mt19937_64 rng(21);  // NOLINT(bugprone-random-generator-seed): a fixed seed, so a failure reproduces
    uint32_t allHigh = 0;
    for (int c = 0; c < 4000; ++c) {
        const Runs runs = randomRuns(rng);
        const bool out = rng() % 2 != 0;
        const uint32_t mps = rng() % 4 ? 512 : 64;
        xudc::PlanStats physical = {};
        xudc::PlanStats now = {};
        if (!plan(runs, mps, out, &physical))
            continue;
        const Runs fixed = fixedRuns(runs);
        ASSERT_NE(plan(fixed, mps, out, &now), 0u) << "case " << c;
        EXPECT_EQ(now.high, 0u) << "case " << c;
        EXPECT_LE(now.bounceBytes, physical.bounceBytes) << "case " << c;
        bool high = true;
        Runs low = runs;
        for (xudc::Run &r : low) {
            high = high && r.pa >= kGiB4;
            r.pa -= 0x8000'0000;
        }
        if (!high)
            continue;
        ++allHigh;
        xudc::PlanStats lowStats = {};
        ASSERT_NE(plan(low, mps, out, &lowStats), 0u) << "case " << c;
        EXPECT_EQ(now.bounceBytes, lowStats.bounceBytes) << "case " << c;
        EXPECT_EQ(xudc::remapHelps(fixed.data(), static_cast<uint32_t>(fixed.size())),
                  xudc::remapHelps(low.data(), static_cast<uint32_t>(low.size())))
            << "case " << c;
    }
    EXPECT_GT(allHigh, 100u);
}

TEST(XudcIova, EveryByteTranslatesToItsOwn)
{
    std::mt19937_64 rng(5);  // NOLINT(bugprone-random-generator-seed): a fixed seed, so a failure reproduces
    for (int c = 0; c < 2000; ++c) {
        const Runs runs = randomRuns(rng);
        const Mapping map(runs);
        const Runs iova = iovaRuns(runs);
        ASSERT_LE(iova.size(), runs.size());
        ASSERT_EQ(total(iova), total(runs));
        for (uint64_t offset = 0; offset < total(runs); offset += 1 + rng() % 97)
            ASSERT_EQ(map.translate(at(iova, offset)), at(runs, offset)) << "case " << c << " byte " << offset;
        for (const xudc::Run &r : iova)
            ASSERT_LE(r.pa + r.length, kGiB4) << "the window is below 4 GiB";
    }
}

TEST(XudcIova, PageBoundaryBreaksJoinIntraPageOnesStay)
{
    const Runs pages = {{0x8000'4000, 4096}, {0x9123'0000, 4096}, {0x1'2000'0000, 8192}};
    EXPECT_EQ(iovaRuns(pages).size(), 1u) << "pages anywhere, above 4 GiB too: one run";
    EXPECT_EQ(Mapping(pages).count, 4u);
    const Runs clusters = {{0x8000'0800, 2048}, {0x8100'0000, 2048}};  // a cluster ends a page, the next starts one
    EXPECT_EQ(iovaRuns(clusters).size(), 1u);
    const Runs mbufs = {{0x8000'0010, 100}, {0x8100'0200, 900}};  // a break inside a page
    EXPECT_EQ(iovaRuns(mbufs).size(), 2u);
    const Runs shared = {{0x8000'0010, 100}, {0x8000'0400, 100}};  // two mbufs in one page: mapped once
    EXPECT_EQ(Mapping(shared).count, 1u);
    EXPECT_EQ(iovaRuns(shared)[1].pa, kBase + 0x400);
}

TEST(XudcIova, RemapHelpsOnlyWhereAMappingCan)
{
    const Runs pages = {{0x8000'4000, 4096}, {0x9123'0000, 4096}};
    EXPECT_TRUE(xudc::remapHelps(pages.data(), 2)) << "a break on a page boundary";
    const Runs crossing = {{0x8000'f000, 8192}};
    EXPECT_TRUE(xudc::remapHelps(crossing.data(), 1)) << "across a 64 KiB line";
    const Runs mbufs = {{0x8000'0010, 100}, {0x8100'0200, 900}};
    EXPECT_FALSE(xudc::remapHelps(mbufs.data(), 2)) << "a break inside a page: nothing a mapping changes";
    const Runs buffer = {{0x8000'4000, 32768}};
    EXPECT_FALSE(xudc::remapHelps(buffer.data(), 1)) << "usbmux's buffer: direct as it is";
    const Runs high = fixedRuns(Runs{{0x1'0000'4000, 32768}});
    EXPECT_FALSE(xudc::remapHelps(high.data(), 1)) << "the same above 4 GiB: at its fixed IOVA";
}

// What stays bounced once a request is in the window, over random layouts at their fixed IOVAs, both directions; and
// what a mapping costs against the copies it saves. The window puts a request's first page at a 64 KiB line, which can
// move a line into a packet that sat on a packet boundary at the request's own address: a few mappings bounce more
// than they save. The driver maps only from kRemapWorthBytes of bounces; what that keeps of these is printed too.
TEST(XudcIova, OnlyWhatAMappingCannotChangeIsBounced)
{
    std::mt19937_64 rng(13);  // NOLINT(bugprone-random-generator-seed): a fixed seed, so a failure reproduces
    uint64_t before[3] = {};
    uint64_t after[3] = {};
    uint64_t ptes = 0;
    uint64_t mapped = 0;
    uint64_t requests = 0;
    uint64_t worse = 0;
    uint64_t worth = 0;
    int64_t copied = 0;
    int64_t worthCopied = 0;
    for (int c = 0; c < 4000; ++c) {
        const Runs runs = randomRuns(rng);
        const Runs fixed = fixedRuns(runs);
        const bool out = rng() % 2 != 0;
        const uint32_t mps = rng() % 4 ? 512 : 64;
        xudc::PlanStats was = {};
        xudc::PlanStats now = {};
        if (!plan(fixed, mps, out, &was))
            continue;
        ++requests;
        EXPECT_EQ(was.high, 0u) << "case " << c;
        if (!xudc::remapHelps(fixed.data(), static_cast<uint32_t>(fixed.size())) || !was.bounceBytes)
            continue;
        const Runs iova = iovaRuns(runs);
        ASSERT_NE(plan(iova, mps, out, &now), 0u);
        EXPECT_EQ(now.high, 0u) << "case " << c;
        worse += now.bounceBytes > was.bounceBytes ? 1 : 0;
        const uint64_t offset = runs[0].pa & (kPage - 1);
        if (iova.size() == 1 && (offset % mps == 0 || offset + total(runs) <= 65536))
            EXPECT_EQ(now.boundary, 0u) << "case " << c;
        for (size_t i = 0; i + 1 < iova.size(); ++i) {
            EXPECT_TRUE(((iova[i].pa + iova[i].length) & (kPage - 1)) || (iova[i + 1].pa & (kPage - 1)))
                << "case " << c << ": an IOVA break is one inside a page";
        }
        const uint32_t w[3] = {was.discontinuity, was.boundary, was.edge};
        const uint32_t n[3] = {now.discontinuity, now.boundary, now.edge};
        for (int k = 0; k < 3; ++k) {
            before[k] += w[k];
            after[k] += n[k];
        }
        ptes += Mapping(runs).count;
        copied += static_cast<int64_t>(was.bounceBytes) - static_cast<int64_t>(now.bounceBytes);
        ++mapped;
        if (was.bounceBytes >= xudc::kRemapWorthBytes) {
            ++worth;
            worthCopied += static_cast<int64_t>(was.bounceBytes) - static_cast<int64_t>(now.bounceBytes);
        }
    }
    EXPECT_LE(worth, mapped);
    std::printf("XUDC window from %u bounced bytes: %llu requests mapped, %.0f bytes of bounce copies saved each\n",
                xudc::kRemapWorthBytes, static_cast<unsigned long long>(worth),
                worth ? static_cast<double>(worthCopied) / static_cast<double>(worth) : 0.0);
    EXPECT_GT(mapped, 100u);
    EXPECT_LT(worse * 20, mapped);
    EXPECT_GT(copied, 0);
    const auto saved =
        static_cast<int64_t>(before[0] + before[1] + before[2]) - static_cast<int64_t>(after[0] + after[1] + after[2]);
    std::printf("XUDC window: %llu of %llu requests mapped (%llu bounce more), %.1f page-table entries written (and "
                "cleared), %.2f packets and %.0f bytes of bounce copies saved per mapped request; bounced packets "
                "before/after: breaks %llu/%llu, 64K %llu/%llu, edges %llu/%llu\n",
                static_cast<unsigned long long>(mapped), static_cast<unsigned long long>(requests),
                static_cast<unsigned long long>(worse), static_cast<double>(ptes) / static_cast<double>(mapped),
                static_cast<double>(saved) / static_cast<double>(mapped),
                static_cast<double>(copied) / static_cast<double>(mapped), static_cast<unsigned long long>(before[0]),
                static_cast<unsigned long long>(after[0]), static_cast<unsigned long long>(before[1]),
                static_cast<unsigned long long>(after[1]), static_cast<unsigned long long>(before[2]),
                static_cast<unsigned long long>(after[2]));
}

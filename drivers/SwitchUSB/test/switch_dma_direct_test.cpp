// kits/SwitchDMA/SwitchDMADirect.h against the prepare() path it replaces in SwitchXUDCData.cpp admit, over random
// kernel-virtual requests: mbuf-like ranges at odd offsets, ranges that touch in virtual memory, pages that are
// or are not physically adjacent, pages above 4 GiB.
//
// Model: a descriptor is ranges of kernel virtual memory, each 4 KiB page mapped to a physical page. The prepared
// descriptor's getPhysicalSegment(kIOMemoryMapperNone) returns, at an offset, the physical address and the bytes that
// stay physically contiguous inside that range (one IOPL per range); admit()'s loop merges across them. The source
// walk (_kIOMemorySourceSegment) returns the virtual address and the bytes that stay virtually contiguous, ranges that
// touch merged (IOGeneralMemoryDescriptor::getPhysicalSegment).
//
// Checked: the runs are identical, so plan() gives identical TRB pieces for every packet size and direction; the cache
// lines maintained by virtual address are the lines performOperation's physical walk maintains; bounce copies move the
// same bytes; a request with a span outside zone memory, an untranslated page or too many runs is refused (it goes
// through prepare()), and which memory takes which path (switch_dma_direct_access). The work per request is counted:
// the prepare() path creates and commits one UPL per range.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <vector>

#include "kits/SwitchDMA/SwitchDMADirect.h"
#include "../XudcTransfer.h"

namespace {

constexpr uint64_t kPage = kSwitchDmaTranslateBytes;

struct Range {
    uint64_t va, length;
};

struct Request {
    std::vector<Range> ranges;
    std::map<uint64_t, uint64_t> pages;  // virtual page -> physical page
    uint32_t total = 0;

    uint64_t toPhys(uint64_t va) const
    {
        const auto it = pages.find(va & ~(kPage - 1));
        return it == pages.end() ? 0 : it->second + (va & (kPage - 1));
    }

    // Range index and offset in it of a request offset.
    std::pair<size_t, uint64_t> locate(uint32_t offset) const
    {
        size_t i = 0;
        uint64_t at = offset;
        while (i < ranges.size() && at >= ranges[i].length)
            at -= ranges[i++].length;
        return {i, at};
    }

    // The prepared descriptor's getPhysicalSegment.
    uint64_t physicalSegment(uint32_t offset, uint64_t *length) const
    {
        const auto [i, at] = locate(offset);
        if (i == ranges.size())
            return 0;
        const uint64_t va = ranges[i].va + at;
        const uint64_t pa = toPhys(va);
        uint64_t n = 0;
        while (at + n < ranges[i].length && toPhys(va + n) == pa + n)
            n += kPage - ((va + n) & (kPage - 1));
        *length = n < ranges[i].length - at ? n : ranges[i].length - at;
        return pa;
    }

    // _kIOMemorySourceSegment.
    bool sourceSegment(uint32_t offset, switch_dma_span *span) const
    {
        auto [i, at] = locate(offset);
        if (i == ranges.size())
            return false;
        span->va = ranges[i].va + at;
        span->length = ranges[i].length - at;
        for (++i; i < ranges.size() && ranges[i - 1].va + ranges[i - 1].length == ranges[i].va; ++i)
            span->length += ranges[i].length;
        return true;
    }

    auto next() const
    {
        return [this](uint32_t offset, switch_dma_span *span) {
            return sourceSegment(offset, span);
        };
    }
};

// admit()'s prepare() loop.
std::vector<xudc::Run> preparedRuns(const Request &r, uint32_t maxRuns)
{
    std::vector<xudc::Run> runs;
    uint32_t offset = 0;
    while (offset < r.total && runs.size() < maxRuns) {
        uint64_t segment = 0;
        const uint64_t pa = r.physicalSegment(offset, &segment);
        if (!pa || !segment)
            return {};
        const uint32_t take = segment < r.total - offset ? static_cast<uint32_t>(segment) : r.total - offset;
        if (!runs.empty() && runs.back().pa + runs.back().length == pa) {
            runs.back().length += take;
        } else {
            runs.push_back({pa, take});
        }
        offset += take;
    }
    return offset < r.total ? std::vector<xudc::Run>{} : runs;
}

std::vector<xudc::Run> directRuns(const Request &r, uint32_t maxRuns, bool wired = true)
{
    std::vector<xudc::Run> runs(maxRuns);
    const uint32_t n = switch_dma_direct_runs(
        r.next(),
        [wired](uint64_t, uint64_t) {
            return wired;
        },
        [&r](uint64_t va) {
            return r.toPhys(va);
        },
        r.total, runs.data(), maxRuns);
    runs.resize(n);
    return runs;
}

Request randomRequest(std::mt19937_64 &rng, bool high)
{
    Request r;
    std::uniform_int_distribution<int> rangeCount(1, 6);
    std::uniform_int_distribution<int> coin(0, 3);
    uint64_t va = 0xfffffff0'10000000ULL + (rng() % 4096) * kPage;
    uint64_t pa = (high ? 0x1'0000'0000ULL : 0x8000'0000ULL) + (rng() % 65536) * kPage;
    const int ranges = rangeCount(rng);
    for (int i = 0; i < ranges; ++i) {
        const uint64_t length = 1 + rng() % (coin(rng) ? 2048 : 40000);
        if (i && coin(rng) == 0) {
            va += r.ranges.back().length;  // touches the previous range
        } else {
            va += (r.ranges.empty() ? 0 : r.ranges.back().length) + (1 + rng() % 16) * kPage + rng() % kPage;
        }
        r.ranges.push_back({va, length});
        r.total += static_cast<uint32_t>(length);
        for (uint64_t page = va & ~(kPage - 1); page < va + length; page += kPage) {
            if (r.pages.contains(page))
                continue;
            pa = coin(rng) ? pa + kPage : pa + (2 + rng() % 64) * kPage;
            r.pages[page] = pa;
        }
    }
    return r;
}

std::set<uint64_t> physicalLines(const Request &r, uint32_t length)
{
    std::set<uint64_t> lines;
    for (uint32_t offset = 0; offset < length;) {
        uint64_t segment = 0;
        const uint64_t pa = r.physicalSegment(offset, &segment);
        const uint64_t take = segment < length - offset ? segment : length - offset;
        for (uint64_t at = pa & ~63ULL; at < pa + take; at += 64)
            lines.insert(at);
        offset += static_cast<uint32_t>(take);
    }
    return lines;
}

std::set<uint64_t> virtualLines(const Request &r, uint32_t length)
{
    std::set<uint64_t> lines;
    EXPECT_TRUE(switch_dma_for_each_span(r.next(), 0, length, [&](uint64_t va, uint32_t take) {
        for (uint64_t at = va & ~63ULL; at < va + take; at += 64)
            lines.insert(r.toPhys(at));
    }));
    return lines;
}

TEST(SwitchDmaDirect, SameRunsSameTrbs)
{
    std::mt19937_64 rng(17);  // NOLINT(bugprone-random-generator-seed): a fixed seed, so a failure reproduces
    uint64_t requests = 0;
    uint64_t upls = 0;
    for (int layout = 0; layout < 3000; ++layout) {
        const Request r = randomRequest(rng, layout % 7 == 0);
        const auto prepared = preparedRuns(r, 128);
        const auto direct = directRuns(r, 128);
        ASSERT_EQ(prepared.size(), direct.size()) << "layout " << layout;
        for (size_t i = 0; i < prepared.size(); ++i) {
            ASSERT_EQ(prepared[i].pa, direct[i].pa) << "layout " << layout << " run " << i;
            ASSERT_EQ(prepared[i].length, direct[i].length) << "layout " << layout << " run " << i;
        }
        for (const uint32_t mps : {64u, 512u, 1024u}) {
            for (const bool out : {false, true}) {
                std::vector<xudc::Piece> a(253);
                std::vector<xudc::Piece> b(253);
                xudc::PlanStats sa = {};
                xudc::PlanStats sb = {};
                const uint32_t na = xudc::plan(prepared.data(), static_cast<uint32_t>(prepared.size()), r.total, mps,
                                               out, 1ULL << 32, a.data(), 253, &sa);
                const uint32_t nb = xudc::plan(direct.data(), static_cast<uint32_t>(direct.size()), r.total, mps, out,
                                               1ULL << 32, b.data(), 253, &sb);
                ASSERT_EQ(na, nb);
                for (uint32_t i = 0; i < na; ++i) {
                    ASSERT_EQ(a[i].pa, b[i].pa);
                    ASSERT_EQ(a[i].offset, b[i].offset);
                    ASSERT_EQ(a[i].length, b[i].length);
                    ASSERT_EQ(a[i].bounce, b[i].bounce);
                }
            }
        }
        ++requests;
        upls += r.ranges.size();
    }
    std::printf("%llu requests: the prepare() path creates and commits %llu UPLs, the direct path none\n",
                static_cast<unsigned long long>(requests), static_cast<unsigned long long>(upls));
}

TEST(SwitchDmaDirect, SameCacheLines)
{
    std::mt19937_64 rng(29);  // NOLINT(bugprone-random-generator-seed): a fixed seed, so a failure reproduces
    for (int layout = 0; layout < 2000; ++layout) {
        const Request r = randomRequest(rng, false);
        EXPECT_EQ(physicalLines(r, r.total), virtualLines(r, r.total)) << "layout " << layout;
        const uint32_t actual = static_cast<uint32_t>(rng() % (r.total + 1));
        EXPECT_EQ(physicalLines(r, actual), virtualLines(r, actual)) << "layout " << layout << " actual " << actual;
    }
}

TEST(SwitchDmaDirect, SameBytesCopied)
{
    std::mt19937_64 rng(41);  // NOLINT(bugprone-random-generator-seed): a fixed seed, so a failure reproduces
    for (int layout = 0; layout < 500; ++layout) {
        const Request r = randomRequest(rng, false);
        std::map<uint64_t, uint8_t> memory;
        std::vector<uint8_t> expected;
        for (const Range &range : r.ranges) {
            for (uint64_t i = 0; i < range.length; ++i) {
                const uint8_t byte = static_cast<uint8_t>(rng());
                memory[range.va + i] = byte;
                expected.push_back(byte);
            }
        }
        const uint32_t offset = static_cast<uint32_t>(rng() % r.total);
        const uint32_t length = static_cast<uint32_t>(rng() % (r.total - offset + 1));
        std::vector<uint8_t> got;
        ASSERT_TRUE(switch_dma_for_each_span(r.next(), offset, length, [&](uint64_t va, uint32_t take) {
            for (uint32_t i = 0; i < take; ++i)
                got.push_back(memory.at(va + i));
        }));
        ASSERT_EQ(std::vector<uint8_t>(expected.begin() + offset, expected.begin() + offset + length), got);
    }
}

TEST(SwitchDmaDirect, RefusesWhatPrepareMustHandle)
{
    std::mt19937_64 rng(53);  // NOLINT(bugprone-random-generator-seed): a fixed seed, so a failure reproduces
    const Request r = randomRequest(rng, false);
    ASSERT_FALSE(directRuns(r, 128).empty());
    EXPECT_TRUE(directRuns(r, 128, false).empty()) << "memory outside the zone map";
    EXPECT_TRUE(directRuns(r, 1).empty() || preparedRuns(r, 128).size() == 1) << "more runs than the driver holds";
    Request hole = r;
    hole.pages.erase(hole.pages.begin());
    EXPECT_TRUE(directRuns(hole, 128).empty()) << "a page with no translation";
    const Request empty{};
    EXPECT_TRUE(directRuns(empty, 128).empty()) << "a zero-length request has no runs to walk";
}

// The path each kind of memory takes: the zone walk for kernel virtual ranges of zone memory (a wired buffer too, when
// its memory is zone memory), the descriptor's own segments for a buffer wired for its life and for no bytes at all,
// prepare() for the rest.
TEST(SwitchDmaDirect, DecidesThePath)
{
    std::mt19937_64 rng(67);  // NOLINT(bugprone-random-generator-seed): a fixed seed, so a failure reproduces
    const Request r = randomRequest(rng, false);
    Request hole = r;
    hole.pages.erase(hole.pages.begin());

    struct Case {
        const char *what;
        const Request *request;
        bool virtualRanges, wiredBuffer, zone;
        uint32_t maxRuns;
        switch_dma_access path;
    };

    const uint32_t many = static_cast<uint32_t>(preparedRuns(r, 128).size());
    ASSERT_GT(many, 1u);
    const Case cases[] = {
        {"mbufs: kernel virtual ranges of zone memory", &r, true, false, true, 128, SWITCH_DMA_ACCESS_PAGE_TABLES},
        {"a kalloc'd wired buffer", &r, true, true, true, 128, SWITCH_DMA_ACCESS_PAGE_TABLES},
        {"createIOBuffer's contiguous buffer", &r, true, true, false, 128, SWITCH_DMA_ACCESS_SEGMENTS},
        {"kernel memory outside the zone map", &r, true, false, false, 128, SWITCH_DMA_ACCESS_PREPARE},
        {"user memory or a pageable buffer", &r, true, false, false, 128, SWITCH_DMA_ACCESS_PREPARE},
        {"a sub- or multi-range descriptor", &r, false, false, true, 128, SWITCH_DMA_ACCESS_PREPARE},
        {"zone memory with an untranslated page", &hole, true, false, true, 128, SWITCH_DMA_ACCESS_PREPARE},
        {"a wired buffer with an untranslated page", &hole, true, true, true, 128, SWITCH_DMA_ACCESS_SEGMENTS},
        {"zone memory in more runs than the driver holds", &r, true, false, true, many - 1, SWITCH_DMA_ACCESS_PREPARE},
        {"a wired buffer in more runs than the driver holds", &r, true, true, true, many - 1,
         SWITCH_DMA_ACCESS_SEGMENTS},
    };
    for (const Case &c : cases) {
        SCOPED_TRACE(c.what);
        std::vector<xudc::Run> runs(128);
        uint32_t count = 99;
        uint32_t walks = 0;
        const switch_dma_access path = switch_dma_direct_access(
            c.virtualRanges, c.wiredBuffer,
            [&](uint32_t offset, switch_dma_span *span) {
                ++walks;
                return c.request->sourceSegment(offset, span);
            },
            [&c](uint64_t, uint64_t) {
                return c.zone;
            },
            [&c](uint64_t va) {
                return c.request->toPhys(va);
            },
            c.request->total, runs.data(), c.maxRuns, &count);
        EXPECT_EQ(path, c.path);
        if (path == SWITCH_DMA_ACCESS_PAGE_TABLES) {
            runs.resize(count);
            const auto prepared = preparedRuns(*c.request, 128);
            ASSERT_EQ(runs.size(), prepared.size());
            for (size_t i = 0; i < runs.size(); ++i)
                EXPECT_TRUE(runs[i].pa == prepared[i].pa && runs[i].length == prepared[i].length) << "run " << i;
        } else {
            EXPECT_EQ(count, 0u) << "no runs unless they came from the page tables";
        }
        if (!c.virtualRanges)
            EXPECT_EQ(walks, 0u) << "only a virtual-range descriptor's source walk is read";
    }
    uint32_t count = 99;
    std::vector<xudc::Run> runs(1);
    const Request empty;
    EXPECT_EQ(switch_dma_direct_access(
                  true, false, empty.next(),
                  [](uint64_t, uint64_t) {
                      return false;
                  },
                  [](uint64_t) {
                      return uint64_t{0};
                  },
                  0, runs.data(), 1, &count),
              SWITCH_DMA_ACCESS_SEGMENTS)
        << "a zero-length request reaches no memory: nothing to prepare";
    EXPECT_EQ(count, 0u);
}

}  // namespace

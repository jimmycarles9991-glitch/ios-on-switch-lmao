// SPDX-License-Identifier: MIT
#include "kernel_model.h"
#include "empty_pack.h"
#include "predication_model.h"
#include "../arena_trim.h"
#include "../gpu.h"
#include "../srgb_pair.h"

#include <gtest/gtest.h>
#include <sys/mman.h>
#include <unistd.h>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint64_t kGranule = 0x10000, kQuarter = 0x40000;

TEST(ArenaHoles, FreeRangesShrinkToWholeGranules)
{
    gm20b::HeapPlacement p(4 * kQuarter);
    uint64_t a = 0, b = 0, c = 0;
    ASSERT_TRUE(p.place(kQuarter, 256, a));
    ASSERT_TRUE(p.place(kQuarter + 0x100, 256, b));
    ASSERT_TRUE(p.place(0x100, 256, c));
    p.release(a, kQuarter);
    // [0, 1/4) free; b takes [1/4, 1/2 + 0x100); c [1/2 + 0x100, 1/2 + 0x200); the rest free to the end
    const std::vector<gm20b::ArenaRange> holes = gm20b::arenaHoles(p, 4 * kQuarter, kGranule);
    ASSERT_EQ(holes.size(), 2u);
    EXPECT_EQ(holes[0], (gm20b::ArenaRange{0, kQuarter}));
    EXPECT_EQ(holes[1], (gm20b::ArenaRange{2 * kQuarter + kGranule, 2 * kQuarter - kGranule}));
    const std::vector<gm20b::ArenaRange> backed = gm20b::arenaBacked(holes, 4 * kQuarter);
    ASSERT_EQ(backed.size(), 1u);
    EXPECT_EQ(backed[0], (gm20b::ArenaRange{kQuarter, kQuarter + kGranule}));
}

TEST(ArenaHoles, TheTailPastTheHeapsSizeIsAHole)
{
    gm20b::HeapPlacement p(kQuarter + 0x1000);
    uint64_t at = 0;
    ASSERT_TRUE(p.place(kQuarter, 256, at));
    const std::vector<gm20b::ArenaRange> holes = gm20b::arenaHoles(p, kQuarter + kGranule, kGranule);
    ASSERT_EQ(holes.size(), 1u);
    EXPECT_EQ(holes[0], (gm20b::ArenaRange{kQuarter, kGranule}));
}

TEST(ArenaHoles, APlacementHeapHasNone)
{
    gm20b::HeapPlacement p(4 * kQuarter, true);
    EXPECT_TRUE(gm20b::arenaHoles(p, 4 * kQuarter, kGranule).empty());
}

TEST(ArenaHoles, APlacementTakesTheGranulesItTouches)
{
    std::vector<gm20b::ArenaRange> holes = {{0, kQuarter}, {2 * kQuarter, kQuarter}};
    const std::vector<gm20b::ArenaRange> taken = gm20b::takeHoles(holes, 0x100, kGranule, kGranule);
    ASSERT_EQ(taken.size(), 1u);
    EXPECT_EQ(taken[0], (gm20b::ArenaRange{0, 2 * kGranule}));
    ASSERT_EQ(holes.size(), 2u);
    EXPECT_EQ(holes[0], (gm20b::ArenaRange{2 * kGranule, kQuarter - 2 * kGranule}));
    EXPECT_EQ(holes[1], (gm20b::ArenaRange{2 * kQuarter, kQuarter}));
}

// The pages of [p, p + bytes) the process has resident
size_t residentPages(const void *p, size_t bytes)
{
    const size_t page = size_t(getpagesize());
    std::vector<char> in(bytes / page);
    mincore(p, bytes, in.data());
    size_t n = 0;
    for (char c : in)
        n += (c & MINCORE_INCORE) != 0;
    return n;
}

class ArenaTrim : public testing::Test {
protected:
    gm20b::Device dev;

    void SetUp() override
    {
        kernel_model::model = {};
        kernel_model::model.vaEnd = 1ull << 40;
        std::string err;
        ASSERT_TRUE(dev.open(emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
    }

    // The model's live imports at [from, from + bytes): each one's offset from `from` and its bytes
    static std::vector<gm20b::ArenaRange> importsIn(const uint8_t *from, uint64_t bytes)
    {
        std::vector<gm20b::ArenaRange> out;
        for (const auto &[handle, import] : kernel_model::model.imports) {
            (void)handle;
            if (import.first >= uintptr_t(from) && import.first < uintptr_t(from) + bytes)
                out.push_back({import.first - uintptr_t(from), import.second});
        }
        std::sort(out.begin(), out.end(), [](const gm20b::ArenaRange &x, const gm20b::ArenaRange &y) {
            return x.offset < y.offset;
        });
        return out;
    }

    bool submitCopies(gm20b::CommandBuffer *cb, uint32_t *completed = nullptr)
    {
        const size_t firstJob = kernel_model::model.jobs.size();
        uint32_t seqno = 0;
        if (!cb->commit(seqno)) {
            cb->abandon();
            return false;
        }
        if (completed)
            *completed = seqno;
        EXPECT_GT(kernel_model::model.jobs.size(), firstJob);
        predication_model::Gpu gpu(0);
        gpu.map = [](uint64_t va) {
            return kernel_model::model.host(va);
        };
        for (size_t j = firstJob; j < kernel_model::model.jobs.size(); ++j)
            for (const TegraGM20BAbiPush &push : kernel_model::model.jobs[j].pushes) {
                const auto *words =
                    reinterpret_cast<const uint32_t *>(kernel_model::model.host(push.va, uint64_t(push.dwords) * 4));
                EXPECT_NE(words, nullptr);
                if (!words)
                    return false;
                gpu.run(words, push.dwords, true);
            }
        return true;
    }
};

class ArenaTrimParts : public ArenaTrim, public testing::WithParamInterface<bool> {};

TEST_P(ArenaTrimParts, AnEmptyTrimmedArenaRestoresWholeAndRetiresEveryImport)
{
    kernel_model::model.unknownUsesRefused = true;
    uint32_t arena = 0;
    ASSERT_TRUE(dev.createArena(4 * kGranule, GetParam(), arena));
    gm20b::Buffer initial;
    ASSERT_TRUE(dev.arenaBuffer(arena, kGranule, initial));
    uint8_t *const base = initial.cpu;
    const uint64_t va = initial.va;
    dev.arenaRelease(initial);
    std::atomic<uint32_t> committed{0};
    dev.trimArenas(committed);
    EXPECT_TRUE(importsIn(base, 4 * kGranule).empty());
    dev.purgeArena(arena, committed);
    ASSERT_TRUE(dev.arenaPurged(arena));
    ASSERT_TRUE(dev.restoreArena(arena));
    EXPECT_EQ(dev.arenaBytes(), 4 * kGranule);

    gm20b::Buffer narrow;
    ASSERT_TRUE(dev.arenaBuffer(arena, kGranule, narrow));
    EXPECT_EQ(narrow.va, va);
    dev.arenaRelease(narrow);
    gm20b::Buffer wide, destination;
    ASSERT_TRUE(dev.arenaBuffer(arena, 2 * kGranule, wide));
    ASSERT_EQ(wide.va, va);
    std::vector<uint8_t> expected(2 * kGranule);
    for (size_t i = 0; i < expected.size(); ++i)
        expected[i] = uint8_t((i * 13) ^ (i >> 12));
    memcpy(wide.cpu, expected.data(), expected.size());
    ASSERT_TRUE(dev.createBuffer(expected.size(), destination));
    memset(destination.cpu, 0xA5, expected.size());
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    cb->use(wide, false);
    cb->use(destination, true);
    gm20b::BlitEncoder *blit = cb->blit();
    ASSERT_NE(blit, nullptr);
    blit->copyBytes(wide.va, destination.va, expected.size());
    blit->end();
    EXPECT_TRUE(submitCopies(cb)) << dev.kernel().error();
    EXPECT_EQ(memcmp(destination.cpu, expected.data(), expected.size()), 0);
    dev.destroyBuffer(destination);
    dev.arenaRelease(wide);
    dev.destroyArena(arena);
    EXPECT_TRUE(importsIn(base, 4 * kGranule).empty());
}

// A heap kept non-volatile gives back the ranges no resource is placed in: its other parts are imported again with their
// contents, the holes' pages are no longer the process's (nothing wrote them), and a placement into a hole binds it again
TEST_F(ArenaTrim, TheUnplacedRangesGoBackAndComeBackWithAPlacement)
{
    uint32_t arena = 0;
    ASSERT_TRUE(dev.createArena(4 * kQuarter, false, arena));
    gm20b::Buffer a, b, c;
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, a));
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, b));
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, c));
    uint8_t *base = a.cpu;
    memset(a.cpu, 0xA5, kQuarter);
    memset(b.cpu, 0x3C, kQuarter);
    memset(c.cpu, 0x5A, kQuarter);
    ASSERT_EQ(importsIn(base, 4 * kQuarter), (std::vector<gm20b::ArenaRange>{{0, 4 * kQuarter}}));
    dev.arenaRelease(b);

    std::atomic<uint32_t> committed{1};
    dev.trimArenas(committed);
    EXPECT_EQ(importsIn(base, 4 * kQuarter), (std::vector<gm20b::ArenaRange>{{0, 4 * kQuarter}}))
        << "a command buffer committed keeps the arena whole";
    committed = 0;
    dev.trimArenas(committed);
    EXPECT_EQ(importsIn(base, 4 * kQuarter), (std::vector<gm20b::ArenaRange>{{0, kQuarter}, {2 * kQuarter, kQuarter}}));
    EXPECT_EQ(residentPages(base + kQuarter, kQuarter), 0u);
    EXPECT_EQ(residentPages(base + 3 * kQuarter, kQuarter), 0u);
    std::vector<uint8_t> want(kQuarter, 0xA5);
    EXPECT_EQ(memcmp(a.cpu, want.data(), kQuarter), 0);
    want.assign(kQuarter, 0x5A);
    EXPECT_EQ(memcmp(c.cpu, want.data(), kQuarter), 0);
    uint64_t placed = 0, volatileBytes = 0, given = 0;
    dev.arenaUse(placed, volatileBytes, &given);
    EXPECT_EQ(given, 2 * kQuarter);
    EXPECT_EQ(dev.arenaBytes(), 2 * kQuarter);

    gm20b::Buffer d;
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, d));
    EXPECT_EQ(d.cpu, base + kQuarter);
    EXPECT_EQ(importsIn(base, 4 * kQuarter),
              (std::vector<gm20b::ArenaRange>{{0, kQuarter}, {kQuarter, kQuarter}, {2 * kQuarter, kQuarter}}));
    bool bound = false;
    for (const auto &[va, at] : kernel_model::model.binds)
        bound = bound || va == d.va;
    EXPECT_TRUE(bound);
    dev.arenaUse(placed, volatileBytes, &given);
    EXPECT_EQ(given, kQuarter);
    EXPECT_EQ(residentPages(base + kQuarter, kQuarter), 0u) << "binding a hole again writes none of its pages";

    // nothing new to give back: the arena stays as it is
    dev.trimArenas(committed);
    EXPECT_EQ(importsIn(base, 4 * kQuarter).size(), 3u);
}

TEST_P(ArenaTrimParts, ALogicalBufferCrossesRetainedAndRestoredParts)
{
    kernel_model::model.unknownUsesRefused = true;
    const bool privateArena = GetParam();
    uint32_t arena = 0;
    ASSERT_TRUE(dev.createArena(4 * kQuarter, privateArena, arena));
    gm20b::Buffer a, b, c;
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, a));
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, b));
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, c));
    uint8_t *const base = a.cpu;
    const uint64_t firstVa = a.va;
    ASSERT_EQ(a.wc, !privateArena);
    dev.arenaRelease(b);
    std::atomic<uint32_t> committed{0};
    dev.trimArenas(committed);
    ASSERT_EQ(importsIn(base, 4 * kQuarter), (std::vector<gm20b::ArenaRange>{{0, kQuarter}, {2 * kQuarter, kQuarter}}));

    dev.arenaRelease(a);
    gm20b::Buffer d, destination;
    ASSERT_TRUE(dev.arenaBuffer(arena, 2 * kQuarter, d));
    ASSERT_EQ(d.cpu, base);
    ASSERT_EQ(d.va, firstVa);
    ASSERT_EQ(d.wc, !privateArena);
    ASSERT_EQ(d.length, 2 * kQuarter);
    d.hostVisible = true;
    std::vector<uint8_t> expected(2 * kQuarter);
    for (size_t i = 0; i < expected.size(); ++i)
        expected[i] = uint8_t((i * 13) ^ (i >> 12));
    memcpy(d.cpu, expected.data(), expected.size());
    ASSERT_TRUE(dev.createBuffer(expected.size(), destination));
    memset(destination.cpu, 0xA5, expected.size());
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    cb->use(d, false);
    cb->use(destination, true);
    gm20b::BlitEncoder *blit = cb->blit();
    ASSERT_NE(blit, nullptr);
    blit->copyBytes(d.va, destination.va, expected.size());
    blit->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(submitCopies(cb, &seqno)) << dev.kernel().error();
    const gm20b::CpuCompletion completion = dev.captureCpuCompletion(seqno, dev.channelGeneration(), {&d});
    ASSERT_TRUE(completion.valid);
    uint64_t completedBytes = 0;
    for (const auto &range : completion.ranges) {
        EXPECT_NE(kernel_model::model.bos.find(range.handle), kernel_model::model.bos.end());
        completedBytes += range.bytes;
    }
    EXPECT_EQ(completedBytes, d.wc ? 0 : d.length);
    EXPECT_EQ(dev.completeCpu(completion), gm20b::CpuCompletionResult::Current);
    EXPECT_EQ(memcmp(destination.cpu, expected.data(), expected.size()), 0);
    dev.destroyBuffer(destination);
    dev.arenaRelease(d);
    dev.arenaRelease(c);
    dev.destroyArena(arena);
}

TEST_P(ArenaTrimParts, ARecordedLiveBufferSurvivesACrossPartPlacement)
{
    kernel_model::model.unknownUsesRefused = true;
    uint32_t arena = 0;
    ASSERT_TRUE(dev.createArena(4 * kQuarter, GetParam(), arena));
    gm20b::Buffer a, gap, b, c, destination;
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter / 2, a));
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter / 2, gap));
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, b));
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, c));
    uint8_t *const base = a.cpu;
    memset(a.cpu, 0x3C, a.length);
    memset(c.cpu, 0x5A, c.length);
    dev.arenaRelease(b);
    std::atomic<uint32_t> committed{0};
    dev.trimArenas(committed);
    ASSERT_EQ(importsIn(base, 4 * kQuarter), (std::vector<gm20b::ArenaRange>{{0, kQuarter}, {2 * kQuarter, kQuarter}}));
    dev.arenaRelease(gap);
    ASSERT_TRUE(dev.createBuffer(a.length + c.length, destination));
    memset(destination.cpu, 0xA5, destination.length);
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    cb->use(a, false);
    cb->use(c, false);
    cb->use(destination, true);
    gm20b::BlitEncoder *blit = cb->blit();
    ASSERT_NE(blit, nullptr);
    blit->copyBytes(a.va, destination.va, a.length);
    blit->copyBytes(c.va, destination.va + a.length, c.length);
    blit->end();

    gm20b::Buffer d;
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, d));
    ASSERT_EQ(d.cpu, base + kQuarter / 2);
    memset(d.cpu, 0xC3, d.length);
    ASSERT_TRUE(submitCopies(cb)) << dev.kernel().error();
    std::vector<uint8_t> expected(a.length, 0x3C);
    expected.resize(a.length + c.length, 0x5A);
    EXPECT_EQ(memcmp(destination.cpu, expected.data(), expected.size()), 0);
    EXPECT_EQ(memcmp(a.cpu, expected.data(), a.length), 0);
    EXPECT_EQ(memcmp(c.cpu, expected.data() + a.length, c.length), 0);
    dev.destroyBuffer(destination);
    dev.arenaRelease(d);
    dev.arenaRelease(a);
    dev.arenaRelease(c);
    dev.destroyArena(arena);
}

TEST_P(ArenaTrimParts, AFailedCrossPartImportPreservesLiveBuffersAndTheirRecordedUse)
{
    kernel_model::model.unknownUsesRefused = true;
    uint32_t arena = 0;
    ASSERT_TRUE(dev.createArena(4 * kQuarter, GetParam(), arena));
    gm20b::Buffer a, gap, b, c, destination;
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter / 2, a));
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter / 2, gap));
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, b));
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, c));
    uint8_t *const base = a.cpu;
    memset(a.cpu, 0x3C, a.length);
    memset(c.cpu, 0x5A, c.length);
    dev.arenaRelease(b);
    std::atomic<uint32_t> committed{0};
    dev.trimArenas(committed);
    ASSERT_EQ(importsIn(base, 4 * kQuarter), (std::vector<gm20b::ArenaRange>{{0, kQuarter}, {2 * kQuarter, kQuarter}}));
    dev.arenaRelease(gap);
    ASSERT_TRUE(dev.createBuffer(a.length + c.length, destination));
    memset(destination.cpu, 0xA5, destination.length);
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    cb->use(a, false);
    cb->use(c, false);
    cb->use(destination, true);
    gm20b::BlitEncoder *blit = cb->blit();
    ASSERT_NE(blit, nullptr);
    blit->copyBytes(a.va, destination.va, a.length);
    blit->copyBytes(c.va, destination.va + a.length, c.length);
    blit->end();

    gm20b::Buffer d;
    kernel_model::model.refuseImports = 1;
    EXPECT_FALSE(dev.arenaBuffer(arena, kQuarter, d));
    EXPECT_EQ(kernel_model::model.refuseImports, 0u);
    ASSERT_TRUE(submitCopies(cb)) << dev.kernel().error();
    std::vector<uint8_t> expected(a.length, 0x3C);
    expected.resize(a.length + c.length, 0x5A);
    EXPECT_EQ(memcmp(destination.cpu, expected.data(), expected.size()), 0);
    EXPECT_EQ(memcmp(a.cpu, expected.data(), a.length), 0);
    EXPECT_EQ(memcmp(c.cpu, expected.data() + a.length, c.length), 0);
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, d));
    EXPECT_EQ(d.cpu, base + kQuarter / 2);
    dev.destroyBuffer(destination);
    dev.arenaRelease(d);
    dev.arenaRelease(a);
    dev.arenaRelease(c);
    dev.destroyArena(arena);
}

TEST_P(ArenaTrimParts, CpuConsumersFindALiveBufferInARetainedPart)
{
    kernel_model::model.unknownUsesRefused = true;
    uint32_t arena = 0;
    ASSERT_TRUE(dev.createArena(4 * kQuarter, GetParam(), arena));
    gm20b::Buffer a, b, c, source;
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, a));
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, b));
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, c));
    a.hostVisible = true;
    uint8_t *const base = a.cpu;
    memset(a.cpu, 0x3C, a.length);
    memset(c.cpu, 0x5A, c.length);
    dev.arenaRelease(b);
    std::atomic<uint32_t> committed{0};
    dev.trimArenas(committed);
    ASSERT_EQ(importsIn(base, 4 * kQuarter), (std::vector<gm20b::ArenaRange>{{0, kQuarter}, {2 * kQuarter, kQuarter}}));

    gm20b::HostAccess writeLease;
    const bool acquiredWrite = dev.hostAccess(a, a.cpu + 7, 2, true, writeLease);
    EXPECT_TRUE(acquiredWrite);
    if (acquiredWrite) {
        a.cpu[7] = 0x31;
        a.cpu[8] = 0x42;
        ASSERT_TRUE(writeLease.finish());
        EXPECT_EQ(a.cpu[6], 0x3C);
        EXPECT_EQ(a.cpu[9], 0x3C);
    }
    ASSERT_TRUE(dev.createBuffer(a.length, source));
    memset(source.cpu, 0xC3, source.length);
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    cb->use(source, false);
    cb->use(a, true);
    gm20b::BlitEncoder *blit = cb->blit();
    ASSERT_NE(blit, nullptr);
    blit->copyBytes(source.va, a.va, a.length);
    blit->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(submitCopies(cb, &seqno)) << dev.kernel().error();
    const gm20b::CpuCompletion completion = dev.captureCpuCompletion(seqno, dev.channelGeneration(), {&a});
    EXPECT_TRUE(completion.valid);
    if (a.wc) {
        EXPECT_TRUE(completion.ranges.empty());
    } else {
        EXPECT_FALSE(completion.ranges.empty());
        for (const auto &range : completion.ranges)
            EXPECT_NE(kernel_model::model.bos.find(range.handle), kernel_model::model.bos.end());
    }
    EXPECT_EQ(dev.completeCpu(completion), gm20b::CpuCompletionResult::Current);
    gm20b::HostAccess readLease;
    const bool acquiredRead = dev.hostAccess(a, false, readLease);
    EXPECT_TRUE(acquiredRead);
    if (acquiredRead) {
        EXPECT_EQ(memcmp(a.cpu, source.cpu, a.length), 0);
        EXPECT_TRUE(readLease.finish());
    }
    std::vector<uint8_t> untouched(c.length, 0x5A);
    EXPECT_EQ(memcmp(c.cpu, untouched.data(), untouched.size()), 0);
    dev.destroyBuffer(source);
    dev.arenaRelease(a);
    dev.arenaRelease(c);
    dev.destroyArena(arena);
}

TEST_P(ArenaTrimParts, ATextureViewKeepsItsOwnersAcrossACpuRangeAndGpuCopy)
{
    kernel_model::model.unknownUsesRefused = true;
    uint32_t arena = 0;
    ASSERT_TRUE(dev.createArena(4 * kQuarter, GetParam(), arena));
    gm20b::Buffer a, b, c, destination;
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, a));
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, b));
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, c));
    uint8_t *const base = a.cpu;
    dev.arenaRelease(b);
    std::atomic<uint32_t> committed{0};
    dev.trimArenas(committed);
    dev.arenaRelease(a);
    gm20b::Texture texture, view;
    gm20b::TextureAsk ask;
    ask.width = 512;
    ask.height = 256;
    ask.format = 80;
    ask.tiled = false;
    ASSERT_TRUE(dev.arenaTexture({arena, {}}, ask, texture));
    ASSERT_EQ(texture.cpu, base);
    ASSERT_EQ(texture.span.bytes, 2 * kQuarter);
    ASSERT_TRUE(dev.createView(texture, 70, view));
    std::vector<uint8_t> expected(texture.span.bytes, 0x3C);
    memcpy(texture.cpu, expected.data(), expected.size());
    gm20b::HostAccess write;
    ASSERT_TRUE(dev.hostAccess(view, view.cpu + kQuarter - 8, 16, true, write));
    memset(view.cpu + kQuarter - 8, 0xA7, 16);
    const auto pinned = importsIn(base, 4 * kQuarter);
    dev.arenaRelease(c);
    dev.trimArenas(committed);
    EXPECT_EQ(importsIn(base, 4 * kQuarter), pinned);
    ASSERT_TRUE(write.finish());
    dev.trimArenas(committed);
    EXPECT_EQ(importsIn(base, 4 * kQuarter), (std::vector<gm20b::ArenaRange>{{0, 2 * kQuarter}}));
    std::fill(expected.begin() + kQuarter - 8, expected.begin() + kQuarter + 8, 0xA7);
    gm20b::HostAccess read;
    ASSERT_TRUE(dev.hostAccess(view, view.cpu + kQuarter - 9, 18, false, read));
    EXPECT_EQ(memcmp(view.cpu + kQuarter - 9, expected.data() + kQuarter - 9, 18), 0);
    ASSERT_TRUE(read.finish());
    ASSERT_TRUE(dev.createBuffer(expected.size(), destination));
    memset(destination.cpu, 0xA5, destination.length);
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    cb->use(view, false);
    cb->use(destination, true);
    gm20b::BlitEncoder *blit = cb->blit();
    ASSERT_NE(blit, nullptr);
    blit->copyBytes(view.va, destination.va, expected.size());
    blit->end();
    ASSERT_TRUE(submitCopies(cb)) << dev.kernel().error();
    EXPECT_EQ(memcmp(destination.cpu, expected.data(), expected.size()), 0);
    dev.destroyBuffer(destination);
    dev.destroyTexture(view);
    dev.destroyTexture(texture);
    dev.destroyArena(arena);
}

TEST_P(ArenaTrimParts, AnImportRefusalPreservesEveryUnattemptedHole)
{
    kernel_model::model.unknownUsesRefused = true;
    uint32_t arena = 0;
    ASSERT_TRUE(dev.createArena(4 * kQuarter, GetParam(), arena));
    gm20b::Buffer a, b, c;
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, a));
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, b));
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, c));
    uint8_t *const base = a.cpu;
    dev.arenaRelease(b);
    std::atomic<uint32_t> committed{0};
    dev.trimArenas(committed);
    dev.arenaRelease(a);
    dev.arenaRelease(c);
    gm20b::Buffer whole;
    kernel_model::model.refuseImports = 1;
    ASSERT_FALSE(dev.arenaBuffer(arena, 4 * kQuarter, whole));
    ASSERT_EQ(kernel_model::model.refuseImports, 0u);
    ASSERT_TRUE(dev.arenaBuffer(arena, 4 * kQuarter, whole));
    EXPECT_EQ(importsIn(base, 4 * kQuarter),
              (std::vector<gm20b::ArenaRange>{
                  {0, kQuarter}, {kQuarter, kQuarter}, {2 * kQuarter, kQuarter}, {3 * kQuarter, kQuarter}}));
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    cb->use(whole, true);
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno)) << dev.kernel().error();
    dev.arenaRelease(whole);
    dev.destroyArena(arena);
}

TEST_P(ArenaTrimParts, AliasableStorageOutlivesPlacementAndRetiresWithItsLastOwner)
{
    uint32_t arena = 0;
    ASSERT_TRUE(dev.createArena(4 * kQuarter, GetParam(), arena));
    gm20b::Buffer a, b, c, replacement;
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, a));
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, b));
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, c));
    uint8_t *const base = a.cpu;
    const uint64_t va = b.va;
    dev.makeArenaAliasable(b.span, b.lease);
    EXPECT_EQ(dev.arenaUsed(arena), 2 * kQuarter);
    std::atomic<uint32_t> committed{0};
    dev.trimArenas(committed);
    EXPECT_EQ(importsIn(base, 4 * kQuarter), (std::vector<gm20b::ArenaRange>{{0, 4 * kQuarter}}));
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, replacement));
    EXPECT_EQ(replacement.va, va);
    dev.destroyBuffer(b);
    EXPECT_EQ(dev.arenaUsed(arena), 3 * kQuarter);
    dev.destroyBuffer(replacement);
    dev.destroyBuffer(a);
    dev.destroyBuffer(c);
    dev.trimArenas(committed);
    EXPECT_TRUE(importsIn(base, 4 * kQuarter).empty());
    dev.destroyArena(arena);
}

// A placement into a purged arena binds it whole first, under the device's lock, so what it places has storage the CPU
// and a job reach, a texture's new view kind included
TEST_P(ArenaTrimParts, APlacementIntoAPurgedArenaBindsItFirst)
{
    uint32_t arena = 0;
    ASSERT_TRUE(dev.createArena(4 * kQuarter, GetParam(), arena));
    std::atomic<uint32_t> committed{0};
    dev.setArenaVolatile(arena, true);
    dev.purgeArena(arena, committed);
    ASSERT_TRUE(dev.arenaPurged(arena));
    gm20b::Buffer b;
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, b));
    EXPECT_FALSE(dev.arenaPurged(arena));
    EXPECT_EQ(dev.purgedArenas(), 0u);
    ASSERT_TRUE(dev.restoreArena(arena));
    b.hostVisible = true;
    gm20b::HostAccess write;
    EXPECT_TRUE(dev.hostAccess(b, true, write));
    EXPECT_TRUE(write.finish());

    dev.purgeArena(arena, committed);
    ASSERT_TRUE(dev.arenaPurged(arena));
    gm20b::TextureAsk mips;
    mips.kind = gm20b::TextureAsk::Mips;
    mips.width = mips.height = 64;
    mips.levels = 4;
    mips.format = 70;
    gm20b::Texture t;
    ASSERT_TRUE(dev.arenaTexture({arena, std::nullopt}, mips, t));
    EXPECT_FALSE(dev.arenaPurged(arena));
    EXPECT_EQ(dev.purgedArenas(), 0u);
    gm20b::HostAccess read;
    EXPECT_TRUE(dev.hostAccess(t, t.cpu, 64, false, read));
    EXPECT_TRUE(read.finish());
    dev.destroyTexture(t);
    dev.arenaRelease(b);
    dev.destroyArena(arena);
}

// makeAliasable gives back the whole footprint the heap answered for a texture, the stand-in placed beside it included.
// The empty pack holds no extended-range conversion state, so an RG8Unorm_sRGB texture's stand-in, placed in the arena as
// arenaTexture places an extended-range one's, stands in for it.
TEST_P(ArenaTrimParts, MakingATextureAliasableGivesBackItsStandInsPlacementToo)
{
    uint32_t arena = 0;
    ASSERT_TRUE(dev.createArena(4 * kQuarter, GetParam(), arena));
    gm20b::TextureAsk ask;
    ask.width = ask.height = 128;
    ask.format = gm20b::kRG8Srgb;
    ask.tiled = GetParam();
    gm20b::Texture t;
    ASSERT_TRUE(dev.arenaTexture({arena, std::nullopt}, ask, t));
    const gm20b::ArenaAt at{arena, std::nullopt};
    ASSERT_TRUE(dev.createStandIn(t, &at));
    ASSERT_NE(t.standIn, nullptr);
    ASSERT_TRUE(t.standIn->arena);
    ASSERT_EQ(dev.arenaUsed(arena), t.span.bytes + t.standIn->span.bytes);
    dev.makeArenaAliasable(t);
    EXPECT_EQ(dev.arenaUsed(arena), 0u);
    gm20b::Buffer whole;
    ASSERT_TRUE(dev.arenaBuffer(arena, 4 * kQuarter, whole)) << "the stand-in's range is free again";
    dev.destroyTexture(t);
    EXPECT_EQ(dev.arenaUsed(arena), 4 * kQuarter) << "the texture's last drop gives back no placement it had given";
    dev.arenaRelease(whole);
    EXPECT_EQ(dev.arenaUsed(arena), 0u);
    dev.destroyArena(arena);
}

// A private heap's arena is one BO from the kernel's spares, never trimmed (arena.cpp): only a shared heap's has parts
INSTANTIATE_TEST_SUITE_P(Storage, ArenaTrimParts, testing::Values(false), [](const testing::TestParamInfo<bool> &info) {
    return info.param ? "Private" : "Shared";
});

// An arena trimmed to no part (nothing placed in it) has no BO left: freeing it counts none of its bytes out again
// (boot 21's rates lines read pool=17592186044256MB in mediaanalysisd, the counter 160 MB below zero)
TEST_F(ArenaTrim, AnArenaTrimmedToNothingLeavesThePoolCountBalanced)
{
    const uint64_t before = dev.stats.poolBytes.load();
    uint32_t arena = 0;
    ASSERT_TRUE(dev.createArena(4 * kQuarter, false, arena));
    gm20b::Buffer a;
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, a));
    uint8_t *base = a.cpu;
    dev.arenaRelease(a);
    std::atomic<uint32_t> committed{0};
    dev.trimArenas(committed);
    EXPECT_TRUE(importsIn(base, 4 * kQuarter).empty()) << "every range given back";
    EXPECT_EQ(dev.stats.poolBytes.load(), before);
    dev.destroyArena(arena);
    EXPECT_EQ(dev.stats.poolBytes.load(), before);
}

// A volatile heap's whole arena goes at the purge (arena_purge.h), never trimmed
TEST_F(ArenaTrim, AVolatileHeapIsPurgedNotTrimmed)
{
    uint32_t arena = 0;
    ASSERT_TRUE(dev.createArena(4 * kQuarter, false, arena));
    gm20b::Buffer a;
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, a));
    ASSERT_TRUE(!dev.setArenaVolatile(arena, true));
    std::atomic<uint32_t> committed{0};
    dev.trimArenas(committed);
    EXPECT_EQ(importsIn(a.cpu, 4 * kQuarter), (std::vector<gm20b::ArenaRange>{{0, 4 * kQuarter}}));
}

class ArenaIoctlGate {
public:
    explicit ArenaIoctlGate(uint32_t method) : method_(method)
    {
        std::lock_guard<std::mutex> l(mu_);
        active_ = this;
        kernel_model::arriving.store(&arrive, std::memory_order_release);
    }

    ~ArenaIoctlGate()
    {
        release();
        kernel_model::arriving.store(nullptr, std::memory_order_release);
        std::lock_guard<std::mutex> l(mu_);
        if (active_ == this)
            active_ = nullptr;
    }

    bool waitHeld()
    {
        std::unique_lock<std::mutex> l(mu_);
        return cv_.wait_for(l, std::chrono::seconds(5), [&] {
            return held_;
        });
    }

    void release()
    {
        std::lock_guard<std::mutex> l(mu_);
        released_ = true;
        cv_.notify_all();
    }

private:
    static int arrive(uint32_t method, const void *)
    {
        std::unique_lock<std::mutex> l(mu_);
        ArenaIoctlGate *g = active_;
        if (!g || method != g->method_ || g->matched_)
            return KERN_SUCCESS;
        g->matched_ = true;
        g->held_ = true;
        g->cv_.notify_all();
        g->cv_.wait(l, [&] {
            return g->released_;
        });
        return KERN_SUCCESS;
    }

    uint32_t method_ = 0;
    bool matched_ = false, held_ = false, released_ = false;
    std::condition_variable cv_;
    static inline std::mutex mu_;
    static inline ArenaIoctlGate *active_ = nullptr;
};

TEST_F(ArenaTrim, ArenaViewAndBackPlacementDropDeviceLockDuringKernelIoctls)
{
    gm20b::Buffer warm;
    ASSERT_TRUE(dev.createBuffer(4096, warm));
    dev.destroyBuffer(warm);

    {
        uint32_t blArena = 0;
        ASSERT_TRUE(dev.createArena(4 * kQuarter, true, blArena));
        ArenaIoctlGate bindGate(TegraGM20BMethodVmBind);
        gm20b::TextureAsk ask;
        ask.kind = gm20b::TextureAsk::Zeta;
        ask.width = 64;
        ask.height = 64;
        ask.format = 252;
        ask.samples = 1;
        gm20b::Texture depth;
        bool made = false;
        std::thread worker([&] {
            made = dev.arenaTexture({blArena, std::nullopt}, ask, depth);
        });
        const bool held = bindGate.waitHeld();
        EXPECT_TRUE(held);
        if (held) {
            EXPECT_TRUE(dev.lockFree());
            gm20b::Buffer probe;
            ASSERT_TRUE(dev.createBuffer(4096, probe));
            dev.destroyBuffer(probe);
        }
        bindGate.release();
        worker.join();
        ASSERT_TRUE(made);
        dev.destroyTexture(depth);
        dev.destroyArena(blArena);
    }

    {
        uint32_t sharedArena = 0;
        ASSERT_TRUE(dev.createArena(4 * kQuarter, false, sharedArena));
        gm20b::Buffer a, b, c;
        ASSERT_TRUE(dev.arenaBuffer(sharedArena, kQuarter, a));
        ASSERT_TRUE(dev.arenaBuffer(sharedArena, kQuarter, b));
        ASSERT_TRUE(dev.arenaBuffer(sharedArena, kQuarter, c));
        dev.arenaRelease(b);
        std::atomic<uint32_t> committed{0};
        dev.trimArenas(committed);

        ArenaIoctlGate importGate(TegraGM20BMethodBoImport);
        gm20b::Buffer d;
        bool made = false;
        std::thread worker([&] {
            made = dev.arenaBuffer(sharedArena, kQuarter, d);
        });
        const bool held = importGate.waitHeld();
        EXPECT_TRUE(held);
        if (held) {
            EXPECT_TRUE(dev.lockFree());
            gm20b::Buffer probe;
            ASSERT_TRUE(dev.createBuffer(4096, probe));
            dev.destroyBuffer(probe);
        }
        importGate.release();
        worker.join();
        ASSERT_TRUE(made);
        dev.arenaRelease(d);
        dev.arenaRelease(c);
        dev.arenaRelease(a);
        dev.destroyArena(sharedArena);
    }
}

}  // namespace

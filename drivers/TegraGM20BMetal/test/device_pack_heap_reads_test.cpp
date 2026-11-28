// SPDX-License-Identifier: MIT
// A pack hit reads none of PackHeap's pages on the CPU: no process keeps them resident (the GPU draws them from the
// kernel's image), so a read is an SD page-in on the creating thread. The test protects the mapping's heap pages after
// the device opens and counts each first touch.
#include "facade_harness.h"
#include "pack_fixture_image.h"
#include "pack_page_touches.h"
#include "../pack_window.h"

#include <gtest/gtest.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr std::array<const char *, 6> kTier0Keys = {
    "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v v  | f f",
    "samples=2 topo=0 ds=0/0 c0=80:15:1:4:5:0:4:5:0  | v blend_v  | f blend_f",
    "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v tess_v  | f tess_f",
    "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v fetch_v  | f fetch_f",
    "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v var_v  | f var_f",
    "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v stride_v  | f stride_f",
};

TEST(PackHeapReads, ATier0FirstHitAndItsVariantsReadNoHeapPage)
{
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
    GTEST_SKIP() << "the fault counter's signal handler is not TSan's to run";
#endif
#endif
    const std::vector<uint8_t> file = pack_fixture::readFileBytes(pack_fixture::kPath);
    ASSERT_FALSE(file.empty());
    pack_fixture::configurePackImageFromBytes(file, true);
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(pack_fixture::kPath, 0, err, gm20b::Device::Role::Client)) << err;
    ASSERT_GT(dev.packWindowBytes(), 0u);

    const gm20b::Pack &pack = dev.pack();
    const uint8_t *heap = pack.section<uint8_t>(gm20b::PackHeap);
    std::vector<gm20b::Pipeline *> made;
    uint32_t touches = 0;
    {
        pack_touches::PageTouches watch(heap, heap + pack.bytes(gm20b::PackHeap));
        ASSERT_GT(watch.bytes(), 0u);
        for (const char *key : kTier0Keys) {
            const uint64_t h = gm20b::Pack::fnv(key, strlen(key));
            const gm20b::PackRun run = pack.fastRun(h);
            ASSERT_GT(run.records, 0u) << key;
            made.push_back(dev.pipeline(run, h, gm20b::PackForm::Default, false));
            (void)dev.variantPipelines(run, false);
        }
        touches = watch.touches();
    }
    EXPECT_EQ(touches, 0u) << "heap pages the creations read on the CPU";
    for (size_t i = 0; i < made.size(); i++)
        EXPECT_NE(made[i], nullptr) << kTier0Keys[i];
    ASSERT_NE(made[4], nullptr);
    ASSERT_NE(made[4]->variantTable, nullptr) << "the variants' state places its program variants";
    EXPECT_FALSE(made[4]->variantTable->chosen.empty());
}

TEST(PackHeapReads, TheRenderServerWiresTheSegmentsWords)
{
    const std::vector<uint8_t> file = pack_fixture::readFileBytes(pack_fixture::kPath);
    ASSERT_FALSE(file.empty());
    gm20b::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(pack_fixture::kPath, 0, err)) << err;
    uint32_t wordsAt = 0, wordsBytes = 0;
    ASSERT_TRUE(pack.segmentWords(wordsAt, wordsBytes));
    ASSERT_GT(wordsBytes, 0u);
    const gm20b::PackHeader &h = *pack.header();
    const auto *base = reinterpret_cast<const uint8_t *>(pack.header());
    TegraGM20BAbiPackCpuRange ranges[gm20b::kPackWindowMaxCpuRanges] = {};
    const uint32_t count = gm20b::packWindowCollectCpuRanges(base, h, uint64_t(getpagesize()), ranges);
    ASSERT_GT(count, 0u);
    const uint64_t from = uint64_t(uintptr_t(base)) + h.section[gm20b::PackHeap].offset + wordsAt;
    const uint64_t to = from + wordsBytes;
    bool covered = false;
    for (uint32_t i = 0; i < count; i++)
        covered |= ranges[i].address <= from && to <= ranges[i].address + ranges[i].bytes;
    EXPECT_TRUE(covered) << "the segments' words block is among the wired ranges";
}

}  // namespace

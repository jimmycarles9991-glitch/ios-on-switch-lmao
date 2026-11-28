// SPDX-License-Identifier: MIT
// A named variant of a tier-0 state is placed where a draw first selects it, as a tier-0 hit is, so a cheap name never
// queues behind the placer's card reads; any other goes to the placer, whose placement of a tier-1 record reads its
// pack pages before Device::lock_ (pack_preread.h), so the import under the lock wires no page left unread.
#include "device_harness.h"
#include "pack_fixture_image.h"
#include "pack_page_touches.h"

#include <gtest/gtest.h>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr const char *kTier0Key = "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v stride_v  | f stride_f";
constexpr const char *kTier1Key = "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v cold_v  | f cold_f";
constexpr const char *kVariant = " | tests=2";

gm20b::PackRun runOf(const gm20b::Device &dev, const char *key)
{
    return dev.pack().fastRun(gm20b::Pack::fnv(key, strlen(key)));
}

class PackVariantsInline : public testing::Test {
protected:
    gm20b::Device dev;

    void SetUp() override
    {
        const std::vector<uint8_t> file = pack_fixture::readFileBytes(pack_fixture::kPath);
        ASSERT_FALSE(file.empty());
        pack_fixture::configurePackImageFromBytes(file, true);
        std::string err;
        ASSERT_TRUE(dev.open(pack_fixture::kPath, 0, err, gm20b::Device::Role::Client)) << err;
    }
};

TEST_F(PackVariantsInline, ATier0StatesVariantIsPlacedWithoutThePlacer)
{
    const gm20b::PackRun run = runOf(dev, kTier0Key);
    ASSERT_GT(run.records, 1u);
    gm20b::Pipeline *inline_ = dev.variantPipelineLockFree(run, kVariant, false);
    ASSERT_NE(inline_, nullptr);
    EXPECT_EQ(dev.variantPipeline(run, kVariant, false), inline_) << "the placer finds the same pipeline";
    EXPECT_EQ(dev.variantPipelineLockFree(run, " | tests=3", false), nullptr) << "a name the pack lacks";
}

TEST_F(PackVariantsInline, ATier1StatesVariantGoesToThePlacerAndIsReadBeforeTheLock)
{
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
    GTEST_SKIP() << "the fault counter's signal handler is not TSan's to run";
#endif
#endif
    const gm20b::PackRun run = runOf(dev, kTier1Key);
    ASSERT_GT(run.records, 1u);
    const gm20b::Pack &pack = dev.pack();
    const uint8_t *heap = pack.section<uint8_t>(gm20b::PackHeap);
    const std::map<uint64_t, std::pair<uint64_t, uint64_t>> before = kernel_model::model.imports;
    pack_touches::PageTouches watch(heap, heap + pack.bytes(gm20b::PackHeap));
    EXPECT_EQ(dev.variantPipelineLockFree(run, kVariant, false), nullptr);
    EXPECT_EQ(watch.touches(), 0u) << "the drawing thread read a tier-1 page";
    ASSERT_NE(dev.variantPipeline(run, kVariant, false), nullptr);
    uint32_t imports = 0, untouched = 0;
    const uintptr_t page = uintptr_t(getpagesize());
    for (const auto &[handle, range] : kernel_model::model.imports) {
        if (before.count(handle))
            continue;
        imports++;
        for (uintptr_t at = range.first; at < range.first + range.second; at += page)
            if (watch.watched(at) && !watch.touched(at)) {
                untouched++;
                break;
            }
    }
    EXPECT_GT(imports, 0u) << "its programs run in place from the pack's pages";
    EXPECT_EQ(untouched, 0u) << "imports of grains no read before the lock brought in";
}

}  // namespace

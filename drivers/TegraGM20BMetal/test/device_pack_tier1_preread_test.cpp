// SPDX-License-Identifier: MIT
// A state outside tier 0 reads its record's pack pages on the creating thread before Device::lock_
// (pack_preread.h), so that under the lock its placement touches no page the pre-read did not and imports no grain of
// its programs the pre-read left unread: those would be card reads with every encoder waiting on the lock. The watch
// covers PackHeap; the fixture's tier-1 table entries share their pages with tier 0's.
#include "device_harness.h"
#include "pack_fixture_image.h"
#include "pack_page_touches.h"
#include "../pack_preread.h"

#include <gtest/gtest.h>
#include <mach/vm_page_size.h>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr const char *kTier1Key = "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v cold_v  | f cold_f";

struct Placed {
    gm20b::Pipeline *pipeline = nullptr;
    uint32_t touchesUnderLock = 0;
    uint32_t untouchedImports = 0, imports = 0;
};

Placed placeTier1(bool preread)
{
    const std::vector<uint8_t> file = pack_fixture::readFileBytes(pack_fixture::kPath);
    EXPECT_FALSE(file.empty());
    pack_fixture::configurePackImageFromBytes(file, true);
    gm20b::Device dev;
    std::string err;
    EXPECT_TRUE(dev.open(pack_fixture::kPath, 0, err, gm20b::Device::Role::Client)) << err;
    const gm20b::Pack &pack = dev.pack();
    const uint64_t h = gm20b::Pack::fnv(kTier1Key, strlen(kTier1Key));
    const gm20b::PackRun run = pack.fastRun(h);
    const gm20b::PackPipeline *pp = run.records ? pack.find(run) : nullptr;
    Placed out;
    if (!pp)
        return out;
    EXPECT_GE(uint64_t(pp - pack.section<gm20b::PackPipeline>(gm20b::PackPipelines)),
              pack.tier0Entries(gm20b::PackPipelines, sizeof(gm20b::PackPipeline)))
        << "the record is past tier 0";
    const uint8_t *heap = pack.section<uint8_t>(gm20b::PackHeap);
    const std::map<uint64_t, std::pair<uint64_t, uint64_t>> before = kernel_model::model.imports;
    pack_touches::PageTouches watch(heap, heap + pack.bytes(gm20b::PackHeap));
    EXPECT_GT(watch.bytes(), 0u);
    if (preread)
        gm20b::prereadPackRecord(pack, *pp, uint64_t(getpagesize()));
    const uint32_t touchedBefore = watch.touches();
    out.pipeline = gm20b::DeviceHarness::placeUnderLock(dev, *pp);
    out.touchesUnderLock = watch.touches() - touchedBefore;
    for (const auto &[handle, range] : kernel_model::model.imports) {
        if (before.count(handle))
            continue;
        out.imports++;
        const uintptr_t page = uintptr_t(getpagesize());
        for (uintptr_t at = range.first; at < range.first + range.second; at += page)
            if (watch.watched(at) && !watch.touched(at)) {
                out.untouchedImports++;
                break;
            }
    }
    return out;
}

TEST(PackTier1Preread, UnderTheLockAPlacementReadsNothingThePrereadDidNot)
{
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
    GTEST_SKIP() << "the fault counter's signal handler is not TSan's to run";
#endif
#endif
    const Placed placed = placeTier1(true);
    ASSERT_NE(placed.pipeline, nullptr);
    EXPECT_GT(placed.imports, 0u) << "the tier-1 programs run in place from the pack's pages";
    EXPECT_EQ(placed.touchesUnderLock, 0u) << "heap pages first read under Device::lock_";
    EXPECT_EQ(placed.untouchedImports, 0u) << "imports of program grains the pre-read left unread";
}

TEST(PackTier1Preread, WithoutThePrereadThePlacementReadsUnderTheLock)
{
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
    GTEST_SKIP() << "the fault counter's signal handler is not TSan's to run";
#endif
#endif
    const Placed placed = placeTier1(false);
    ASSERT_NE(placed.pipeline, nullptr);
    EXPECT_GT(placed.touchesUnderLock + placed.untouchedImports, 0u)
        << "the watch sees what a placement reads; without the pre-read it must see the lock's reads";
}

}  // namespace

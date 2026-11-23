// SPDX-License-Identifier: BSD-2-Clause-Patent
// Window B's slots placed in its IOVA space: a decoder's pool maps each buffer once and keeps it mapped, room is made
// from idle mappings in least recently used order and never from one a set uses, and a slot whose allocation the
// backing registry freed goes at the next latch.
#include "switch_native_scanout_harness.h"
#include <atomic>
#include <memory>
#include <set>
#include <thread>

namespace {
// The fixture's picture: 1536x736 NV12, both planes, in whole 64 KiB granules.
constexpr uint64_t kPicture = 1536 * 736 * 3 / 2, kPictureSpan = TegraSMMUSpace::round(kPicture);

using Shipped = Display<>;
using FourPictures = Display<8, 4 * kPictureSpan>;

struct Played {
    unsigned maps, mapsAfterFirstCycle, remaps;
};

class PlaneSpace : public NativeScanoutFixture {
protected:
    // One frame of allocation `id`: placed, shown alone, and every other lease given back at its latch.
    template <class D> int frame(D &d, uint64_t id, uint64_t content)
    {
        nextNative.allocation.allocation_id = id;
        nextNative.allocation.content_generation = content;
        const int slot = admit(d);
        if (slot < 0)
            return slot;
        EXPECT_EQ(image.readHigh - image.readLow, kPicture);
        for (bool &busy : d.busy)
            busy = false;
        d.busy[slot] = true;
        d.planeReleaseIdle();
        return slot;
    }

    // A decoder cycling `pool` buffers in order for `frames` frames.
    Played play(unsigned pool, unsigned frames)
    {
        auto d = std::make_unique<Shipped>();
        Played played = {};
        for (unsigned f = 0; f < frames; ++f) {
            const unsigned before = d->probeDomain.maps;
            EXPECT_GE(frame(*d, 100 + f % pool, f + 1), 0) << f;
            if (f >= pool)
                played.mapsAfterFirstCycle += d->probeDomain.maps - before;
            EXPECT_EQ(leases, 1u) << f;
        }
        d->expectSpaceConsistent();
        played.maps = d->probeDomain.maps;
        played.remaps = d->stPlaneRemaps;
        EXPECT_EQ(d->probeDomain.unmaps, 0u);
        return played;
    }

    template <class D> static int slotOf(const D &d, uint64_t id)
    {
        for (unsigned k = 0; k < D::kPlaneSlots; ++k)
            if (d.planeSlots[k].iova && d.planeSlots[k].mappedAllocation == id)
                return int(k);
        return -1;
    }
};

// Boot 22's display.planes: a 1080p video's decoder cycled 14-16 buffers and every latched frame made a mapping.
TEST_F(PlaneSpace, APoolOfSixteenMapsEachBufferOnce)
{
    const Played played = play(16, 48);
    EXPECT_EQ(played.maps, 16u);
    EXPECT_EQ(played.mapsAfterFirstCycle, 0u);
    EXPECT_EQ(played.remaps, 0u);
}

TEST_F(PlaneSpace, APoolOfThirtyTwoMapsEachBufferOnce)
{
    const Played played = play(32, 96);
    EXPECT_EQ(played.maps, 32u);
    EXPECT_EQ(played.mapsAfterFirstCycle, 0u);
    EXPECT_EQ(played.remaps, 0u);
}

TEST_F(PlaneSpace, AFullSpaceDropsTheLeastRecentlyUsedIdleMappingAndReusesItsIova)
{
    auto d = std::make_unique<FourPictures>();
    for (uint64_t id = 1; id <= 4; ++id)
        ASSERT_GE(frame(*d, id, id), 0);
    EXPECT_EQ(d->planeSpace.bytesFree(), 0u);
    const uint32_t first = d->planeSlots[slotOf(*d, 1)].iova, second = d->planeSlots[slotOf(*d, 2)].iova;
    ASSERT_GE(frame(*d, 5, 5), 0);
    EXPECT_EQ(slotOf(*d, 1), -1);
    EXPECT_EQ(d->planeSlots[slotOf(*d, 5)].iova, first);
    EXPECT_EQ(d->probeDomain.iova, first);
    EXPECT_EQ(d->probeDomain.unmaps, 1u);
    EXPECT_EQ(d->stPlaneRemaps, 0u);
    d->expectSpaceConsistent();
    ASSERT_GE(frame(*d, 1, 6), 0);
    EXPECT_EQ(d->planeSlots[slotOf(*d, 1)].iova, second);
    EXPECT_EQ(d->stPlaneRemaps, 1u);
    d->expectSpaceConsistent();
}

TEST_F(PlaneSpace, ASlotASetUsesIsNeverDroppedForRoom)
{
    auto d = std::make_unique<FourPictures>();
    for (uint64_t id = 1; id <= 4; ++id)
        ASSERT_GE(frame(*d, id, id), 0);
    const int one = slotOf(*d, 1), two = slotOf(*d, 2);
    const uint32_t oneIova = d->planeSlots[one].iova, twoIova = d->planeSlots[two].iova;
    for (bool &busy : d->busy)
        busy = false;
    d->busy[one] = d->busy[two] = true;
    nextNative.allocation.allocation_id = 5;
    const int five = admit(*d);
    ASSERT_GE(five, 0);
    EXPECT_EQ(d->planeSlots[one].iova, oneIova);
    EXPECT_EQ(d->planeSlots[two].iova, twoIova);
    EXPECT_EQ(slotOf(*d, 3), -1);
    d->expectSpaceConsistent();
    for (unsigned k = 0; k < FourPictures::kPlaneSlots; ++k)
        d->busy[k] = d->planeSlots[k].iova != 0;
    const unsigned unmaps = d->probeDomain.unmaps, logged = ioLogLines;
    nextNative.allocation.allocation_id = 6;
    EXPECT_EQ(admit(*d), -11);
    EXPECT_EQ(d->probeDomain.unmaps, unmaps);
    EXPECT_EQ(ioLogLines, logged + 1);
    nextNative.allocation.allocation_id = 7;
    EXPECT_EQ(admit(*d), -11);
    EXPECT_EQ(ioLogLines, logged + 1);
    d->expectSpaceConsistent();
    for (bool &busy : d->busy)
        busy = false;
    d->planeReleaseIdle();
}

TEST_F(PlaneSpace, ANewPageListGenerationMapsAgain)
{
    auto d = std::make_unique<Shipped>();
    ASSERT_GE(frame(*d, 10, 1), 0);
    ++nextNative.allocation.allocation_generation;
    ASSERT_GE(frame(*d, 10, 2), 0);
    EXPECT_EQ(d->probeDomain.maps, 2u);
    EXPECT_EQ(d->stPlaneRemaps, 0u);
    d->expectSpaceConsistent();
}

TEST_F(PlaneSpace, AGpuSurfaceTakesItsIovaFromTheSameSpaceAndGivesItBack)
{
    auto d = std::make_unique<Shipped>();
    IOMemoryDescriptor memory;
    memory.length = 3 * TegraSMMUSpace::kGranule;
    const int k = d->planeSlotFor(&memory, 0, memory.length, false);
    ASSERT_GE(k, 0);
    EXPECT_EQ(d->planeSpace.bytesTotal() - d->planeSpace.bytesFree(), memory.length);
    EXPECT_EQ(d->probeDomain.pages.at(d->planeSlotIova(k) + 4096), memory.pa + 4096);
    EXPECT_EQ(memory.refs, 2u);
    d->expectSpaceConsistent();
    d->windowIdle = true;
    d->planeReleaseIdle();
    EXPECT_EQ(memory.completes, 1u);
    EXPECT_EQ(memory.refs, 1u);
    EXPECT_EQ(d->planeSpace.bytesFree(), d->planeSpace.bytesTotal());
    memory.hole = 2;
    EXPECT_EQ(d->planeSlotFor(&memory, 0, memory.length, false), -1);
    EXPECT_EQ(memory.completes, memory.prepares);
    EXPECT_EQ(d->planeSpace.bytesFree(), d->planeSpace.bytesTotal());
    d->expectSpaceConsistent();
}

// The slot maps only the pages the memory has; the next frame's range, the same one, finds that slot again.
TEST_F(PlaneSpace, ARangePastTheSurfacesMemoryIsMappedOnce)
{
    auto d = std::make_unique<Shipped>();
    IOMemoryDescriptor memory;
    memory.length = 2 * TegraSMMUSpace::kGranule;
    const uint64_t high = 3 * TegraSMMUSpace::kGranule;
    const int k = d->planeSlotFor(&memory, 0, high, false);
    ASSERT_GE(k, 0);
    EXPECT_EQ(d->planeSlots[k].high, memory.length);
    EXPECT_EQ(d->planeSlotFor(&memory, 0, high, false), k);
    EXPECT_EQ(memory.prepares, 1u);
    EXPECT_EQ(d->probeDomain.unmaps, 0u);
    d->expectSpaceConsistent();
    d->windowIdle = true;
    d->planeReleaseIdle();
    EXPECT_EQ(memory.completes, 1u);
    EXPECT_EQ(d->planeSpace.bytesFree(), d->planeSpace.bytesTotal());
}

TEST_F(PlaneSpace, AFreedAllocationsSlotGoesAtTheNextLatch)
{
    auto d = std::make_unique<Shipped>();
    const int slot = frame(*d, 10, 1);
    ASSERT_GE(slot, 0);
    ASSERT_EQ(noted.size(), 1u);
    EXPECT_EQ(noted[0].first, nextNative.view.native);
    EXPECT_EQ(noted[0].second, uint32_t(SWITCH_GPU_ENGINE_DISPLAY));
    d->busy[slot] = false;
    d->planeReleaseIdle();
    EXPECT_EQ(leases, 0u);
    ASSERT_NE(d->planeSlotIova(slot), 0u);
    Shipped::planeForgotten(10);
    d->planeReleaseIdle();
    EXPECT_EQ(d->planeSlots[slot].iova, 0u);
    EXPECT_EQ(d->probeDomain.unmaps, 1u);
    EXPECT_EQ(d->planeSpace.bytesFree(), d->planeSpace.bytesTotal());
    d->expectSpaceConsistent();
}

TEST_F(PlaneSpace, AFreedAllocationASetStillUsesGoesOnceNoneDoes)
{
    auto d = std::make_unique<Shipped>();
    const int slot = frame(*d, 10, 1);
    ASSERT_GE(slot, 0);
    Shipped::planeForgotten(10);
    d->planeReleaseIdle();
    EXPECT_NE(d->planeSlots[slot].iova, 0u);
    EXPECT_TRUE(d->planeSlots[slot].forgotten);
    d->busy[slot] = false;
    d->planeReleaseIdle();
    EXPECT_EQ(d->planeSlots[slot].iova, 0u);
    EXPECT_EQ(leases, 0u);
}

// Only an allocation a slot maps is queued, so the ring holds one for every slot, however many the display ever noted.
TEST_F(PlaneSpace, TheRetirementThreadQueuesOnlyWhatASlotMaps)
{
    auto d = std::make_unique<Shipped>();
    for (uint64_t id = 1; id <= Shipped::kPlaneSlots; ++id)
        ASSERT_GE(frame(*d, id, id), 0);
    for (bool &busy : d->busy)
        busy = false;
    const unsigned logged = ioLogLines;
    for (uint64_t id = 1000; id < 1500; ++id)
        Shipped::planeForgotten(id);
    for (uint64_t id = 1; id <= Shipped::kPlaneSlots; ++id)
        Shipped::planeForgotten(id);
    d->planeReleaseIdle();
    EXPECT_EQ(d->planeSpace.bytesFree(), d->planeSpace.bytesTotal());
    EXPECT_EQ(d->probeDomain.pages.size(), 0u);
    EXPECT_EQ(ioLogLines, logged);
}

// The retirement thread queues while the gate drains: every ID arrives once.
TEST(PlaneForgets, OneProducerAndOneConsumerLoseNothing)
{
    constexpr unsigned kSlots = 48, kRounds = 2000;
    auto forgets = std::make_unique<switch_display::PlaneForgets<kSlots>>();
    std::atomic<unsigned> round{0};
    std::thread producer([&] {
        for (unsigned r = 1; r <= kRounds; ++r) {
            while (round.load(std::memory_order_acquire) != r)
                std::this_thread::yield();
            for (unsigned k = 0; k < kSlots; ++k)
                forgets->forget(uint64_t(r) * kSlots + (k * 7) % kSlots);
        }
    });
    for (unsigned r = 1; r <= kRounds; ++r) {
        for (unsigned k = 0; k < kSlots; ++k)
            forgets->publish(k, uint64_t(r) * kSlots + k);
        round.store(r, std::memory_order_release);
        std::set<uint64_t> seen;
        while (seen.size() < kSlots) {
            forgets->drain([&](uint64_t id) {
                EXPECT_TRUE(seen.insert(id).second) << id;
                forgets->publish(unsigned(id % kSlots), 0);
            });
        }
        EXPECT_EQ(*seen.begin(), uint64_t(r) * kSlots);
        EXPECT_EQ(*seen.rbegin(), uint64_t(r) * kSlots + kSlots - 1);
    }
    producer.join();
}
}  // namespace

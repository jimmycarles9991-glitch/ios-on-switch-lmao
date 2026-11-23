// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "switch_nvjpg_engine_model.h"
#include <new>
#define IONewZero(type, count)             new (std::nothrow) type[count]()
#define IONewData(type, count)             new (std::nothrow) type[count]
#define IODelete(pointer, type, count)     delete[] (pointer)
#define IODeleteData(pointer, type, count) delete[] (pointer)
#include "../TegraNVJPGMapping.cpp"

void switch_nvjpg_count_mapping(int)
{}

struct switch_gpu_backing {
    switch_gpu_backing_info info;
    void *context = nullptr;
    void (*destroy)(void *) = nullptr;
};

extern "C" void *switch_gpu_backing_mapping_context(const switch_gpu_backing *backing, uint32_t engine)
{
    EXPECT_EQ(engine, uint32_t(SWITCH_GPU_ENGINE_NVJPG));
    return backing ? backing->context : nullptr;
}

extern "C" void switch_gpu_backing_describe(switch_gpu_backing *backing, switch_gpu_backing_info *info)
{
    *info = backing->info;
}

extern "C" int switch_gpu_backing_mapping_install(switch_gpu_backing *backing, uint32_t engine, uint64_t generation,
                                                  void *context, void (*destroyContext)(void *))
{
    EXPECT_EQ(engine, uint32_t(SWITCH_GPU_ENGINE_NVJPG));
    EXPECT_EQ(generation, backing->info.allocation_generation);
    EXPECT_EQ(backing->context, nullptr);
    backing->context = context;
    backing->destroy = destroyContext;
    return 0;
}

namespace {
uint64_t gNextScanPa = 0xa0000000;
bool gScanAllocFails;
uint64_t gWatchedAddress;  // a scan mapping that must be gone before its pages are
int gFreedWhileMapped;

switch_gpu_backing *gRetiring;
bool gRetiredDuringRun;

switch_gpu_backing backingOf(const std::vector<uint64_t> &pages, uint64_t generation)
{
    switch_gpu_backing backing{};
    backing.info.allocation_id = generation;
    backing.info.allocation_generation = generation;
    backing.info.pages = pages.data();
    backing.info.page_count = static_cast<uint32_t>(pages.size());
    return backing;
}

SwitchNVJPGMapping *mapped(switch_gpu_backing &backing, uint32_t extent, uint64_t writeEnd)
{
    const char *why = nullptr;
    SwitchNVJPGMapping *mapping = switch_nvjpg_mapping(switch_nvjpg_backing(&backing), extent, writeEnd, &why);
    EXPECT_EQ(why, nullptr);
    return mapping;
}

// The GPU driver's retirement worker, run while the engine thread is inside a decode: the first time the decode takes
// gStateLock with the job lock held.
void retireDuringTheRun(IOLock *lock)
{
    if (lock != gStateLock || !gRetiring || !gJobLock->locked)
        return;
    switch_gpu_backing *backing = gRetiring;
    gRetiring = nullptr;
    backing->destroy(backing->context);
    gRetiredDuringRun = true;
}
}  // namespace

bool switch_dma_pages_alloc(switch_dma_pages *out, size_t size, IOOptionBits)
{
    *out = {};
    if (gScanAllocFails)
        return false;
    const uint32_t count = static_cast<uint32_t>((size + 4095) / 4096);
    out->pages = new uint64_t[count];
    for (uint32_t i = 0; i < count; ++i)
        out->pages[i] = gNextScanPa + i * 4096ULL;
    gNextScanPa += count * 4096ULL;
    out->va = new uint8_t[count * 4096ULL];
    out->count = count;
    out->size = count * 4096ULL;
    return true;
}

void switch_dma_pages_free(switch_dma_pages *buffer)
{
    if (gWatchedAddress && gDomain.translate(gWatchedAddress) != UINT64_MAX)
        ++gFreedWhileMapped;
    delete[] buffer->pages;
    delete[] buffer->va;
    *buffer = {};
}

TEST_F(NVJPGGate, RepeatedDecodesUseTheBackingsMappingWithoutLocksOrTableWrites)
{
    const std::vector<uint64_t> pages{0x91000000, 0x92000000};
    switch_gpu_backing backing = backingOf(pages, 2);
    gSmmuRangeFlushes.clear();
    SwitchNVJPGMapping *mapping = mapped(backing, 2, 2 * 4096 + 100);
    ASSERT_NE(mapping, nullptr);
    EXPECT_EQ(mapping->pages, pages.data());
    EXPECT_EQ(mapping->count, 2u);
    EXPECT_EQ(mapping->mapped, 3u);
    EXPECT_EQ(gDomain.translate(mapping->address), pages[0]);
    EXPECT_EQ(gDomain.translate(mapping->address + 4096), pages[1]);
    EXPECT_EQ(gDomain.translate(mapping->address + 8192), gSink.pa);
    EXPECT_EQ(gDomain.translate(mapping->address + uint64_t(mapping->mapped) * 4096), UINT64_MAX);
    ASSERT_EQ(gSmmuRangeFlushes.size(), 1u);
    switch_test_lock_taken = +[](IOLock *) {
        ADD_FAILURE() << "retained mapping took a lock";
    };
    for (unsigned i = 0; i < 20; ++i)
        EXPECT_EQ(mapped(backing, 2, 2 * 4096 + 100 - i), mapping);
    switch_test_lock_taken = nullptr;
    EXPECT_EQ(gSmmuRangeFlushes.size(), 1u);
    const uint64_t address = mapping->address;
    backing.destroy(backing.context);
    EXPECT_EQ(gDomain.translate(address), UINT64_MAX);
    ASSERT_EQ(gSmmuRangeFlushes.size(), 2u);
}

TEST_F(NVJPGGate, ShorterAliasesPutMcuPaddingInTheSinkInsteadOfAdjacentBackingPages)
{
    const std::vector<uint64_t> pages{0x94000000, 0x95000000};
    switch_gpu_backing backing = backingOf(pages, 4);
    SwitchNVJPGMapping *shorter = mapped(backing, 1, 2 * 4096);
    SwitchNVJPGMapping *whole = mapped(backing, 2, 2 * 4096);
    ASSERT_NE(shorter, nullptr);
    ASSERT_NE(whole, nullptr);
    EXPECT_NE(shorter->address, whole->address);
    EXPECT_EQ(gDomain.translate(shorter->address + 4096), gSink.pa);
    EXPECT_EQ(gDomain.translate(whole->address + 4096), pages[1]);
    const size_t flushes = gSmmuRangeFlushes.size();
    switch_test_lock_taken = +[](IOLock *) {
        ADD_FAILURE() << "retained alias mapping took a lock";
    };
    EXPECT_EQ(mapped(backing, 1, 2 * 4096), shorter);
    EXPECT_EQ(mapped(backing, 2, 2 * 4096), whole);
    switch_test_lock_taken = nullptr;
    EXPECT_EQ(gSmmuRangeFlushes.size(), flushes);
    const uint64_t first = shorter->address, second = whole->address;
    backing.destroy(backing.context);
    EXPECT_EQ(gDomain.translate(first), UINT64_MAX);
    EXPECT_EQ(gDomain.translate(second), UINT64_MAX);
}

// The sink holds what the decode's plan writes past the destination's pages, no more: a picture that ends inside its
// pages maps none, and one that needs more padding later gets a longer alias, which shorter needs then reuse.
TEST_F(NVJPGGate, TheSinkCoversOnlyThePaddingThePlanWrites)
{
    const std::vector<uint64_t> pages{0x96000000, 0x96001000, 0x96002000};
    switch_gpu_backing backing = backingOf(pages, 6);
    SwitchNVJPGMapping *exact = mapped(backing, 3, 3 * 4096);
    ASSERT_NE(exact, nullptr);
    EXPECT_EQ(exact->mapped, 3u);
    EXPECT_EQ(gDomain.translate(exact->address + 3 * 4096), UINT64_MAX);
    SwitchNVJPGMapping *padded = mapped(backing, 3, 5 * 4096 - 1);
    ASSERT_NE(padded, nullptr);
    EXPECT_NE(padded, exact);
    EXPECT_EQ(padded->mapped, 5u);
    EXPECT_EQ(gDomain.translate(padded->address + 4 * 4096), gSink.pa);
    EXPECT_EQ(gDomain.translate(padded->address + 5 * 4096), UINT64_MAX);
    EXPECT_EQ(mapped(backing, 3, 4 * 4096), padded);
    EXPECT_EQ(mapped(backing, 3, 2 * 4096), exact);
    backing.destroy(backing.context);
    EXPECT_EQ(gSpaces[TEGRA_NVJPG_WINDOW_OUT].bytesFree(), gSpaces[TEGRA_NVJPG_WINDOW_OUT].bytesTotal());
}

// A full output window is a refusal with its own reason (the service hands the image to the CPU), and installs nothing
// on the backing.
TEST_F(NVJPGGate, AFullOutputWindowIsANamedRefusal)
{
    TegraSMMUSpace &space = gSpaces[TEGRA_NVJPG_WINDOW_OUT];
    const uint64_t all = space.bytesFree();
    const uint64_t hog = space.alloc(all);
    ASSERT_NE(hog, 0u);
    const std::vector<uint64_t> pages{0x98000000};
    switch_gpu_backing backing = backingOf(pages, 8);
    const char *why = nullptr;
    EXPECT_EQ(switch_nvjpg_mapping(switch_nvjpg_backing(&backing), 1, 4096, &why), nullptr);
    EXPECT_STREQ(why, "output window full");
    EXPECT_EQ(backing.context, nullptr);
    ASSERT_TRUE(space.release(hog, all));
    EXPECT_NE(mapped(backing, 1, 4096), nullptr);
    backing.destroy(backing.context);
}

// Backing retirement runs on the GPU driver's one worker, which every GPU, VIC and NVDEC backing free queues behind: it
// unmaps while a decode holds the job lock, without waiting for that decode.
TEST_F(NVJPGGate, RetirementUnmapsWhileADecodeHoldsTheJobLock)
{
    const std::vector<uint64_t> pages{0x97000000};
    switch_gpu_backing backing = backingOf(pages, 7);
    SwitchNVJPGMapping *mapping = mapped(backing, 1, 4096);
    ASSERT_NE(mapping, nullptr);
    const uint64_t address = mapping->address;
    gRetiring = &backing;
    gRetiredDuringRun = false;
    switch_test_lock_taken = retireDuringTheRun;
    TegraNVJPGJob job = decodeJob();
    nvjpg_dec_status status = {};
    tegra_nvjpg_lock();
    EXPECT_NO_THROW(tegra_nvjpg_run(&job, &status, nullptr));
    tegra_nvjpg_unlock();
    switch_test_lock_taken = nullptr;
    EXPECT_TRUE(gRetiredDuringRun);
    EXPECT_FALSE(gMapLock->locked);
    EXPECT_EQ(gDomain.translate(address), UINT64_MAX);
}

// A queue slot's scan buffer is mapped once, read-only with the sink page after it, and kept: later scans that fit write
// no page entry. A longer scan grows it, the old buffer unmapped before its pages go back.
TEST_F(NVJPGGate, AScanSlotMapsOnceAndGrowsOnlyWithItsOldBufferUnmappedFirst)
{
    SwitchNVJPGScan scan{};
    gSmmuRangeFlushes.clear();
    ASSERT_EQ(switch_nvjpg_scan_fit(&scan, 10000), 0);
    ASSERT_NE(scan.address, 0u);
    EXPECT_EQ(scan.buffer.size, TegraSMMUSpace::kGranule);
    EXPECT_EQ(gSmmuRangeFlushes.size(), 1u);
    EXPECT_EQ(gDomain.translate(scan.address), scan.buffer.pages[0]);
    EXPECT_EQ(gDomain.translate(scan.address + scan.buffer.size), gSink.pa);
    EXPECT_EQ(gDomain.translate(scan.address + scan.buffer.size + 4096), UINT64_MAX);
    EXPECT_EQ(*gDomain.pte(scan.address, false) & TegraSMMUDomain::kWrite, 0u);
    for (uint64_t bytes : {1ULL, 10000ULL, 65536ULL})
        EXPECT_EQ(switch_nvjpg_scan_fit(&scan, bytes), 0);
    EXPECT_EQ(gSmmuRangeFlushes.size(), 1u) << "a scan that fits maps nothing";

    gWatchedAddress = scan.address;
    gFreedWhileMapped = 0;
    ASSERT_EQ(switch_nvjpg_scan_fit(&scan, 3 * TegraSMMUSpace::kGranule + 1), 0);
    gWatchedAddress = 0;
    EXPECT_EQ(gFreedWhileMapped, 0) << "the old buffer's pages went back while NVJPG could still read them";
    EXPECT_EQ(scan.buffer.size, 4 * TegraSMMUSpace::kGranule);
    EXPECT_EQ(gDomain.translate(scan.address + 3 * TegraSMMUSpace::kGranule), scan.buffer.pages[48]);
    EXPECT_EQ(gSmmuRangeFlushes.size(), 3u) << "the old mapping's unmap, then the new one's map";
    tegra_nvjpg_pages_unmap(TEGRA_NVJPG_WINDOW_IN, scan.address, scan.buffer.count + 1);
    switch_dma_pages_free(&scan.buffer);
}

// A slot that cannot grow says why and is left empty, so its next use allocates afresh.
TEST_F(NVJPGGate, AScanSlotThatCannotGrowIsLeftEmpty)
{
    SwitchNVJPGScan scan{};
    ASSERT_EQ(switch_nvjpg_scan_fit(&scan, 4096), 0);
    TegraSMMUSpace &space = gSpaces[TEGRA_NVJPG_WINDOW_IN];
    const uint64_t all = space.bytesFree();
    const uint64_t hog = space.alloc(all);
    ASSERT_NE(hog, 0u);
    EXPECT_EQ(switch_nvjpg_scan_fit(&scan, 2 * TegraSMMUSpace::kGranule), -28);
    EXPECT_EQ(scan.address, 0u);
    EXPECT_EQ(scan.buffer.size, 0u);
    ASSERT_TRUE(space.release(hog, all));
    gScanAllocFails = true;
    EXPECT_EQ(switch_nvjpg_scan_fit(&scan, 4096), -12);
    gScanAllocFails = false;
    EXPECT_EQ(scan.address, 0u);
    ASSERT_EQ(switch_nvjpg_scan_fit(&scan, 4096), 0);
    EXPECT_NE(scan.address, 0u);
    tegra_nvjpg_pages_unmap(TEGRA_NVJPG_WINDOW_IN, scan.address, scan.buffer.count + 1);
    switch_dma_pages_free(&scan.buffer);
    EXPECT_EQ(space.bytesFree(), space.bytesTotal());
}

// Growth concentrates in as few slots as it can: a scan takes the shortest free buffer that holds it, else the longest.
TEST(NVJPGScanPick, TheShortestFreeBufferThatHoldsTheScanElseTheLongest)
{
    SwitchNVJPGScan scans[4] = {};
    const uint64_t sizes[4] = {0, 64 << 10, 256 << 10, 128 << 10};
    for (uint32_t i = 0; i < 4; ++i)
        scans[i].buffer.size = sizes[i];
    EXPECT_EQ(switch_nvjpg_scan_pick(scans, 0xf, 100 << 10), 3u);
    EXPECT_EQ(switch_nvjpg_scan_pick(scans, 0xf, 64 << 10), 1u);
    EXPECT_EQ(switch_nvjpg_scan_pick(scans, 0xf, 300 << 10), 2u);
    EXPECT_EQ(switch_nvjpg_scan_pick(scans, 0x3, 100 << 10), 1u);
    EXPECT_EQ(switch_nvjpg_scan_pick(scans, 0x1, 100 << 10), 0u);
    EXPECT_EQ(switch_nvjpg_scan_pick(scans, 0x5, 1), 2u);
}

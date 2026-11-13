// SPDX-License-Identifier: BSD-2-Clause-Patent
// switch_nvdec_output_recycle with a connection's park (SwitchNVDECPark.h): a session whose exported pictures hold their
// buffers until the client's pool surface takes the next picture allocates and maps a fixed number of buffers, however
// many frames it decodes; held buffers are never handed back; a full park, a size change and a closing client give
// buffers back.
#include <gtest/gtest.h>
#include <deque>
#include <memory>
#include <set>
#include <vector>
#include "switch_nvdec_output_stubs.h"
#include "../SwitchNVDECOutput.cpp"
#include "../SwitchNVDECPark.cpp"

namespace {
struct Allocation {
    IOBufferMemoryDescriptor memory;
    IOMemoryMap map;
    switch_gpu_backing backing;
};

std::vector<std::unique_ptr<Allocation>> gAllocations;
std::set<const switch_gpu_backing *> gHeld, gDecoding;
uint32_t gFrees;
task_t gTask = reinterpret_cast<task_t>(0x1234);
}  // namespace

extern "C" bool tegra_vic_present_ready()
{
    return true;
}

extern "C" bool switch_native_surface_gpu_ready()
{
    return true;
}

extern "C" bool switch_native_surface_display_ready()
{
    return true;
}

extern "C" int switch_gpu_backing_pending(switch_gpu_backing *backing, uint32_t, uint32_t *engines, uint32_t *cpuOwners)
{
    *cpuOwners = gHeld.count(backing) ? 1 : 0;
    *engines = gDecoding.count(backing) ? 1u << SWITCH_GPU_ENGINE_NVDEC : 0;
    return 0;
}

extern "C" int tegra_nvdec_buffer_alloc(uint64_t bytes, TegraNVDECBlock *block)
{
    gAllocations.push_back(std::make_unique<Allocation>());
    Allocation &a = *gAllocations.back();
    a.memory.mapping = &a.map;
    *block = {};
    block->size = bytes;
    block->iova = 0x60000000 + (gAllocations.size() << 24);
    block->pages.memory = &a.memory;
    block->backing = &a.backing;
    return 0;
}

extern "C" void tegra_nvdec_buffer_free(TegraNVDECBlock *block)
{
    ++gFrees;
    --block->backing->refs;
    *block = {};
}

extern "C" IOReturn tegra_vic_surface_lookup(uint32_t, task_t, TegraVICSurface *)
{
    return -22;
}

extern "C" void tegra_vic_surface_release(TegraVICSurface *)
{}

int switch_native_surface_publish(const TegraVICSurface *, const TegraVICPicture *, const char **, bool *)
{
    return -22;
}

namespace {
constexpr uint64_t kBytes = 3ULL << 20;

class SwitchNVDECParkTest : public testing::Test {
protected:
    SwitchNVDECPark park = {};

    void SetUp() override
    {
        gAllocations.clear();
        gHeld.clear();
        gDecoding.clear();
        gFrees = 0;
    }

    SwitchNVDECBuffer fresh(uint32_t handle, uint64_t bytes = kBytes)
    {
        SwitchNVDECBuffer b = {};
        b.handle = handle;
        EXPECT_EQ(tegra_nvdec_buffer_alloc(bytes, &b.block), 0);
        b.map = gAllocations.back()->memory.createMappingInTask(gTask, 0, kIOMapAnywhere | kIOMapDefaultCache);
        return b;
    }
};

// Three decoder surfaces, every picture exported into a pool of five client surfaces, each of which lets its earlier
// picture go when it takes a new one (switch_native_surface_publish's invalidate): every surface the decoder recycles
// is still held. 600 frames.
TEST_F(SwitchNVDECParkTest, SteadyDecodeAllocatesAFixedSet)
{
    constexpr unsigned kSurfaces = 3, kPool = 5, kFrames = 600;
    std::vector<SwitchNVDECBuffer> surfaces;
    for (unsigned i = 0; i < kSurfaces; ++i)
        surfaces.push_back(fresh(i + 1));
    const size_t startup = gAllocations.size();
    std::deque<const switch_gpu_backing *> exported;
    std::set<const switch_gpu_backing *> distinct;
    for (unsigned frame = 0; frame < kFrames; ++frame) {
        SwitchNVDECBuffer &s = surfaces[frame % kSurfaces];
        ASSERT_EQ(switch_nvdec_output_recycle(&s, gTask, &park), 0);
        ASSERT_EQ(gHeld.count(s.block.backing), 0u) << "frame " << frame << " decodes into a held buffer";
        distinct.insert(s.block.backing);
        gHeld.insert(s.block.backing);
        exported.push_back(s.block.backing);
        if (exported.size() > kPool) {
            gHeld.erase(exported.front());
            exported.pop_front();
        }
    }
    const size_t replacements = gAllocations.size() - startup;
    EXPECT_LE(replacements, kPool + 1u);
    EXPECT_EQ(park.allocated, replacements);
    EXPECT_EQ(park.taken + park.allocated, park.recycled);
    EXPECT_GT(park.taken, kFrames - kSurfaces - kPool - 2u);
    EXPECT_LE(distinct.size(), kSurfaces + kPool + 1u);
    EXPECT_EQ(gFrees, 0u);
}

TEST_F(SwitchNVDECParkTest, AHeldParkedBufferIsNeverTaken)
{
    SwitchNVDECBuffer s = fresh(1);
    gHeld.insert(s.block.backing);
    const auto *first = s.block.backing;
    ASSERT_EQ(switch_nvdec_output_recycle(&s, gTask, &park), 0);
    EXPECT_EQ(park.count, 1u);
    EXPECT_EQ(park.allocated, 1u);
    gHeld.insert(s.block.backing);
    ASSERT_EQ(switch_nvdec_output_recycle(&s, gTask, &park), 0);
    EXPECT_EQ(park.allocated, 2u);
    EXPECT_NE(s.block.backing, first);
    gHeld.erase(first);
    const auto *third = s.block.backing;
    gHeld.insert(third);
    ASSERT_EQ(switch_nvdec_output_recycle(&s, gTask, &park), 0);
    EXPECT_EQ(s.block.backing, first);
    EXPECT_EQ(park.taken, 1u);
    EXPECT_EQ(park.parked[0].block.backing, third);
}

TEST_F(SwitchNVDECParkTest, AFreeBufferOfAnotherSizeIsGivenBack)
{
    SwitchNVDECBuffer small = fresh(1, kBytes / 2);
    gHeld.insert(small.block.backing);
    ASSERT_EQ(switch_nvdec_output_recycle(&small, gTask, &park), 0);
    gHeld.clear();
    IOMemoryMap *parkedMap = park.parked[0].map;
    SwitchNVDECBuffer big = fresh(2);
    gHeld.insert(big.block.backing);
    ASSERT_EQ(switch_nvdec_output_recycle(&big, gTask, &park), 0);
    EXPECT_EQ(parkedMap->unmaps, 1u);
    EXPECT_EQ(gFrees, 1u);
    EXPECT_EQ(park.count, 1u);
    EXPECT_EQ(park.parked[0].block.size, kBytes);
}

TEST_F(SwitchNVDECParkTest, AFullParkGivesTheReplacedBufferBackAsBefore)
{
    SwitchNVDECBuffer s = fresh(1);
    std::vector<IOMemoryMap *> maps;
    for (uint32_t i = 0; i <= SwitchNVDECPark::kCapacity; ++i) {
        gHeld.insert(s.block.backing);
        maps.push_back(s.map);
        ASSERT_EQ(switch_nvdec_output_recycle(&s, gTask, &park), 0);
    }
    EXPECT_EQ(park.count, SwitchNVDECPark::kCapacity);
    EXPECT_EQ(maps[0]->unmaps, 0u);
    EXPECT_EQ(maps.back()->refs, 0u);
    EXPECT_EQ(gFrees, 1u);
    EXPECT_EQ(park.allocated, SwitchNVDECPark::kCapacity + 1u);
    EXPECT_EQ(park.most, SwitchNVDECPark::kCapacity);
}

// A queued job may still read a buffer it holds no reference to: one with engine work recorded stays parked.
TEST_F(SwitchNVDECParkTest, ABufferAnEngineStillUsesIsNotGivenBack)
{
    SwitchNVDECBuffer small = fresh(1, kBytes / 2);
    gHeld.insert(small.block.backing);
    ASSERT_EQ(switch_nvdec_output_recycle(&small, gTask, &park), 0);
    const auto *parked = park.parked[0].block.backing;
    gHeld.clear();
    gDecoding.insert(parked);
    SwitchNVDECBuffer big = fresh(2);
    gHeld.insert(big.block.backing);
    ASSERT_EQ(switch_nvdec_output_recycle(&big, gTask, &park), 0);
    EXPECT_EQ(gFrees, 0u);
    EXPECT_EQ(park.count, 2u);
    EXPECT_EQ(park.parked[0].block.backing, parked);
}

// Ordered engine work (a job still reading it as a reference) does not stop a same-size buffer's reuse: the next job
// waits for it in its own stream.
TEST_F(SwitchNVDECParkTest, OrderedEngineWorkStillAllowsReuse)
{
    SwitchNVDECBuffer s = fresh(1);
    const auto *first = s.block.backing;
    gHeld.insert(first);
    ASSERT_EQ(switch_nvdec_output_recycle(&s, gTask, &park), 0);
    gHeld.erase(first);
    gDecoding.insert(first);
    gHeld.insert(s.block.backing);
    ASSERT_EQ(switch_nvdec_output_recycle(&s, gTask, &park), 0);
    EXPECT_EQ(s.block.backing, first);
    EXPECT_EQ(park.taken, 1u);
}

TEST_F(SwitchNVDECParkTest, DropUnmapsEveryParkedBufferAndFreesOnlyWhenIdle)
{
    SwitchNVDECBuffer s = fresh(1);
    for (int i = 0; i < 3; ++i) {
        gHeld.insert(s.block.backing);
        ASSERT_EQ(switch_nvdec_output_recycle(&s, gTask, &park), 0);
    }
    std::vector<IOMemoryMap *> maps;
    for (uint32_t i = 0; i < park.count; ++i)
        maps.push_back(park.parked[i].map);
    switch_nvdec_park_drop(&park, false);
    EXPECT_EQ(park.count, 0u);
    EXPECT_EQ(gFrees, 0u);
    for (IOMemoryMap *map : maps)
        EXPECT_EQ(map->unmaps, 1u);

    SwitchNVDECPark idle = {};
    gHeld.insert(s.block.backing);
    ASSERT_EQ(switch_nvdec_output_recycle(&s, gTask, &idle), 0);
    switch_nvdec_park_drop(&idle, true);
    EXPECT_EQ(gFrees, 1u);
}

TEST_F(SwitchNVDECParkTest, WithoutAParkTheReplacedBufferGoesBackAsBefore)
{
    SwitchNVDECBuffer s = fresh(1);
    gHeld.insert(s.block.backing);
    IOMemoryMap *before = s.map;
    ASSERT_EQ(switch_nvdec_output_recycle(&s, gTask), 0);
    EXPECT_EQ(before->refs, 0u);
    EXPECT_EQ(gFrees, 1u);
}
}  // namespace

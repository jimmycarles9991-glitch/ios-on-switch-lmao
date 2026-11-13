// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <cstring>
#include "switch_nvdec_output_stubs.h"
#include "../SwitchNVDECOutput.cpp"
#include "../SwitchNVDECPark.cpp"

namespace {
IOBufferMemoryDescriptor oldMemory, nextMemory;
IOMemoryMap oldMap, nextMap;
switch_gpu_backing oldBacking, nextBacking;
uint64_t pages[512];
bool vicReady, gpuReady, displayReady;
int queryError, allocationError, lookupError, publishError;
uint32_t pendingEngines, cpuOwnerCount;
uint32_t allocations, frees, queries, lookups, surfaceReleases, publishes;
uint32_t requestedId;
task_t requestedTask;
TegraVICPicture exportedPicture;
IOMemoryDescriptor surfaceMemory;
}

extern "C" bool tegra_vic_present_ready()
{
    return vicReady;
}

extern "C" bool switch_native_surface_gpu_ready()
{
    return gpuReady;
}

extern "C" bool switch_native_surface_display_ready()
{
    return displayReady;
}

extern "C" int switch_gpu_backing_pending(switch_gpu_backing *backing, uint32_t access, uint32_t *engines,
                                          uint32_t *cpuOwners)
{
    EXPECT_EQ(backing, &oldBacking);
    EXPECT_EQ(access, uint32_t(SWITCH_GPU_BACKING_WRITE));
    ++queries;
    *engines = pendingEngines;
    *cpuOwners = cpuOwnerCount;
    return queryError;
}

extern "C" int tegra_nvdec_buffer_alloc(uint64_t bytes, TegraNVDECBlock *block)
{
    ++allocations;
    if (allocationError)
        return allocationError;
    *block = {};
    block->size = bytes;
    block->iova = 0x60000000;
    block->pages.memory = &nextMemory;
    block->backing = &nextBacking;
    return 0;
}

extern "C" void tegra_nvdec_buffer_free(TegraNVDECBlock *block)
{
    ++frees;
    --block->backing->refs;
    block->pages.memory->release();
    *block = {};
}

extern "C" IOReturn tegra_vic_surface_lookup(uint32_t id, task_t task, TegraVICSurface *surface)
{
    ++lookups;
    requestedId = id;
    requestedTask = task;
    *surface = {};
    surface->id = id;
    surface->memory = &surfaceMemory;
    surface->desc.length = 1695744;
    surface->baseOffset = 0;
    return lookupError;
}

extern "C" void tegra_vic_surface_release(TegraVICSurface *)
{
    ++surfaceReleases;
}

int switch_native_surface_publish(const TegraVICSurface *surface, const TegraVICPicture *picture, const char **why,
                                  bool *)
{
    ++publishes;
    EXPECT_EQ(surface->id, picture->surfaceID);
    exportedPicture = *picture;
    if (publishError && why)
        *why = "native export into a surface whose allocation is not its descriptor's length";
    return publishError;
}

namespace {
class SwitchNVDECOutputTest : public testing::Test {
protected:
    SwitchNVDECBuffer buffer;
    TegraNVDECPresent request;
    task_t task = reinterpret_cast<task_t>(0x1234);

    void SetUp() override
    {
        oldMemory = {};
        nextMemory = {};
        oldMap = {};
        nextMap = {};
        oldBacking = {};
        nextBacking = {};
        nextMemory.mapping = &nextMap;
        vicReady = gpuReady = displayReady = true;
        queryError = allocationError = lookupError = publishError = 0;
        pendingEngines = cpuOwnerCount = 0;
        allocations = frees = queries = lookups = surfaceReleases = publishes = 0;
        requestedId = 0;
        requestedTask = nullptr;
        exportedPicture = {};
        buffer = {};
        buffer.handle = 19;
        buffer.map = &oldMap;
        buffer.block.size = 2 * 1024 * 1024;
        buffer.block.iova = 0x50000000;
        buffer.block.backing = &oldBacking;
        buffer.block.pages.memory = &oldMemory;
        buffer.block.pages.pages = pages;
        buffer.block.pages.count = 512;
        request = {};
        request.pitch = 1536;
        request.rows = 736;
        request.chroma_offset = request.pitch * request.rows;
        request.block_height_log2 = 1;
        request.width = 1280;
        request.height = 720;
        request.sample_bytes = 1;
        request.surface_id = 52;
    }

    void unchanged() const
    {
        EXPECT_EQ(buffer.handle, 19u);
        EXPECT_EQ(buffer.map, &oldMap);
        EXPECT_EQ(buffer.block.backing, &oldBacking);
        EXPECT_EQ(buffer.block.pages.memory, &oldMemory);
        EXPECT_EQ(oldMap.refs, 1u);
        EXPECT_EQ(oldMemory.refs, 1u);
        EXPECT_EQ(oldBacking.refs, 1u);
    }
};

TEST_F(SwitchNVDECOutputTest, UnusedAllocationReusesMapAndHandle)
{
    EXPECT_EQ(switch_nvdec_output_recycle(&buffer, task), 0);
    unchanged();
    EXPECT_EQ(queries, 1u);
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(frees, 0u);
}

// Under asynchronous decode a surface comes round again while the job that wrote it, VIC's copy of it or a GPU read
// of it is still queued: the next job's dependencies order those, so the buffer stays.
TEST_F(SwitchNVDECOutputTest, EngineWorkTheNextJobOrdersKeepsTheAllocation)
{
    pendingEngines = 1u << SWITCH_GPU_ENGINE_NVDEC | 1u << SWITCH_GPU_ENGINE_VIC | 1u << SWITCH_GPU_ENGINE_GM20B;
    EXPECT_EQ(switch_nvdec_output_recycle(&buffer, task), 0);
    unchanged();
    EXPECT_EQ(allocations, 0u);
}

// NVENC still reading the picture in place (a transcode): NVDEC's next job waits for it on NVENC's syncpoint.
TEST_F(SwitchNVDECOutputTest, AnEncoderStillReadingKeepsTheAllocation)
{
    pendingEngines = 1u << SWITCH_GPU_ENGINE_NVENC;
    EXPECT_EQ(switch_nvdec_output_recycle(&buffer, task), 0);
    unchanged();
    EXPECT_EQ(allocations, 0u);
}

TEST_F(SwitchNVDECOutputTest, ExportedAllocationGetsIndependentReplacement)
{
    cpuOwnerCount = 1;
    oldBacking.refs = 2;
    EXPECT_EQ(switch_nvdec_output_recycle(&buffer, task), 0);
    EXPECT_EQ(buffer.handle, 19u);
    EXPECT_EQ(buffer.map, &nextMap);
    EXPECT_EQ(buffer.block.backing, &nextBacking);
    EXPECT_EQ(buffer.block.iova, 0x60000000u);
    EXPECT_EQ(nextMemory.mappedTask, task);
    EXPECT_EQ(nextMemory.mappedOptions, kIOMapAnywhere | kIOMapDefaultCache);
    EXPECT_EQ(oldBacking.refs, 1u);
    EXPECT_EQ(oldMap.refs, 0u);
    EXPECT_EQ(frees, 1u);
}

TEST_F(SwitchNVDECOutputTest, AScanoutStillReadingGetsAReplacement)
{
    pendingEngines = 1u << SWITCH_GPU_ENGINE_NVDEC | 1u << SWITCH_GPU_ENGINE_DISPLAY;
    EXPECT_EQ(switch_nvdec_output_recycle(&buffer, task), 0);
    EXPECT_EQ(buffer.block.backing, &nextBacking);
    EXPECT_EQ(allocations, 1u);
}

TEST_F(SwitchNVDECOutputTest, FailedAllocationPreservesCurrentOutput)
{
    cpuOwnerCount = 1;
    allocationError = -12;
    EXPECT_EQ(switch_nvdec_output_recycle(&buffer, task), -12);
    unchanged();
    EXPECT_EQ(frees, 0u);
}

TEST_F(SwitchNVDECOutputTest, FailedMappingDropsOnlyReplacement)
{
    cpuOwnerCount = 1;
    nextMemory.mapping = nullptr;
    EXPECT_EQ(switch_nvdec_output_recycle(&buffer, task), -12);
    unchanged();
    EXPECT_EQ(frees, 1u);
    EXPECT_EQ(nextBacking.refs, 0u);
    EXPECT_EQ(nextMemory.refs, 0u);
}

TEST_F(SwitchNVDECOutputTest, FailedQueryDoesNotAllocateOrRevive)
{
    queryError = -22;
    EXPECT_EQ(switch_nvdec_output_recycle(&buffer, task), -22);
    unchanged();
    EXPECT_EQ(allocations, 0u);
}

TEST_F(SwitchNVDECOutputTest, ExportRequiresEveryConsumerReady)
{
    gpuReady = false;
    EXPECT_EQ(switch_nvdec_output_export(&buffer.block, &request, task), -95);
    gpuReady = true;
    displayReady = false;
    EXPECT_FALSE(switch_nvdec_native_output_ready());
    displayReady = true;
    vicReady = false;
    EXPECT_EQ(switch_nvdec_output_export(&buffer.block, &request, task), -95);
    EXPECT_EQ(lookups, 0u);
    EXPECT_EQ(publishes, 0u);
}

TEST_F(SwitchNVDECOutputTest, FullNativeExportPublishesWithoutPixelCopy)
{
    EXPECT_EQ(switch_nvdec_output_export(&buffer.block, &request, task), 0);
    EXPECT_EQ(requestedId, request.surface_id);
    EXPECT_EQ(requestedTask, task);
    EXPECT_EQ(exportedPicture.backing, &oldBacking);
    EXPECT_EQ(exportedPicture.pages, pages);
    EXPECT_EQ(exportedPicture.pitch, 1536u);
    EXPECT_EQ(exportedPicture.rows, 736u);
    EXPECT_EQ(exportedPicture.bytes, 1695744u);
    EXPECT_EQ(publishes, 1u);
    EXPECT_EQ(surfaceReleases, 1u);
}

TEST_F(SwitchNVDECOutputTest, FailedPublishReleasesLookupAndPreservesBuffer)
{
    publishError = -16;
    EXPECT_EQ(switch_nvdec_output_export(&buffer.block, &request, task), -16);
    EXPECT_EQ(surfaceReleases, 1u);
    unchanged();
}

TEST_F(SwitchNVDECOutputTest, AFailedExportNamesItsRefusalAndTheSurfacesSizes)
{
    SwitchNVDECExportRefusal refusal = {};
    vicReady = false;
    EXPECT_EQ(switch_nvdec_output_export(&buffer.block, &request, task, &refusal), -95);
    EXPECT_STREQ(refusal.why, "native output while VIC, the GPU or the display cannot read it");
    vicReady = true;
    request.dst_x = 2;
    EXPECT_EQ(switch_nvdec_output_export(&buffer.block, &request, task, &refusal), -95);
    EXPECT_STREQ(refusal.why, "native output of a placed picture");
    request.dst_x = 0;
    publishError = -22;
    surfaceMemory.length = 1703936;
    EXPECT_EQ(switch_nvdec_output_export(&buffer.block, &request, task, &refusal), -22);
    EXPECT_STREQ(refusal.why, "native export into a surface whose allocation is not its descriptor's length");
    EXPECT_EQ(refusal.allocationBytes, 1695744u);
    EXPECT_EQ(refusal.descriptorBytes, 1703936u);
    EXPECT_EQ(refusal.baseOffset, 0u);
}

TEST_F(SwitchNVDECOutputTest, PlacedPicturesDoNotPublish)
{
    request.dst_x = 2;
    EXPECT_EQ(switch_nvdec_output_export(&buffer.block, &request, task), -95);
    EXPECT_EQ(lookups, 0u);
}

TEST_F(SwitchNVDECOutputTest, TenBitPicturesPublishTheirSixteenBitPlanes)
{
    request.sample_bytes = 2;
    request.width = 640;
    EXPECT_EQ(switch_nvdec_output_export(&buffer.block, &request, task), 0);
    EXPECT_EQ(exportedPicture.sampleBytes, 2u);
    EXPECT_EQ(publishes, 1u);
}

TEST_F(SwitchNVDECOutputTest, InvalidGeometryLeavesOutputUnchanged)
{
    TegraVICPicture picture = {};
    picture.width = 73;
    auto invalid = request;
    invalid.crop_x = UINT32_MAX;
    EXPECT_EQ(switch_nvdec_output_picture(&buffer.block, &invalid, &picture), -22);
    EXPECT_EQ(picture.width, 73u);
    invalid = request;
    invalid.chroma_offset = 256;
    EXPECT_EQ(switch_nvdec_output_picture(&buffer.block, &invalid, &picture), -22);
    invalid = request;
    invalid.rows = UINT32_MAX;
    EXPECT_EQ(switch_nvdec_output_picture(&buffer.block, &invalid, &picture), -22);
    EXPECT_EQ(picture.width, 73u);
}

TEST_F(SwitchNVDECOutputTest, AlphaPlaneTakesEightBitLumaAndIsNeverExported)
{
    TegraVICPicture picture = {};
    auto alpha = request;
    alpha.plane = 2;
    EXPECT_EQ(switch_nvdec_output_picture(&buffer.block, &alpha, &picture), 0);
    EXPECT_EQ(picture.plane, 2u);
    for (const uint32_t plane : {1u, 3u}) {
        alpha.plane = plane;
        EXPECT_EQ(switch_nvdec_output_picture(&buffer.block, &alpha, &picture), -22) << plane;
    }
    alpha.plane = 2;
    alpha.sample_bytes = 2;
    EXPECT_EQ(switch_nvdec_output_picture(&buffer.block, &alpha, &picture), -22);
    alpha.sample_bytes = 1;
    EXPECT_EQ(switch_nvdec_output_export(&buffer.block, &alpha, task), -95);
    EXPECT_EQ(publishes, 0u);
}
}  // namespace

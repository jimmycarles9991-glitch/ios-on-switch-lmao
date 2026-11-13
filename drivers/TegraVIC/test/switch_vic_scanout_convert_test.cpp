// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <cstdlib>
#include <vector>
#include "switch_vic_backing_stubs.h"
#undef IONewZero
#undef IODelete
#define IONewZero(type, count)         static_cast<type *>(std::calloc(count, sizeof(type)))
#define IODelete(pointer, type, count) std::free(pointer)
#include "../SwitchVICScanoutConvert.cpp"

struct SwitchVICMapping {
    switch_gpu_backing *backing;
    uint64_t address;
};

struct switch_gpu_cpu_lease {};

namespace {
switch_gpu_backing sourceBacking, logicalBacking, targetBacking;
switch_gpu_cpu_lease sourceLease, targetLease;
volatile uint64_t sourceGeneration = 1, representationGeneration = 9;
unsigned mappings, submissions, exports, mapCalls;
uint32_t targetPages;
int admissionError, exportError;
bool ready, missingTraits, inlineCompletion, failMapping;
void (*callback)(void *, int);
void *callbackContext;
std::vector<switch_gpu_backing_use> uses;
std::vector<switch_gpu_backing_guard> guards;
constexpr uint64_t kFence = (uint64_t(7) << 32) + 53;
}

extern "C" bool tegra_vic_ready()
{
    return ready;
}

extern "C" const TegraVICTraits *tegra_vic_traits()
{
    return missingTraits ? nullptr : &kTegraVICTraits;
}

extern "C" uint64_t tegra_vic_config_address()
{
    return 0x80001000;
}

extern "C" uint64_t tegra_vic_filter_address()
{
    return 0x80002000;
}

extern "C" void switch_gpu_backing_retain(switch_gpu_backing *backing)
{
    ++backing->refs;
}

extern "C" void switch_gpu_backing_describe(switch_gpu_backing *backing, switch_gpu_backing_info *info)
{
    EXPECT_EQ(backing, &targetBacking);
    *info = {};
    info->page_count = targetPages;
    info->allocation_id = 35;
    info->allocation_generation = 4;
}

extern "C" int switch_gpu_backing_export_read(switch_gpu_backing *backing, switch_gpu_backing_token *token,
                                              switch_gpu_cpu_lease **lease)
{
    EXPECT_EQ(backing, &targetBacking);
    EXPECT_EQ(submissions, 1u);
    ++exports;
    if (exportError)
        return exportError;
    *token = {kFence, 2, SWITCH_GPU_ENGINE_VIC, 11};
    *lease = &targetLease;
    return 0;
}

SwitchVICMapping *switch_vic_mapping_acquire(switch_gpu_backing *backing, bool writable)
{
    EXPECT_EQ(writable, backing == &targetBacking);
    ++mapCalls;
    if (failMapping && writable)
        return nullptr;
    ++mappings;
    ++backing->refs;
    return new SwitchVICMapping{backing, writable ? 0x40000000u : 0x30000000u};
}

void switch_vic_mapping_release(SwitchVICMapping *mapping)
{
    if (!mapping)
        return;
    --mappings;
    --mapping->backing->refs;
    delete mapping;
}

uint64_t switch_vic_mapping_address(const SwitchVICMapping *mapping)
{
    return mapping->address;
}

int switch_vic_submit_checked(const TegraVICJob *job, uint32_t count, const TegraVICAcquire *acquire,
                              const SwitchVICOwnership &ownership, void (*done)(void *, int), void *context,
                              uint32_t *legacyFence, SwitchVICEvents *)
{
    EXPECT_EQ(count, 1u);
    EXPECT_GT(job->count, 0u);
    EXPECT_EQ(acquire, nullptr);
    EXPECT_EQ(legacyFence, nullptr);
    EXPECT_EQ(ownership.leaseCount, 0u);
    uses.assign(ownership.uses, ownership.uses + ownership.useCount);
    guards.assign(ownership.guards, ownership.guards + ownership.guardCount);
    ++submissions;
    if (!admissionError) {
        if (inlineCompletion)
            done(context, 0);
        else {
            callback = done;
            callbackContext = context;
        }
    }
    return admissionError;
}

namespace {
class SwitchVICScanoutConvert : public testing::Test {
protected:
    SwitchNativeScanout source = {}, output = {};
    TegraVICImage image = {};
    TegraVICOp operation = {};

    void SetUp() override
    {
        mappings = submissions = exports = mapCalls = 0;
        targetPages = 16;
        admissionError = exportError = 0;
        ready = true;
        missingTraits = inlineCompletion = failMapping = false;
        callback = nullptr;
        callbackContext = nullptr;
        uses.clear();
        guards.clear();
        source.lease = &sourceLease;
        source.view.native = &sourceBacking;
        source.view.logical = &logicalBacking;
        source.view.guard = {&logicalBacking, &sourceGeneration, 1};
        source.view.representation = {&logicalBacking, &representationGeneration, 9};
        source.view.producer = {18, 3, SWITCH_GPU_ENGINE_NVDEC, 7};
        source.allocation.page_count = 16;
        image.format = tegra_vic_format(TEGRA_VIC_FOURCC('4', '2', '0', 'v'));
        image.width = 128;
        image.height = 64;
        image.pitch[0] = image.pitch[1] = 256;
        image.rows[0] = 64;
        image.rows[1] = 32;
        image.base[1] = 256 * 64;
        image.span[0] = 256 * 64;
        image.span[1] = 256 * 32;
        image.standard = TEGRA_VIC_YCC_709;
        operation.srcW = 128u << 16;
        operation.srcH = 64u << 16;
        operation.dstW = 128;
        operation.dstH = 64;
    }

    int submit()
    {
        return switch_vic_scanout_convert(&source, &image, &targetBacking, 128, 64, 512, &operation, &output);
    }

    void finish()
    {
        if (callback) {
            const auto done = callback;
            callback = nullptr;
            done(callbackContext, 0);
        }
    }

    void TearDown() override
    {
        finish();
        if (output.view.native)
            --output.view.native->refs;
        EXPECT_EQ(mappings, 0u);
        EXPECT_EQ(sourceBacking.refs, 1u);
        EXPECT_EQ(logicalBacking.refs, 1u);
        EXPECT_EQ(targetBacking.refs, 1u);
    }
};

TEST_F(SwitchVICScanoutConvert, PublishesExactNativeDependenciesAndKeepsMapsUntilHardwareCompletion)
{
    ASSERT_EQ(submit(), 0);
    ASSERT_EQ(uses.size(), 3u);
    EXPECT_EQ(uses[0].backing, &sourceBacking);
    EXPECT_EQ(uses[0].access, uint32_t(SWITCH_GPU_BACKING_READ));
    EXPECT_EQ(uses[1].backing, &targetBacking);
    EXPECT_EQ(uses[1].access, uint32_t(SWITCH_GPU_BACKING_WRITE));
    EXPECT_EQ(uses[2].backing, &logicalBacking);
    EXPECT_EQ(uses[2].access, uint32_t(SWITCH_GPU_BACKING_READ));
    ASSERT_EQ(guards.size(), 2u);
    EXPECT_EQ(guards[0].generation, &sourceGeneration);
    EXPECT_EQ(guards[1].generation, &representationGeneration);
    EXPECT_EQ(mappings, 2u);
    EXPECT_EQ(output.view.producer.value, kFence);
    EXPECT_EQ(output.view.producer.engine, uint32_t(SWITCH_GPU_ENGINE_VIC));
    EXPECT_EQ(output.lease, &targetLease);
    EXPECT_EQ(output.view.layout.planes[0].pitch, 512u);
    EXPECT_EQ(output.view.layout.planes[0].bytes, 32768u);
    EXPECT_EQ(output.allocation.allocation_generation, 4u);
    finish();
    EXPECT_EQ(mappings, 0u);
    EXPECT_EQ(targetBacking.refs, 2u);
}

TEST_F(SwitchVICScanoutConvert, InlineCompletionCannotDestroyCallerStateBeforeExport)
{
    inlineCompletion = true;
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(mappings, 0u);
    EXPECT_EQ(exports, 1u);
    EXPECT_EQ(output.view.native, &targetBacking);
}

TEST_F(SwitchVICScanoutConvert, RejectedSubmissionReleasesMapsAndNeverExports)
{
    admissionError = -16;
    EXPECT_EQ(submit(), -16);
    EXPECT_EQ(mappings, 0u);
    EXPECT_EQ(exports, 0u);
    EXPECT_EQ(output.lease, nullptr);
}

TEST_F(SwitchVICScanoutConvert, FailedSnapshotDoesNotReleaseMappingsOfAlreadyAcceptedConversion)
{
    exportError = -5;
    EXPECT_EQ(submit(), -5);
    EXPECT_EQ(mappings, 2u);
    EXPECT_EQ(output.view.native, nullptr);
    EXPECT_EQ(output.lease, nullptr);
    finish();
    EXPECT_EQ(mappings, 0u);
}

TEST_F(SwitchVICScanoutConvert, RejectsBoundsAndAliasingBeforeMapping)
{
    image.base[1] = (uint64_t(source.allocation.page_count) << 12) - 256;
    EXPECT_EQ(submit(), -22);
    image.base[1] = 256 * 64;
    targetPages = 1;
    EXPECT_EQ(submit(), -22);
    targetPages = 16;
    source.view.native = &targetBacking;
    EXPECT_EQ(submit(), -22);
    source.view.native = &sourceBacking;
    source.view.logical = &targetBacking;
    EXPECT_EQ(submit(), -22);
    EXPECT_EQ(mapCalls, 0u);
    EXPECT_EQ(submissions, 0u);
}

TEST_F(SwitchVICScanoutConvert, RequiresImmutableInputAndReadyTraits)
{
    source.lease = nullptr;
    EXPECT_EQ(submit(), -22);
    source.lease = &sourceLease;
    ready = false;
    EXPECT_EQ(submit(), -19);
    ready = true;
    missingTraits = true;
    EXPECT_EQ(submit(), -19);
    EXPECT_EQ(mapCalls, 0u);
}

TEST_F(SwitchVICScanoutConvert, PartialMappingFailureUnwindsFirstMap)
{
    failMapping = true;
    EXPECT_EQ(submit(), -12);
    EXPECT_EQ(mappings, 0u);
    EXPECT_EQ(submissions, 0u);
}

TEST_F(SwitchVICScanoutConvert, UnsupportedTransformCannotPublishDestination)
{
    operation.transform = 8;
    EXPECT_EQ(submit(), -95);
    EXPECT_EQ(mappings, 0u);
    EXPECT_EQ(submissions, 0u);
    EXPECT_EQ(exports, 0u);
}
}  // namespace

class SwitchVICScanoutPlace : public SwitchVICScanoutConvert {
protected:
    switch_gpu_backing windowB = {};
    TegraVICImage layout = {};

    void SetUp() override
    {
        SwitchVICScanoutConvert::SetUp();
        layout.format = tegra_vic_format(TEGRA_VIC_FOURCC('B', 'G', 'R', 'A'));
        layout.width = 128;
        layout.height = 128;
        layout.pitch[0] = 512;
        layout.rows[0] = 128;
        layout.span[0] = 512 * 128;
        layout.blockLinear = 1;
        layout.blockHeightLog2 = 4;
    }

    int place(const SwitchVICPlaceReads &also = {})
    {
        return switch_vic_scanout_place(&source, &image, &targetBacking, &layout, &operation, also, &output);
    }
};

TEST_F(SwitchVICScanoutPlace, ABlockLinearDestinationIsWrittenAsTheCallerLaysItOut)
{
    ASSERT_EQ(place(), 0);
    const auto &plane = output.view.layout.planes[0];
    EXPECT_EQ(plane.layout, uint32_t(SWITCH_GPU_BACKING_BLOCK_LINEAR));
    EXPECT_EQ(plane.block_height_log2, 4u);
    EXPECT_EQ(plane.pitch, 512u);
    EXPECT_EQ(plane.bytes, 65536u);
    EXPECT_EQ(output.view.producer.engine, uint32_t(SWITCH_GPU_ENGINE_VIC));
    EXPECT_EQ(output.view.native, &targetBacking);
    ASSERT_EQ(uses.size(), 3u);
}

TEST_F(SwitchVICScanoutPlace, BackingsTheFlipAlsoWaitsForAreUsesTheJobOrdersAfterNotMappings)
{
    ASSERT_EQ(place({{&windowB, nullptr}, 1}), 0);
    ASSERT_EQ(uses.size(), 4u);
    EXPECT_EQ(uses[3].backing, &windowB);
    EXPECT_EQ(uses[3].access, uint32_t(SWITCH_GPU_BACKING_READ));
    EXPECT_EQ(mapCalls, 2u);
}

TEST_F(SwitchVICScanoutPlace, ALayoutPastTheDestinationOrOfPartialBlocksIsRefused)
{
    layout.span[0] = 512 * 128 + 4096;
    EXPECT_EQ(place(), -22);
    EXPECT_EQ(mapCalls, 0u);
    layout.span[0] = 512 * 128;
    layout.rows[0] = 120;
    EXPECT_EQ(place(), -95);
    EXPECT_EQ(submissions, 0u);
    EXPECT_EQ(mappings, 0u);
}

TEST_F(SwitchVICScanoutPlace, TheConversionKeepsItsPitchRule)
{
    EXPECT_EQ(switch_vic_scanout_convert(&source, &image, &targetBacking, 128, 64, 320, &operation, &output), -22);
    EXPECT_EQ(mapCalls, 0u);
}

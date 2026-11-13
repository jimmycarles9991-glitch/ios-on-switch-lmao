// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <cstdlib>
#include <vector>
#include "switch_vic_backing_stubs.h"
#undef IONewZero
#undef IODelete
#define IONewZero(type, count)         static_cast<type *>(std::calloc(count, sizeof(type)))
#define IODelete(pointer, type, count) std::free(pointer)
#include "../SwitchVICScanoutCompose.cpp"

struct SwitchVICMapping {
    switch_gpu_backing *backing;
    uint64_t address;
};

struct switch_gpu_cpu_lease {};

namespace {
switch_gpu_backing videoBacking, logicalBacking, cardBacking, targetBacking;
switch_gpu_cpu_lease sourceLease, targetLease;
volatile uint64_t sourceGeneration = 1, representationGeneration = 9;
unsigned mappings, submissions, exports;
uint32_t targetPages;
int admissionError;
bool multiSlot;
TegraVICTraits traits;
TegraVICJob lastJob;
void (*callback)(void *, int);
void *callbackContext;
std::vector<switch_gpu_backing_use> uses;
std::vector<switch_gpu_backing_guard> guards;
constexpr uint64_t kFence = (uint64_t(7) << 32) + 54;
}  // namespace

extern "C" bool tegra_vic_ready()
{
    return true;
}

extern "C" const TegraVICTraits *tegra_vic_traits()
{
    traits = kTegraVICTraits;
    traits.multiSlot = multiSlot;
    return &traits;
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
    *info = {};
    info->page_count = backing == &targetBacking ? targetPages : 4096;
    info->allocation_id = 36;
}

extern "C" int switch_gpu_backing_export_read(switch_gpu_backing *backing, switch_gpu_backing_token *token,
                                              switch_gpu_cpu_lease **lease)
{
    EXPECT_EQ(backing, &targetBacking);
    ++exports;
    *token = {kFence, 2, SWITCH_GPU_ENGINE_VIC, 11};
    *lease = &targetLease;
    return 0;
}

SwitchVICMapping *switch_vic_mapping_acquire(switch_gpu_backing *backing, bool writable)
{
    EXPECT_EQ(writable, backing == &targetBacking);
    ++mappings;
    ++backing->refs;
    const uint64_t address = writable ? 0x40000000u : backing == &videoBacking ? 0x30000000u : 0x38000000u;
    return new SwitchVICMapping{backing, address};
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
    EXPECT_EQ(acquire, nullptr);
    EXPECT_EQ(legacyFence, nullptr);
    lastJob = *job;
    uses.assign(ownership.uses, ownership.uses + ownership.useCount);
    guards.assign(ownership.guards, ownership.guards + ownership.guardCount);
    ++submissions;
    if (!admissionError) {
        callback = done;
        callbackContext = context;
    }
    return admissionError;
}

namespace {
class SwitchVICScanoutCompose : public testing::Test {
protected:
    SwitchNativeScanout video = {}, card = {}, output = {};
    SwitchVICComposeSource sources[2] = {};
    TegraVICImage layout = {};
    TegraVICRect target = {0, 0, 640, 720};

    void SetUp() override
    {
        mappings = submissions = exports = 0;
        targetPages = 1024;
        admissionError = 0;
        multiSlot = true;
        callback = nullptr;
        uses.clear();
        guards.clear();
        video.lease = &sourceLease;
        video.view.native = &videoBacking;
        video.view.logical = &logicalBacking;
        video.view.guard = {&logicalBacking, &sourceGeneration, 1};
        video.view.representation = {&logicalBacking, &representationGeneration, 9};
        video.allocation.page_count = 4096;
        card.lease = &sourceLease;
        card.view.native = &cardBacking;
        card.allocation.page_count = 4096;
        auto &v = sources[0];
        v.scanout = &video;
        v.image.format = tegra_vic_format(TEGRA_VIC_FOURCC('4', '2', '0', 'v'));
        v.image.width = 1280;
        v.image.height = 720;
        v.image.pitch[0] = v.image.pitch[1] = 1280;
        v.image.rows[0] = 720;
        v.image.rows[1] = 360;
        v.image.base[1] = 1280 * 720;
        v.image.span[0] = v.image.span[1] = 1280 * 720;
        v.slot.srcW = 1280u << 16;
        v.slot.srcH = 720u << 16;
        v.slot.dstW = 640;
        v.slot.dstH = 360;
        v.slot.alpha10 = 1023;
        auto &c = sources[1];
        c.scanout = &card;
        c.image.format = tegra_vic_format(TEGRA_VIC_FOURCC('B', 'G', 'R', 'A'));
        c.image.width = 256;
        c.image.height = 256;
        c.image.pitch[0] = 1024;
        c.image.rows[0] = 256;
        c.image.span[0] = 1024 * 256;
        c.slot.srcW = c.slot.srcH = 256u << 16;
        c.slot.dstY = 400;
        c.slot.dstW = c.slot.dstH = 256;
        c.slot.blend = TEGRA_VIC_BLEND_PREMULTIPLIED;
        c.slot.alpha10 = 1023;
        layout.format = tegra_vic_format(TEGRA_VIC_FOURCC('B', 'G', 'R', 'A'));
        layout.width = 640;
        layout.height = 720;
        layout.pitch[0] = 2560;
        layout.rows[0] = 768;
        layout.span[0] = 2560ull * 768;
        layout.blockLinear = 1;
        layout.blockHeightLog2 = 4;
    }

    int submit(uint32_t count = 2)
    {
        return switch_vic_scanout_compose(sources, count, &targetBacking, layout, target, &output);
    }

    void TearDown() override
    {
        if (callback) {
            const auto done = callback;
            callback = nullptr;
            done(callbackContext, 0);
        }
        if (output.view.native)
            --output.view.native->refs;
        EXPECT_EQ(mappings, 0u);
        EXPECT_EQ(videoBacking.refs, 1u);
        EXPECT_EQ(cardBacking.refs, 1u);
        EXPECT_EQ(targetBacking.refs, 1u);
    }
};

TEST_F(SwitchVICScanoutCompose, OneJobReadsEverySourceAndWritesTheDestination)
{
    ASSERT_EQ(submit(), 0);
    ASSERT_EQ(uses.size(), 4u);
    EXPECT_EQ(uses[0].backing, &videoBacking);
    EXPECT_EQ(uses[1].backing, &cardBacking);
    EXPECT_EQ(uses[2].backing, &targetBacking);
    EXPECT_EQ(uses[2].access, uint32_t(SWITCH_GPU_BACKING_WRITE));
    EXPECT_EQ(uses[3].backing, &logicalBacking);
    EXPECT_EQ(guards.size(), 2u);
    EXPECT_EQ(mappings, 3u);
    bool video = false, card = false;
    for (uint32_t i = 0; i < lastJob.count; ++i) {
        video |= lastJob.method[i] == 0x400 && lastJob.value[i] == 0x30000000u >> 8;
        card |= lastJob.method[i] == 0x460 && lastJob.value[i] == 0x38000000u >> 8;
    }
    EXPECT_TRUE(video);
    EXPECT_TRUE(card);
    EXPECT_EQ(output.view.producer.value, kFence);
    EXPECT_EQ(output.lease, &targetLease);
    EXPECT_EQ(output.view.layout.planes[0].layout, uint32_t(SWITCH_GPU_BACKING_BLOCK_LINEAR));
    EXPECT_EQ(output.view.layout.planes[0].block_height_log2, 4u);
    EXPECT_EQ(output.view.layout.planes[0].pitch, 2560u);
}

TEST_F(SwitchVICScanoutCompose, ASourceTwiceIsOneUse)
{
    sources[1] = sources[0];
    sources[1].slot.dstY = 360;
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(uses.size(), 3u);
    EXPECT_EQ(guards.size(), 2u);
}

TEST_F(SwitchVICScanoutCompose, WhatTheEngineCannotComposeIsRefusedBeforeSubmission)
{
    multiSlot = false;
    EXPECT_EQ(submit(), -95);
    EXPECT_EQ(submissions, 0u);
    EXPECT_EQ(mappings, 0u);
    EXPECT_EQ(submit(1), 0);
}

TEST_F(SwitchVICScanoutCompose, ADestinationSmallerThanItsLayoutIsRefused)
{
    targetPages = 100;
    EXPECT_EQ(submit(), -22);
    EXPECT_EQ(submissions, 0u);
}

TEST_F(SwitchVICScanoutCompose, ARefusedSubmissionReleasesItsMappingsAndNeverExports)
{
    admissionError = -16;
    EXPECT_EQ(submit(), -16);
    EXPECT_EQ(mappings, 0u);
    EXPECT_EQ(exports, 0u);
    EXPECT_EQ(output.lease, nullptr);
}

TEST_F(SwitchVICScanoutCompose, ASourceWithoutItsFrameIsRefused)
{
    card.lease = nullptr;
    EXPECT_EQ(submit(), -22);
    card.lease = &sourceLease;
    sources[1].image.span[0] = uint64_t(4097) << 12;
    EXPECT_EQ(submit(), -22);
    EXPECT_EQ(submissions, 0u);
}
}  // namespace

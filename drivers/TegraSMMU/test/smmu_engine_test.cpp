// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "smmu_engine_stubs.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "drivers/TegraSMMU/TegraSMMU.h"
#include "drivers/TegraSMMU/TegraSMMUEngine.h"

namespace {
constexpr uintptr_t kMc = 0x70019000;
constexpr uint32_t kPtbAsid = 0x1c;
constexpr uint32_t kPtbData = 0x20;
constexpr uint64_t kWindowBase = 0x10000000ULL;
constexpr uint64_t kWindowBytes = 4ULL << 20;

std::map<uint32_t, uint32_t> gRegisters;
std::map<uint32_t, uint32_t> gPtbByAsid;
std::vector<uint32_t> gDetached;
unsigned gDmaLive;
uint64_t gNextDmaPa = 0x90000000ULL;

class SmmuEngineTest : public testing::Test {
protected:
    void SetUp() override
    {
        gRegisters.clear();
        gPtbByAsid.clear();
        gDetached.clear();
        gDmaLive = 0;
    }

    void TearDown() override
    {
        EXPECT_EQ(gDmaLive, 0u);
    }
};

TEST_F(SmmuEngineTest, PartialAttachRollsBackFirstGpuGroupAndClearsAttachments)
{
    constexpr TegraSMMUSwgroup kGpuGroups[] = {
        {TEGRA_SMMU_GPU_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_2, (1u << 24) | (1u << 25)},
        {TEGRA_SMMU_GPU_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_4, (1u << 8) | (1u << 9)},
    };
    gRegisters[TEGRA_SMMU_TRANSLATION_ENABLE_2] = (1u << 24) | (1u << 25);

    TegraSMMUEngine engine;
    ASSERT_TRUE(engine.init(TEGRA_SMMU_ASID_GPU, kWindowBase, kWindowBytes, 0));
    ASSERT_EQ(gDmaLive, 1u);

    char line[160] = {};
    EXPECT_EQ(engine.attach(kGpuGroups, 2, line, sizeof(line)), -1);
    EXPECT_NE(line[0], '\0');

    EXPECT_EQ(gRegisters[TEGRA_SMMU_GPU_ASID], 0u);
    EXPECT_EQ(gPtbByAsid[TEGRA_SMMU_ASID_GPU], 0u);
    ASSERT_EQ(gDetached.size(), 1u);
    EXPECT_EQ(gDetached[0], static_cast<uint32_t>(TEGRA_SMMU_GPU_ASID));

    engine.release();
    EXPECT_EQ(gDmaLive, 0u);

    gRegisters.clear();
    EXPECT_EQ(tegra_smmu_resume(), 0);
    EXPECT_EQ(gRegisters[TEGRA_SMMU_GPU_ASID], 0u);
    EXPECT_EQ(gPtbByAsid[TEGRA_SMMU_ASID_GPU], 0u);
}

TEST_F(SmmuEngineTest, PartialAttachRollsBackMultipleDistinctSwgroups)
{
    constexpr TegraSMMUSwgroup kGroups[] = {
        {TEGRA_SMMU_NVDEC_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_3, (1u << 24) | (1u << 25)},
        {TEGRA_SMMU_NVJPG_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_3, (1u << 30) | (1u << 31)},
        {TEGRA_SMMU_GPU_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_4, (1u << 8) | (1u << 9)},
    };
    gRegisters[TEGRA_SMMU_TRANSLATION_ENABLE_3] = (1u << 24) | (1u << 25) | (1u << 30) | (1u << 31);

    TegraSMMUEngine engine;
    ASSERT_TRUE(engine.init(TEGRA_SMMU_ASID_GPU, kWindowBase, kWindowBytes, 0));

    char line[160] = {};
    EXPECT_EQ(engine.attach(kGroups, 3, line, sizeof(line)), -1);
    EXPECT_EQ(gRegisters[TEGRA_SMMU_NVDEC_ASID], 0u);
    EXPECT_EQ(gRegisters[TEGRA_SMMU_NVJPG_ASID], 0u);
    EXPECT_EQ(gRegisters[TEGRA_SMMU_GPU_ASID], 0u);
    EXPECT_EQ(gPtbByAsid[TEGRA_SMMU_ASID_GPU], 0u);
    EXPECT_EQ(gDetached.size(), 2u);

    engine.release();

    gRegisters.clear();
    EXPECT_EQ(tegra_smmu_resume(), 0);
    EXPECT_EQ(gRegisters[TEGRA_SMMU_NVDEC_ASID], 0u);
    EXPECT_EQ(gRegisters[TEGRA_SMMU_NVJPG_ASID], 0u);
}

TEST_F(SmmuEngineTest, FullGpuAttachLatchesBothTranslationRegistersAndSurvivesResume)
{
    constexpr TegraSMMUSwgroup kGpuGroups[] = {
        {TEGRA_SMMU_GPU_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_2, (1u << 24) | (1u << 25)},
        {TEGRA_SMMU_GPU_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_4, (1u << 8) | (1u << 9)},
    };
    gRegisters[TEGRA_SMMU_TRANSLATION_ENABLE_2] = (1u << 24) | (1u << 25);
    gRegisters[TEGRA_SMMU_TRANSLATION_ENABLE_4] = (1u << 8) | (1u << 9);

    TegraSMMUEngine engine;
    ASSERT_TRUE(engine.init(TEGRA_SMMU_ASID_GPU, kWindowBase, kWindowBytes, 0));
    const uint32_t expectedPtb = engine.domain.ptb();

    char line[160] = {};
    ASSERT_EQ(engine.attach(kGpuGroups, 2, line, sizeof(line)), 0) << line;
    EXPECT_EQ(gRegisters[TEGRA_SMMU_GPU_ASID], (1u << 31) | TEGRA_SMMU_ASID_GPU);
    EXPECT_EQ(gPtbByAsid[TEGRA_SMMU_ASID_GPU], expectedPtb);

    // SC7 resets non-secure SMMU registers; BL31 restores the secure translation enables before resume.
    gRegisters.erase(TEGRA_SMMU_GPU_ASID);
    gPtbByAsid.clear();
    EXPECT_EQ(tegra_smmu_resume(), 0);
    EXPECT_EQ(gRegisters[TEGRA_SMMU_GPU_ASID], (1u << 31) | TEGRA_SMMU_ASID_GPU);
    EXPECT_EQ(gPtbByAsid[TEGRA_SMMU_ASID_GPU], expectedPtb);

    engine.detach();
    engine.release();
}
}  // namespace

bool switch_dma_alloc(switch_dma_buffer *out, size_t size, uint64_t, IOOptionBits)
{
    void *mem = nullptr;
    if (posix_memalign(&mem, 4096, size) != 0)
        return false;
    memset(mem, 0, size);
    out->memory = nullptr;
    out->va = static_cast<uint8_t *>(mem);
    out->pa = gNextDmaPa;
    out->size = size;
    gNextDmaPa += (size + 4095) & ~size_t(4095);
    ++gDmaLive;
    return true;
}

void switch_dma_free(switch_dma_buffer *buffer)
{
    if (!buffer || !buffer->va)
        return;
    free(buffer->va);
    *buffer = {};
    --gDmaLive;
}

extern "C" {
uint32_t switch_mmio_model_read32(uintptr_t address)
{
    const uint32_t offset = static_cast<uint32_t>(address - kMc);
    if (offset == kPtbData)
        return gPtbByAsid[gRegisters[kPtbAsid] & 0x7fu];
    return gRegisters[offset];
}

void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
    const uint32_t offset = static_cast<uint32_t>(address - kMc);
    gRegisters[offset] = value;
    if (offset == kPtbData)
        gPtbByAsid[gRegisters[kPtbAsid] & 0x7fu] = value;
}

uint16_t switch_mmio_model_read16(uintptr_t address)
{
    return static_cast<uint16_t>(switch_mmio_model_read32(address));
}

void switch_mmio_model_write16(uintptr_t address, uint16_t value)
{
    switch_mmio_model_write32(address, value);
}

uint8_t switch_mmio_model_read8(uintptr_t address)
{
    return static_cast<uint8_t>(switch_mmio_model_read32(address));
}

void switch_mmio_model_write8(uintptr_t address, uint8_t value)
{
    switch_mmio_model_write32(address, value);
}

uintptr_t ml_io_map(uintptr_t phys_addr, uintptr_t)
{
    return phys_addr;
}

void switch_mc_errors_resume()
{}

void switch_mc_errors_detached(uint32_t asidReg)
{
    gDetached.push_back(asidReg);
}
}

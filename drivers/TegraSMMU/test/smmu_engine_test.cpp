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
std::vector<std::pair<uint32_t, uint32_t>> gFlushes;
unsigned gDmaLive;
bool gGpubHardwired = true;
uint64_t gNextDmaPa = 0x90000000ULL;

class SmmuEngineTest : public testing::Test {
protected:
    void SetUp() override
    {
        gRegisters.clear();
        gPtbByAsid.clear();
        gDetached.clear();
        gFlushes.clear();
        gDmaLive = 0;
    }

    void TearDown() override
    {
        EXPECT_EQ(gDmaLive, 0u);
    }
};

constexpr TegraSMMUSwgroup kNvencGroups[] = {
    {TEGRA_SMMU_NVENC_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_0, 1u << 28},
    {TEGRA_SMMU_NVENC_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_1, 1u << 11},
};

TEST_F(SmmuEngineTest, PartialAttachRollsBackTheFirstTranslationRegisterAndClearsAttachments)
{
    gRegisters[TEGRA_SMMU_TRANSLATION_ENABLE_0] = 1u << 28;

    TegraSMMUEngine engine;
    ASSERT_TRUE(engine.init(TEGRA_SMMU_ASID_NVENC, kWindowBase, kWindowBytes, 0));
    ASSERT_EQ(gDmaLive, 1u);

    char line[160] = {};
    EXPECT_EQ(engine.attach(kNvencGroups, 2, line, sizeof(line)), -1);
    EXPECT_NE(line[0], '\0');

    EXPECT_EQ(gRegisters[TEGRA_SMMU_NVENC_ASID], 0u);
    EXPECT_EQ(gPtbByAsid[TEGRA_SMMU_ASID_NVENC], 0u);
    ASSERT_EQ(gDetached.size(), 1u);
    EXPECT_EQ(gDetached[0], static_cast<uint32_t>(TEGRA_SMMU_NVENC_ASID));

    engine.release();
    EXPECT_EQ(gDmaLive, 0u);

    gRegisters.clear();
    EXPECT_EQ(tegra_smmu_resume(), 0);
    EXPECT_EQ(gRegisters[TEGRA_SMMU_NVENC_ASID], 0u);
    EXPECT_EQ(gPtbByAsid[TEGRA_SMMU_ASID_NVENC], 0u);
}

TEST_F(SmmuEngineTest, PartialAttachRollsBackMultipleDistinctSwgroups)
{
    constexpr TegraSMMUSwgroup kGroups[] = {
        {TEGRA_SMMU_NVDEC_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_3, (1u << 24) | (1u << 25)},
        {TEGRA_SMMU_NVJPG_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_3, (1u << 30) | (1u << 31)},
        {TEGRA_SMMU_VIC_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_2, 3u << 12},
    };
    gRegisters[TEGRA_SMMU_TRANSLATION_ENABLE_3] = (1u << 24) | (1u << 25) | (1u << 30) | (1u << 31);

    TegraSMMUEngine engine;
    ASSERT_TRUE(engine.init(TEGRA_SMMU_ASID_NVDEC, kWindowBase, kWindowBytes, 0));

    char line[160] = {};
    EXPECT_EQ(engine.attach(kGroups, 3, line, sizeof(line)), -1);
    EXPECT_EQ(gRegisters[TEGRA_SMMU_NVDEC_ASID], 0u);
    EXPECT_EQ(gRegisters[TEGRA_SMMU_NVJPG_ASID], 0u);
    EXPECT_EQ(gRegisters[TEGRA_SMMU_VIC_ASID], 0u);
    EXPECT_EQ(gPtbByAsid[TEGRA_SMMU_ASID_NVDEC], 0u);
    EXPECT_EQ(gDetached.size(), 2u);

    engine.release();

    gRegisters.clear();
    EXPECT_EQ(tegra_smmu_resume(), 0);
    EXPECT_EQ(gRegisters[TEGRA_SMMU_NVDEC_ASID], 0u);
    EXPECT_EQ(gRegisters[TEGRA_SMMU_NVJPG_ASID], 0u);
}

TEST_F(SmmuEngineTest, FullAttachLatchesBothTranslationRegistersAndSurvivesResume)
{
    gRegisters[TEGRA_SMMU_TRANSLATION_ENABLE_0] = 1u << 28;
    gRegisters[TEGRA_SMMU_TRANSLATION_ENABLE_1] = 1u << 11;

    TegraSMMUEngine engine;
    ASSERT_TRUE(engine.init(TEGRA_SMMU_ASID_NVENC, kWindowBase, kWindowBytes, 0));
    const uint32_t expectedPtb = engine.domain.ptb();

    char line[160] = {};
    ASSERT_EQ(engine.attach(kNvencGroups, 2, line, sizeof(line)), 0) << line;
    EXPECT_EQ(gRegisters[TEGRA_SMMU_NVENC_ASID], (1u << 31) | TEGRA_SMMU_ASID_NVENC);
    EXPECT_EQ(gPtbByAsid[TEGRA_SMMU_ASID_NVENC], expectedPtb);

    // SC7 resets non-secure SMMU registers; BL31 restores the secure translation enables before resume.
    gRegisters.erase(TEGRA_SMMU_NVENC_ASID);
    gPtbByAsid.clear();
    EXPECT_EQ(tegra_smmu_resume(), 0);
    EXPECT_EQ(gRegisters[TEGRA_SMMU_NVENC_ASID], (1u << 31) | TEGRA_SMMU_ASID_NVENC);
    EXPECT_EQ(gPtbByAsid[TEGRA_SMMU_ASID_NVENC], expectedPtb);

    engine.detach();
    engine.release();
}

// GPUB: GM20B's a[34] = 1 accesses, enable bit hardwired to 1 (T210 TRM 18.11.1.136), so it reads 0x80000000 at reset.
constexpr uint32_t kGpubBits2 = (1u << 24) | (1u << 25), kGpubBits4 = (1u << 8) | (1u << 9);
constexpr uint32_t kGpubAsids[4] = {TEGRA_SMMU_ASID_GPU_LOW, TEGRA_SMMU_ASID_GPU_HIGH, TEGRA_SMMU_ASID_GPU,
                                    TEGRA_SMMU_ASID_FAULT};
constexpr uint32_t kGpubPtbs[4] = {0xe0080001, 0xe0080002, 0xe0080003, 0xe0080004};
constexpr uint32_t kGpubSelected = (1u << 31) | TEGRA_SMMU_ASID_GPU_LOW | TEGRA_SMMU_ASID_GPU_HIGH << 8 |
                                   TEGRA_SMMU_ASID_GPU << 16 | TEGRA_SMMU_ASID_FAULT << 24;

int attachGpub(uint32_t translationReg, uint32_t bits, const uint32_t *ptbs = kGpubPtbs)
{
    char line[160] = {};
    return tegra_smmu_attach_split(kGpubAsids, ptbs, TEGRA_SMMU_GPUB_ASID, translationReg, bits, true, line,
                                   sizeof(line));
}

// The 24 Nov boots: "the swgroup is already translated by ASID 0 (MC 0xaac = 0x80000000)". The register's reset value
// names ASID 0 because its enable cannot be cleared, not because anyone owns it.
TEST_F(SmmuEngineTest, GpubsResetValueRefusesAPlainAttachButNotAnAlwaysTranslatedOne)
{
    gRegisters[TEGRA_SMMU_TRANSLATION_ENABLE_2] = kGpubBits2;
    gRegisters[TEGRA_SMMU_TRANSLATION_ENABLE_4] = kGpubBits4;
    char line[160] = {};
    EXPECT_EQ(tegra_smmu_attach(TEGRA_SMMU_ASID_GPU, 0xe0080003, TEGRA_SMMU_GPUB_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_2,
                                kGpubBits2, line, sizeof(line)),
              -1);
    EXPECT_STREQ(line, "the swgroup is already translated by ASID 0 (MC 0xaac = 0x80000000)");

    ASSERT_EQ(attachGpub(TEGRA_SMMU_TRANSLATION_ENABLE_2, kGpubBits2), 0);
    ASSERT_EQ(attachGpub(TEGRA_SMMU_TRANSLATION_ENABLE_4, kGpubBits4), 0) << "the second register, same directories";
    EXPECT_EQ(gRegisters[TEGRA_SMMU_GPUB_ASID], kGpubSelected);
    for (uint32_t field = 0; field < 4; ++field)
        EXPECT_EQ(gPtbByAsid[kGpubAsids[field]], kGpubPtbs[field]) << "field " << field;

    gRegisters.erase(TEGRA_SMMU_GPUB_ASID);
    gPtbByAsid.clear();
    EXPECT_EQ(tegra_smmu_resume(), 0) << "SC7: every field's directory again";
    EXPECT_EQ(gRegisters[TEGRA_SMMU_GPUB_ASID], kGpubSelected);
    EXPECT_EQ(gPtbByAsid[TEGRA_SMMU_ASID_GPU_HIGH], kGpubPtbs[1]);

    tegra_smmu_detach(TEGRA_SMMU_ASID_GPU_LOW, TEGRA_SMMU_GPUB_ASID);
    EXPECT_EQ(gRegisters[TEGRA_SMMU_GPUB_ASID], (1u << 31) | 0x74747474u) << "parked on the fault ASID";
    EXPECT_EQ(gPtbByAsid[TEGRA_SMMU_ASID_GPU_LOW], 0u);
    EXPECT_EQ(gPtbByAsid[TEGRA_SMMU_ASID_GPU_HIGH], 0u);
    EXPECT_EQ(gPtbByAsid[TEGRA_SMMU_ASID_GPU], 0u);
    EXPECT_EQ(gPtbByAsid[TEGRA_SMMU_ASID_FAULT], kGpubPtbs[3]) << "the fault directory stays for other swgroups";
    ASSERT_EQ(attachGpub(TEGRA_SMMU_TRANSLATION_ENABLE_2, kGpubBits2), 0) << "a parked GPUB is nobody's";
    tegra_smmu_detach(TEGRA_SMMU_ASID_GPU_LOW, TEGRA_SMMU_GPUB_ASID);
}

// GPUB's client bits are read-only, "GPU translation is handled by swid" (TRM 18.11.1.68): whatever they read, the
// swgroup translates, so they cannot keep it unattached (and the GPU unconfined) as its enable bit once did.
TEST_F(SmmuEngineTest, GpubsReadOnlyClientBitsDoNotGateItsAttach)
{
    ASSERT_EQ(attachGpub(TEGRA_SMMU_TRANSLATION_ENABLE_2, kGpubBits2), 0);
    ASSERT_EQ(attachGpub(TEGRA_SMMU_TRANSLATION_ENABLE_4, kGpubBits4), 0);
    EXPECT_EQ(gRegisters[TEGRA_SMMU_GPUB_ASID], kGpubSelected);
    tegra_smmu_detach(TEGRA_SMMU_ASID_GPU_LOW, TEGRA_SMMU_GPUB_ASID);
}

// The emulator's MC keeps what is written to 0xaac; its GPU never sends a[34] = 1 through a directory.
TEST_F(SmmuEngineTest, AGpubWhoseEnableReadsZeroIsNotTegraX1sAndIsNotAttached)
{
    gRegisters[TEGRA_SMMU_TRANSLATION_ENABLE_2] = kGpubBits2;
    gGpubHardwired = false;
    char line[160] = {};
    EXPECT_EQ(tegra_smmu_attach_split(kGpubAsids, kGpubPtbs, TEGRA_SMMU_GPUB_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_2,
                                      kGpubBits2, true, line, sizeof(line)),
              -1);
    gGpubHardwired = true;
    EXPECT_STREQ(line, "the swgroup's enable reads 0 (MC 0xaac = 0), but Tegra X1 hardwires it to 1");
    EXPECT_EQ(gRegisters[TEGRA_SMMU_GPUB_ASID], 0u);
}

TEST_F(SmmuEngineTest, AGpubAttachWithOtherDirectoriesOrSomeoneElsesTranslationIsRefused)
{
    gRegisters[TEGRA_SMMU_TRANSLATION_ENABLE_2] = kGpubBits2;
    gRegisters[TEGRA_SMMU_TRANSLATION_ENABLE_4] = kGpubBits4;
    gRegisters[TEGRA_SMMU_GPUB_ASID] = (1u << 31) | 5u | 5u << 8;
    EXPECT_EQ(attachGpub(TEGRA_SMMU_TRANSLATION_ENABLE_2, kGpubBits2), -1) << "ASID 5 is not the reset value";
    gRegisters[TEGRA_SMMU_GPUB_ASID] = 1u << 31;
    ASSERT_EQ(attachGpub(TEGRA_SMMU_TRANSLATION_ENABLE_2, kGpubBits2), 0);
    const uint32_t other[4] = {kGpubPtbs[0], 0xe0080009, kGpubPtbs[2], kGpubPtbs[3]};
    EXPECT_EQ(attachGpub(TEGRA_SMMU_TRANSLATION_ENABLE_4, kGpubBits4, other), -1);
    tegra_smmu_detach(TEGRA_SMMU_ASID_GPU_LOW, TEGRA_SMMU_GPUB_ASID);
}

// The SD card's view shares the fault ASID's directory with GM20B's and nothing else.
TEST_F(SmmuEngineTest, TwoSplitSwgroupsShareTheFaultAsidButNotEachOthersAsids)
{
    gRegisters[TEGRA_SMMU_TRANSLATION_ENABLE_2] = kGpubBits2;
    gRegisters[TEGRA_SMMU_TRANSLATION_ENABLE_3] = 1u | 1u << 4;
    ASSERT_EQ(attachGpub(TEGRA_SMMU_TRANSLATION_ENABLE_2, kGpubBits2), 0);
    char line[160] = {};
    const uint32_t sdAsids[4] = {TEGRA_SMMU_ASID_SDMMC1_LOW, TEGRA_SMMU_ASID_SDMMC1_HIGH, TEGRA_SMMU_ASID_FAULT,
                                 TEGRA_SMMU_ASID_FAULT};
    const uint32_t sdPtbs[4] = {0xe0080011, 0xe0080012, kGpubPtbs[3], kGpubPtbs[3]};
    ASSERT_EQ(tegra_smmu_attach_split(sdAsids, sdPtbs, TEGRA_SMMU_SDMMC1A_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_3,
                                      1u | 1u << 4, false, line, sizeof(line)),
              0)
        << line;
    EXPECT_EQ(gRegisters[TEGRA_SMMU_SDMMC1A_ASID], (1u << 31) | 113u | 112u << 8 | 116u << 16 | 116u << 24);
    tegra_smmu_detach(TEGRA_SMMU_ASID_SDMMC1_LOW, TEGRA_SMMU_SDMMC1A_ASID);
    EXPECT_EQ(gRegisters[TEGRA_SMMU_SDMMC1A_ASID], 0u) << "SDMMC1A can bypass: its detach clears it";

    const uint32_t stealing[4] = {TEGRA_SMMU_ASID_GPU_LOW, TEGRA_SMMU_ASID_SDMMC1_HIGH, TEGRA_SMMU_ASID_FAULT,
                                  TEGRA_SMMU_ASID_FAULT};
    EXPECT_EQ(tegra_smmu_attach_split(stealing, sdPtbs, TEGRA_SMMU_SDMMC1A_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_3,
                                      1u | 1u << 4, false, line, sizeof(line)),
              -1)
        << "GM20B's ASID with another directory";
    tegra_smmu_detach(TEGRA_SMMU_ASID_GPU_LOW, TEGRA_SMMU_GPUB_ASID);
}

TEST_F(SmmuEngineTest, MapRespectsReserveHeadroomAndUnmapsExactCount)
{
    TegraSMMUEngine engine;
    ASSERT_TRUE(engine.init(TEGRA_SMMU_ASID_GPU, kWindowBase, kWindowBytes, 0));

    std::vector<uint64_t> pages(16 * 16);
    for (size_t i = 0; i < pages.size(); ++i)
        pages[i] = 0x80000000ULL + (i << 12);

    const uint64_t reserve = 2ULL << 20;
    const uint64_t first = engine.map(pages.data(), static_cast<uint32_t>(pages.size()), true, reserve);
    ASSERT_NE(first, 0u);
    const uint64_t second = engine.map(pages.data(), static_cast<uint32_t>(pages.size()), true, reserve);
    ASSERT_NE(second, 0u);
    EXPECT_EQ(engine.map(pages.data(), 16, true, reserve), 0u) << "cannot dip into reserved 2 MiB";

    const uint64_t comp = engine.map(pages.data(), static_cast<uint32_t>(pages.size()), true);
    ASSERT_NE(comp, 0u) << "unreserved allocation still consumes reserved headroom";

    uint64_t total = 0, freeBytes = 0;
    engine.usage(&total, &freeBytes);
    EXPECT_EQ(total, kWindowBytes);
    EXPECT_EQ(freeBytes, 1ULL << 20);

    engine.unmap(comp, static_cast<uint32_t>(pages.size()));
    engine.unmap(second, static_cast<uint32_t>(pages.size()));
    engine.unmap(first, static_cast<uint32_t>(pages.size()));
    engine.usage(&total, &freeBytes);
    EXPECT_EQ(freeBytes, kWindowBytes);

    engine.release();
}

TEST_F(SmmuEngineTest, FreshMapDoesNoMcFlushAndUnmapFlushesBothAtomsOfEveryGranule)
{
    TegraSMMUEngine engine;
    ASSERT_TRUE(engine.init(TEGRA_SMMU_ASID_VIC, kWindowBase, kWindowBytes, 0));
    gFlushes.clear();

    uint64_t pages[16];
    for (uint32_t i = 0; i < 16; ++i)
        pages[i] = 0x80000000ULL + (uint64_t(i) << 12);

    const uint64_t iova = engine.map(pages, 16, true);
    ASSERT_NE(iova, 0u);
    EXPECT_TRUE(gFlushes.empty()) << "fresh map in pre-reserved 64 KiB window needs no PTC/TLB flush";

    // Unmapping a 1-page mapping in a 64 KiB granule must still flush both 32-byte PTE atoms (64 bytes) and TLB.
    engine.unmap(iova, 1);
    uint32_t ptcWrites = 0, tlbWrites = 0;
    for (const auto &[offset, value] : gFlushes) {
        if (offset == 0x34)
            ++ptcWrites;
        else if (offset == 0x30)
            ++tlbWrites;
    }
    EXPECT_EQ(ptcWrites, 2u);
    EXPECT_EQ(tlbWrites, 1u);

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
    if (offset == TEGRA_SMMU_GPUB_ASID && gGpubHardwired)
        return gRegisters[offset] | 1u << 31;
    return gRegisters[offset];
}

void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
    const uint32_t offset = static_cast<uint32_t>(address - kMc);
    gRegisters[offset] = value;
    if (offset == kPtbData)
        gPtbByAsid[gRegisters[kPtbAsid] & 0x7fu] = value;
    if (offset == 0x34 || offset == 0x30)
        gFlushes.emplace_back(offset, value);
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

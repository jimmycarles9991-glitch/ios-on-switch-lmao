// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "switch_wlan_dma_stubs.h"
#include "../SwitchWLANDma.h"
#include "../../TegraSMMU/TegraSMMU.h"
#include "../../TegraSMMU/TegraSMMUDomain.h"
#include "../../../kits/SwitchDMA/SwitchDMABuffer.h"
#include "bazel/testing/fresh_process.h"

#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr uint64_t kTablesPa = 0x98000000, kPool = 0x9a3ff000, kFramebuffer = 0xf5a00000;
constexpr size_t kLarge = 2u << 20, kSmall = 512u << 10;
alignas(4096) std::array<uint32_t, 3072> switch_tables;
IOBufferMemoryDescriptor switch_memory;
uint64_t switch_table_pa = kTablesPa;
uint64_t switch_unmanaged_page = UINT64_MAX;
bool switch_allocate_fails, switch_restore_fails;
unsigned switch_allocations, switch_attach_failure, switch_restores, switch_detaches, switch_frees;

struct SwitchDmaAttachment {
    uint32_t asid, ptb, group, reg, bits;
    bool wide;
};

std::vector<SwitchDmaAttachment> switch_attachments;
std::vector<std::string> switch_events;
std::vector<uintptr_t> switch_cleaned;
std::vector<uint64_t> switch_checked_pages;
char switch_why[160];

TegraSMMUDomain switch_view()
{
    TegraSMMUDomain domain;
    domain.va = switch_tables.data();
    domain.pa = switch_table_pa;
    domain.pages = 3;
    return domain;
}

uint32_t switch_ptb()
{
    return switch_view().ptb();
}

void switch_expect_mapping(uint64_t address = 0, size_t bytes = 0)
{
    const auto domain = switch_view();
    unsigned mapped = 0, directories = 0;
    for (unsigned directory = 0; directory < TegraSMMUDomain::kEntries; ++directory) {
        const uint32_t pde = switch_tables[directory];
        if (!pde)
            continue;
        ++directories;
        EXPECT_EQ(pde & ~TegraSMMUDomain::kPfnMask, TegraSMMUDomain::kRead | TegraSMMUDomain::kWrite |
                                                        TegraSMMUDomain::kNonSecure | TegraSMMUDomain::kNext);
        const uint64_t table = static_cast<uint64_t>(pde & TegraSMMUDomain::kPfnMask) << 12;
        REQUIRE(table >= switch_table_pa + 4096 && table < switch_table_pa + sizeof switch_tables);
        const uint32_t *entries = domain.table(static_cast<uint32_t>((table - switch_table_pa) / 4096));
        for (unsigned entry = 0; entry < TegraSMMUDomain::kEntries; ++entry) {
            if (!entries[entry])
                continue;
            ++mapped;
            const uint64_t iova = static_cast<uint64_t>(directory) * TegraSMMUDomain::kSpan + entry * 4096u;
            EXPECT_GE(iova, address);
            EXPECT_LT(iova, address + bytes);
            EXPECT_EQ(entries[entry], TegraSMMUDomain::entry(iova, true));
            EXPECT_EQ(domain.translate(iova + 4095), iova + 4095);
        }
    }
    EXPECT_EQ(mapped, bytes / 4096);
    const unsigned expected_directories = bytes ? static_cast<unsigned>((address + bytes - 1) / TegraSMMUDomain::kSpan -
                                                                        address / TegraSMMUDomain::kSpan + 1)
                                                : 0;
    EXPECT_EQ(directories, expected_directories);
    if (bytes) {
        EXPECT_EQ(domain.translate(address - 1), UINT64_MAX);
        EXPECT_EQ(domain.translate(address + bytes), UINT64_MAX);
    }
    for (uint64_t at = switch_table_pa; at < switch_table_pa + sizeof switch_tables; at += 4096)
        EXPECT_EQ(domain.translate(at), UINT64_MAX);
    EXPECT_EQ(domain.translate(kFramebuffer), UINT64_MAX);
    EXPECT_EQ(switch_detaches, 0u);
    EXPECT_EQ(switch_frees, 0u);
}

bool switch_start()
{
    return switch_wlan_dma_start(switch_why, sizeof switch_why);
}

bool switch_map(uint64_t address, size_t bytes)
{
    return switch_wlan_dma_map(address, bytes, switch_why, sizeof switch_why);
}
}

void switch_test_dma_cache(const char *operation, uintptr_t address)
{
    EXPECT_STREQ(operation, "cvac");
    EXPECT_GE(address, reinterpret_cast<uintptr_t>(switch_tables.data()));
    EXPECT_LT(address, reinterpret_cast<uintptr_t>(switch_tables.data()) + sizeof switch_tables);
    switch_cleaned.push_back(address);
    switch_events.emplace_back("clean");
}

void switch_test_dma_barrier()
{
    switch_events.emplace_back("barrier");
}

bool switch_dma_pa_managed(uint64_t address)
{
    switch_checked_pages.push_back(address);
    return address >= 0x80000000 && address < 0xf0000000 && address != switch_unmanaged_page;
}

bool switch_dma_alloc(switch_dma_buffer *out, size_t size, uint64_t physical_mask, IOOptionBits options)
{
    ++switch_allocations;
    EXPECT_EQ(size, sizeof switch_tables);
    EXPECT_EQ(physical_mask, kSwitchDMABelow4G);
    EXPECT_EQ(options, 0u);
    if (switch_allocate_fails)
        return false;
    switch_tables.fill(0);
    *out = {&switch_memory, reinterpret_cast<uint8_t *>(switch_tables.data()), switch_table_pa, size};
    return true;
}

void switch_dma_free(switch_dma_buffer *)
{
    ++switch_frees;
}

extern "C" int tegra_smmu_attach(uint32_t asid, uint32_t ptb, uint32_t group, uint32_t reg, uint32_t bits, char *,
                                 size_t)
{
    switch_attachments.push_back({asid, ptb, group, reg, bits, false});
    switch_events.emplace_back("attach");
    return switch_attach_failure == switch_attachments.size() ? -1 : 0;
}

extern "C" int tegra_smmu_attach_wide(uint32_t asid, uint32_t ptb, uint32_t group, uint32_t reg, uint32_t bits, char *,
                                      size_t)
{
    switch_attachments.push_back({asid, ptb, group, reg, bits, true});
    return -1;
}

extern "C" int switch_smmu_restore(uint32_t asid, uint32_t ptb, uint32_t group, char *, size_t)
{
    ++switch_restores;
    EXPECT_EQ(asid, 118u);
    EXPECT_EQ(ptb, switch_ptb());
    EXPECT_EQ(group, 0x238u);
    switch_events.emplace_back("restore");
    return switch_restore_fails ? -1 : 0;
}

extern "C" void tegra_smmu_flush(uint32_t asid)
{
    EXPECT_EQ(asid, 118u);
    switch_events.emplace_back("flush");
}

extern "C" void tegra_smmu_detach(uint32_t, uint32_t)
{
    ++switch_detaches;
}

TEST(SwitchWlanDma, StartsWithEmptyDomainBeforeAnyPoolIsAvailable)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        EXPECT_FALSE(switch_map(kPool, kLarge));
        EXPECT_EQ(switch_allocations, 0u);
        REQUIRE(switch_start());
        REQUIRE(switch_attachments.size() == 2);
        for (const auto &attachment : switch_attachments) {
            EXPECT_EQ(attachment.asid, 118u);
            EXPECT_EQ(attachment.ptb, switch_ptb());
            EXPECT_EQ(attachment.group, 0x238u);
            EXPECT_FALSE(attachment.wide);
        }
        EXPECT_EQ(switch_attachments[0].reg, 0x228u);
        EXPECT_EQ(switch_attachments[0].bits, 1u << 14);
        EXPECT_EQ(switch_attachments[1].reg, 0x22cu);
        EXPECT_EQ(switch_attachments[1].bits, 1u << 17);
        REQUIRE(switch_events.size() >= 4);
        EXPECT_EQ(switch_events[switch_events.size() - 4], "barrier");
        EXPECT_EQ(switch_events[switch_events.size() - 3], "flush");
        EXPECT_EQ(switch_events.back(), "attach");
        EXPECT_EQ(switch_cleaned.size(), 4096u / 64);
        switch_expect_mapping();
    });
}

TEST(SwitchWlanDma, MapsOnlyTheTwoMiBPoolAcrossDirectoryBoundary)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        REQUIRE(switch_start());
        switch_events.clear();
        REQUIRE(switch_map(kPool, kLarge));
        REQUIRE(switch_checked_pages.size() == kLarge / 4096);
        for (size_t index = 0; index < switch_checked_pages.size(); ++index)
            EXPECT_EQ(switch_checked_pages[index], kPool + index * 4096);
        REQUIRE(switch_events.size() >= 3);
        EXPECT_EQ(switch_events[switch_events.size() - 3], "barrier");
        EXPECT_EQ(switch_events[switch_events.size() - 2], "flush");
        EXPECT_EQ(switch_events.back(), "restore");
        switch_expect_mapping(kPool, kLarge);
    });
}

TEST(SwitchWlanDma, MapsOnlyTheFallbackPool)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        REQUIRE(switch_start());
        REQUIRE(switch_map(0x9b100000, kSmall));
        switch_expect_mapping(0x9b100000, kSmall);
    });
}

TEST(SwitchWlanDma, RejectsInvalidRangesWithoutChangingTheEmptyTables)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        REQUIRE(switch_start());
        const std::pair<uint64_t, size_t> ranges[] = {
            {kPool, 0},           {kPool + 1, kSmall}, {kPool, kSmall - 1},       {kPool, kLarge + 4096},
            {1ULL << 32, kSmall}, {0xfffff000, 8192},  {UINT64_MAX - 4095, 4096},
        };
        for (const auto &range : ranges)
            EXPECT_FALSE(switch_map(range.first, range.second));
        EXPECT_EQ(switch_restores, 0u);
        switch_expect_mapping();
    });
}

TEST(SwitchWlanDma, RejectsAnyIntersectionWithItsTableAllocation)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        REQUIRE(switch_start());
        EXPECT_FALSE(switch_map(kTablesPa - 4096, 8192));
        EXPECT_FALSE(switch_map(kTablesPa, 4096));
        EXPECT_FALSE(switch_map(kTablesPa + 8192, 8192));
        EXPECT_EQ(switch_restores, 0u);
        switch_expect_mapping();
    });
}

TEST(SwitchWlanDma, RejectsBelowDramAndMmioWithoutPublishingAnyMappings)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        REQUIRE(switch_start());
        const auto tables = switch_tables;
        const auto events = switch_events;
        EXPECT_FALSE(switch_map(0x1000, 4096));
        EXPECT_FALSE(switch_map(0x70019000, 4096));
        EXPECT_EQ(switch_tables, tables);
        EXPECT_EQ(switch_events, events);
        EXPECT_EQ(switch_restores, 0u);
        switch_expect_mapping();
    });
}

TEST(SwitchWlanDma, RejectsAnUnmanagedMiddlePageBeforeWritingAnyPte)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        REQUIRE(switch_start());
        const auto tables = switch_tables;
        const auto events = switch_events;
        switch_unmanaged_page = kPool + kLarge / 2;
        EXPECT_FALSE(switch_map(kPool, kLarge));
        REQUIRE(switch_checked_pages.size() == kLarge / 2 / 4096 + 1);
        EXPECT_EQ(switch_checked_pages.back(), switch_unmanaged_page);
        EXPECT_EQ(switch_tables, tables);
        EXPECT_EQ(switch_events, events);
        EXPECT_EQ(switch_restores, 0u);
        switch_expect_mapping();
        switch_unmanaged_page = UINT64_MAX;
        REQUIRE(switch_map(0x9b100000, kSmall));
        switch_expect_mapping(0x9b100000, kSmall);
    });
}

TEST(SwitchWlanDma, ReusesTheSameTablesAndMappingAcrossRecovery)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        REQUIRE(switch_start());
        REQUIRE(switch_map(kPool, kLarge));
        const auto tables = switch_tables;
        const size_t cleaned = switch_cleaned.size();
        REQUIRE(switch_start());
        REQUIRE(switch_map(kPool, kLarge));
        EXPECT_EQ(switch_allocations, 1u);
        EXPECT_EQ(switch_attachments.size(), 2u);
        EXPECT_EQ(switch_restores, 3u);
        EXPECT_EQ(switch_cleaned.size(), cleaned);
        EXPECT_EQ(switch_tables, tables);
        EXPECT_FALSE(switch_map(kPool + 4096, kLarge));
        EXPECT_FALSE(switch_map(kPool, kSmall));
        switch_expect_mapping(kPool, kLarge);
    });
}

TEST(SwitchWlanDma, FailedAllocationCannotAttachOrMapAndCanRetry)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        switch_allocate_fails = true;
        EXPECT_FALSE(switch_start());
        EXPECT_FALSE(switch_map(kPool, kSmall));
        EXPECT_TRUE(switch_attachments.empty());
        EXPECT_TRUE(switch_events.empty());
        switch_allocate_fails = false;
        REQUIRE(switch_start());
        EXPECT_EQ(switch_allocations, 2u);
        switch_expect_mapping();
    });
}

class SwitchWlanDmaAttachmentFailure : public ::testing::TestWithParam<unsigned> {};

TEST_P(SwitchWlanDmaAttachmentFailure, RefusesMappingUntilBothClientsHaveAttached)
{
    const unsigned failed = GetParam();
    switch_ios::test::ExpectPassesInFreshProcess([failed] {
        switch_attach_failure = failed;
        EXPECT_FALSE(switch_start());
        EXPECT_FALSE(switch_map(kPool, kSmall));
        EXPECT_EQ(switch_restores, 0u);
        EXPECT_EQ(switch_attachments.size(), failed);
        switch_expect_mapping();
        switch_attach_failure = 0;
        REQUIRE(switch_start());
        REQUIRE(switch_map(kPool, kSmall));
        EXPECT_EQ(switch_allocations, 1u);
        switch_expect_mapping(kPool, kSmall);
    });
}

INSTANTIATE_TEST_SUITE_P(Client, SwitchWlanDmaAttachmentFailure, ::testing::Values(1u, 2u));

class SwitchWlanDmaTableFailure : public ::testing::TestWithParam<uint64_t> {};

TEST_P(SwitchWlanDmaTableFailure, RefusesInvalidTablePhysicalAddresses)
{
    const uint64_t address = GetParam();
    switch_ios::test::ExpectPassesInFreshProcess([address] {
        switch_table_pa = address;
        EXPECT_FALSE(switch_start());
        EXPECT_FALSE(switch_start());
        EXPECT_FALSE(switch_map(kPool, kSmall));
        EXPECT_EQ(switch_allocations, 1u);
        EXPECT_TRUE(switch_attachments.empty());
        EXPECT_TRUE(switch_cleaned.empty());
    });
}

INSTANTIATE_TEST_SUITE_P(Physical, SwitchWlanDmaTableFailure,
                         ::testing::Values(kTablesPa + 1, 1ULL << 32, 0xffffe000ULL));

TEST(SwitchWlanDma, FailedRestoreRetainsThePoolAndNeverFallsBackToBypass)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        REQUIRE(switch_start());
        switch_restore_fails = true;
        EXPECT_FALSE(switch_map(kPool, kLarge));
        const auto tables = switch_tables;
        EXPECT_FALSE(switch_map(kPool + 4096, kLarge));
        EXPECT_FALSE(switch_map(kPool, kSmall));
        EXPECT_FALSE(switch_start());
        EXPECT_EQ(switch_tables, tables);
        EXPECT_EQ(switch_attachments.size(), 2u);
        switch_expect_mapping(kPool, kLarge);
        switch_restore_fails = false;
        REQUIRE(switch_start());
        REQUIRE(switch_map(kPool, kLarge));
        EXPECT_EQ(switch_allocations, 1u);
        switch_expect_mapping(kPool, kLarge);
    });
}

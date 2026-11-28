// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <switch/pmuv3.h>

namespace {

std::string readFile(const char *path)
{
    std::ifstream in(path);
    EXPECT_TRUE(in.is_open()) << "failed to open " << path;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string extractPmuv3Branch(const std::string &src, const std::string &marker)
{
    const size_t start = src.find(marker);
    EXPECT_NE(start, std::string::npos) << "marker not found: " << marker;
    if (start == std::string::npos) {
        return {};
    }
    int depth = 1;
    size_t pos = src.find('\n', start);
    while (pos != std::string::npos && depth > 0) {
        const size_t next = src.find('\n', pos + 1);
        const std::string line = src.substr(pos + 1, next == std::string::npos ? std::string::npos : next - pos - 1);
        if (line.rfind("#if", 0) == 0) {
            depth++;
        } else if (line.rfind("#else", 0) == 0 || line.rfind("#elif", 0) == 0) {
            if (depth == 1) {
                return src.substr(start, pos - start);
            }
        } else if (line.rfind("#endif", 0) == 0) {
            depth--;
            if (depth == 0) {
                return src.substr(start, pos - start);
            }
        }
        pos = next;
    }
    return src.substr(start);
}

void expectNoApplePmuSysregs(const std::string &code, const char *label)
{
    static const char *const kForbidden[] = {
        "PMCR0_EL1", "PMCR1_EL1", "PMCR2_EL1", "PMSR_EL1", "PMESR0_EL1", "PMESR1_EL1",
        "S3_1_C15_", "S3_2_C15_", "S3_7_C15_", "\"PMC0\"", "\"PMC1\"",   "\"PMC2\"",
    };
    for (const char *token : kForbidden) {
        EXPECT_EQ(code.find(token), std::string::npos) << label << " must not reference Apple CPMU register " << token;
    }
}

TEST(SwitchPmuv3, EvtyperModeFiltering)
{
    const uint32_t both = switch_pmuv3_evtyper(SWITCH_PMUV3_EVENT_INST_RETIRED, true, true);
    EXPECT_EQ(both, 0x00000008U);
    EXPECT_EQ(switch_pmuv3_evtyper_event(both), 0x0008U);
    EXPECT_TRUE(switch_pmuv3_evtyper_counts_user(both));
    EXPECT_TRUE(switch_pmuv3_evtyper_counts_kernel(both));
    EXPECT_EQ(both & SWITCH_PMEVTYPER_NSH, 0U);

    const uint32_t userOnly = switch_pmuv3_evtyper(0x0013U, true, false);
    EXPECT_EQ(userOnly, SWITCH_PMEVTYPER_P | 0x0013U);
    EXPECT_TRUE(switch_pmuv3_evtyper_counts_user(userOnly));
    EXPECT_FALSE(switch_pmuv3_evtyper_counts_kernel(userOnly));

    const uint32_t kernelOnly = switch_pmuv3_evtyper(0x0016U, false, true);
    EXPECT_EQ(kernelOnly, SWITCH_PMEVTYPER_U | 0x0016U);
    EXPECT_FALSE(switch_pmuv3_evtyper_counts_user(kernelOnly));
    EXPECT_TRUE(switch_pmuv3_evtyper_counts_kernel(kernelOnly));

    const uint32_t neither = switch_pmuv3_evtyper(0xFFFFU, false, false);
    EXPECT_EQ(switch_pmuv3_evtyper_event(neither), SWITCH_PMUV3_EVENT_MASK);
    EXPECT_FALSE(switch_pmuv3_evtyper_counts_user(neither));
    EXPECT_FALSE(switch_pmuv3_evtyper_counts_kernel(neither));
}

TEST(SwitchPmuv3, ReloadAndDeltaMath)
{
    EXPECT_TRUE(switch_pmuv3_is_64bit(0));
    EXPECT_FALSE(switch_pmuv3_is_64bit(1));
    EXPECT_FALSE(switch_pmuv3_is_64bit(6));

    EXPECT_EQ(switch_pmuv3_idle_reload(0), UINT64_C(0));
    EXPECT_EQ(switch_pmuv3_idle_reload(1), UINT64_C(0x80000000));
    EXPECT_EQ(switch_pmuv3_idle_reload(6), UINT64_C(0x80000000));

    // 64-bit PMCCNTR_EL0 reloads and wraps.
    EXPECT_EQ(switch_pmuv3_reload_value(0, 0), UINT64_C(0));
    EXPECT_EQ(switch_pmuv3_reload_value(0, 1), UINT64_MAX);
    const uint64_t cycReload = switch_pmuv3_reload_value(0, 50000000ULL);
    EXPECT_EQ(switch_pmuv3_delta(0, cycReload, 123ULL), 50000123ULL);

    // 32-bit PMEVCNTR0..5_EL0 reloads, boundary stepping, and wraps.
    EXPECT_EQ(switch_pmuv3_reload_value(1, 0), SWITCH_PMUV3_IDLE_RELOAD_32);
    EXPECT_EQ(switch_pmuv3_reload_value(1, 1), UINT64_C(0xFFFFFFFF));
    EXPECT_EQ(switch_pmuv3_reload_value(1, 10), UINT64_C(0xFFFFFFF6));
    EXPECT_EQ(switch_pmuv3_reload_value(1, SWITCH_PMUV3_MAX_PERIOD_32), SWITCH_PMUV3_IDLE_RELOAD_32);
    EXPECT_EQ(switch_pmuv3_reload_value(1, UINT64_C(0x100000001)), SWITCH_PMUV3_IDLE_RELOAD_32);

    const uint64_t insReload = switch_pmuv3_reload_value(1, 50000000ULL);
    EXPECT_GE(insReload, SWITCH_PMUV3_IDLE_RELOAD_32);
    EXPECT_EQ(switch_pmuv3_delta(1, insReload, 77ULL), 50000077ULL);
    EXPECT_EQ((UINT64_C(1) << 32) - switch_pmuv3_reload_value(2, 100000ULL), 100000ULL);
    EXPECT_EQ((UINT64_C(1) << 32) - switch_pmuv3_reload_value(2, SWITCH_PMUV3_MAX_PERIOD_32),
              SWITCH_PMUV3_MAX_PERIOD_32);

    // Free-running wrap followed by an in-flight recount read before PMI must never double-count.
    const uint64_t prevBeforeWrap = UINT64_C(0xFFFFF000);
    const uint64_t curAtRecount = UINT64_C(0x00000010);
    const uint64_t curAtPmi = UINT64_C(0x00000040);
    const uint64_t d1 = switch_pmuv3_delta(1, prevBeforeWrap, curAtRecount);
    const uint64_t d2 = switch_pmuv3_delta(1, curAtRecount, curAtPmi);
    EXPECT_EQ(d1, UINT64_C(0x1010));
    EXPECT_EQ(d2, UINT64_C(0x30));
    EXPECT_EQ(d1 + d2, UINT64_C(0x1040));
}

TEST(SwitchPmuv3, SlotAndOverflowMaskMapping)
{
    EXPECT_EQ(switch_pmuv3_slot_hw_bit(0), 31U);
    for (unsigned int slot = 1; slot < SWITCH_PMUV3_PMC_COUNT; ++slot) {
        EXPECT_EQ(switch_pmuv3_slot_hw_bit(slot), slot - 1U);
    }
    EXPECT_EQ(switch_pmuv3_slot_hw_bit(SWITCH_PMUV3_PMC_COUNT), UINT32_MAX);
    EXPECT_EQ(switch_pmuv3_slot_hw_mask(SWITCH_PMUV3_PMC_COUNT), 0U);

    EXPECT_EQ(switch_pmuv3_slot_mask_to_hw(0x03U), SWITCH_PMUV3_FIXED_HW_MASK);
    EXPECT_EQ(switch_pmuv3_slot_mask_to_hw(0x7FU), SWITCH_PMUV3_ALL_HW_MASK);
    EXPECT_EQ(switch_pmuv3_hw_mask_to_slots(SWITCH_PMUV3_FIXED_HW_MASK), 0x03U);
    EXPECT_EQ(switch_pmuv3_hw_mask_to_slots(SWITCH_PMUV3_ALL_HW_MASK), 0x7FU);

    for (uint32_t mask = 0; mask < (1U << SWITCH_PMUV3_PMC_COUNT); ++mask) {
        EXPECT_EQ(switch_pmuv3_hw_mask_to_slots(switch_pmuv3_slot_mask_to_hw(mask)), mask);
    }
}

TEST(SwitchPmuv3, Tegra210SpiMappingAndSnapshotDefaults)
{
    for (uint32_t cpu = 0; cpu < SWITCH_PMUV3_MAX_CPUS; ++cpu) {
        const uint32_t intid = switch_pmuv3_cpu_intid(cpu);
        EXPECT_EQ(intid, 176U + cpu);
        EXPECT_EQ(switch_pmuv3_intid_to_cpu(intid), static_cast<int32_t>(cpu));
    }
    EXPECT_EQ(switch_pmuv3_cpu_intid(4), 0U);
    EXPECT_EQ(switch_pmuv3_intid_to_cpu(175), -1);
    EXPECT_EQ(switch_pmuv3_intid_to_cpu(180), -1);

    struct switch_pmuv3_snapshot snap = {};
    switch_pmuv3_snapshot_init(&snap);
    EXPECT_EQ(snap.pmcr, SWITCH_PMCR_ENABLED);
    EXPECT_NE(snap.pmcr & SWITCH_PMCR_LC, 0U);
    EXPECT_EQ(snap.pmcnten, SWITCH_PMUV3_FIXED_HW_MASK);
    EXPECT_EQ(snap.pminten, SWITCH_PMUV3_FIXED_HW_MASK);
    EXPECT_EQ(snap.pmccfiltr, SWITCH_PMCCFILTR_ALL_NS);
    EXPECT_EQ(snap.pmccntr, 0U);
    EXPECT_EQ(snap.pmevtyper[0], SWITCH_PMUV3_EVENT_INST_RETIRED);
    for (unsigned int i = 0; i < SWITCH_PMUV3_EVENT_CTRS; ++i) {
        EXPECT_EQ(snap.pmevcntr[i], SWITCH_PMUV3_IDLE_RELOAD_32);
    }
}

TEST(SwitchPmuv3, BothXnuConfigsAndDriversWirePmuv3WithoutAppleSysregs)
{
    const std::string pmuv3Header = readFile(PMUV3_HEADER_PATH);
    expectNoApplePmuSysregs(pmuv3Header, "pmuv3.h");

    const std::string master10063 = readFile(MASTER_10063_PATH);
    EXPECT_NE(master10063.find("config_cpu_counters"), std::string::npos);
    EXPECT_NE(master10063.find("config_pervasive_cpi"), std::string::npos);

    const std::string master12377 = readFile(MASTER_12377_PATH);
    EXPECT_NE(master12377.find("config_cpu_counters"), std::string::npos);
    EXPECT_NE(master12377.find("config_pervasive_cpi"), std::string::npos);

    const std::string bcm10063 = readFile(BCM2837_10063_PATH);
    EXPECT_NE(bcm10063.find("#define SWITCH_ARMV8_PMUV3"), std::string::npos);
    EXPECT_NE(bcm10063.find("#define CPMU_AIC_PMI"), std::string::npos);

    const std::string bcm12377 = readFile(BCM2837_12377_PATH);
    EXPECT_NE(bcm12377.find("#define SWITCH_ARMV8_PMUV3"), std::string::npos);
    EXPECT_NE(bcm12377.find("#define CPMU_AIC_PMI"), std::string::npos);
    EXPECT_NE(bcm12377.find("#define CPMU_PMC_COUNT 7"), std::string::npos);

    const std::string board10063 = readFile(BOARD_10063_PATH);
    EXPECT_NE(board10063.find("#define CORE_NCTRS                     7"), std::string::npos);

    const std::string board12377 = readFile(BOARD_12377_PATH);
    EXPECT_NE(board12377.find("#define CORE_NCTRS                     7"), std::string::npos);

    expectNoApplePmuSysregs(extractPmuv3Branch(readFile(MONOTONIC_H_10063_PATH), "#if defined(SWITCH_ARMV8_PMUV3)"),
                            "10063/monotonic.h");
    expectNoApplePmuSysregs(extractPmuv3Branch(readFile(MONOTONIC_ARM64_10063_PATH), "#if defined(SWITCH_ARMV8_PMUV3)"),
                            "10063/monotonic_arm64.c");
    expectNoApplePmuSysregs(extractPmuv3Branch(readFile(KERN_MONOTONIC_10063_PATH),
                                               "#if defined(__arm64__) && defined(SWITCH_ARMV8_PMUV3)"),
                            "10063/kern_monotonic.c");
    expectNoApplePmuSysregs(extractPmuv3Branch(readFile(KPC_10063_PATH), "#elif defined(SWITCH_ARMV8_PMUV3)"),
                            "10063/kpc.c");
    expectNoApplePmuSysregs(extractPmuv3Branch(readFile(CPC_ARM64_H_12377_PATH), "#if defined(SWITCH_ARMV8_PMUV3)"),
                            "12377/cpc_arm64.h");
    expectNoApplePmuSysregs(extractPmuv3Branch(readFile(CPC_ARM64_C_12377_PATH), "#if defined(SWITCH_ARMV8_PMUV3)"),
                            "12377/cpc_arm64.c");
    expectNoApplePmuSysregs(extractPmuv3Branch(readFile(KPC_12377_PATH),
                                               "#if defined(SWITCH_ARMV8_PMUV3)\n\nuint32_t\nkpc_rawpmu_config_count"),
                            "12377/kpc.c");

    const std::string armGic = readFile(ARM_GIC_PATH);
    EXPECT_NE(armGic.find("{32 + 145, 1}"), std::string::npos);
    EXPECT_NE(armGic.find("{32 + 146, 2}"), std::string::npos);
    EXPECT_NE(armGic.find("{32 + 147, 3}"), std::string::npos);

    const std::string armCpu = readFile(ARM_CPU_PATH);
    EXPECT_NE(armCpu.find("registerPmuInterrupt"), std::string::npos);
    EXPECT_NE(armCpu.find("pmiHandler"), std::string::npos);
    EXPECT_NE(armCpu.find("32U + 144U"), std::string::npos);
}

}  // namespace

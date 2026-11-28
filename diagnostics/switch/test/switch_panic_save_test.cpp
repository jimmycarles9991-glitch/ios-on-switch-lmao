// Panic warm-reset guards on SwitchPanicSave.c and SwitchPanicEarly.c: Tegra210/T210B01 hangs on PMC_MAIN_RST while
// SCLK runs on PLLP, so pmc_reset() calls PSCI SYSTEM_RESET first and resets SCLK to POR before the PMC_MAIN_RST
// fallback, using the architectural counter so early panics do not depend on kernel clock state.
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::SourceText;

TEST(SwitchPanicSave, PmcResetCallsPsciSystemResetAndResetsSclkBeforeFallback)
{
    const std::string source = SourceText("../SwitchPanicSave.c");
    const CodeText text(source);
    EXPECT_TRUE(text.has("#define PSCI_SYSTEM_RESET UINT64_C(0x84000009)"));
    EXPECT_TRUE(text.has("#define CAR_PA UINT64_C(0x60006000)"));
    EXPECT_TRUE(text.has("#define CAR_SCLK_BURST_POLICY 0x028u"));
    EXPECT_TRUE(text.has("#define CAR_SUPER_SCLK_DIVIDER 0x030u"));
    EXPECT_TRUE(text.has("#define CAR_SCLK_POR 0x10000000u"));

    const CodeText reset = Block(source, "pmc_reset(void)");
    EXPECT_TRUE(reset.before("~PMC_SCRATCH0_MODES", "\"smc #0\""));
    EXPECT_TRUE(reset.before("register uint64_t x0 __asm__(\"x0\") = PSCI_SYSTEM_RESET;", "\"smc #0\""));
    EXPECT_TRUE(reset.before("\"smc #0\"", "car[CAR_SCLK_BURST_POLICY / 4] = CAR_SCLK_POR;"));
    EXPECT_TRUE(reset.before("car[CAR_SCLK_BURST_POLICY / 4] = CAR_SCLK_POR;",
                             "car[CAR_SUPER_SCLK_DIVIDER / 4] = 0;"));
    EXPECT_TRUE(reset.before("car[CAR_SUPER_SCLK_DIVIDER / 4] = 0;", "wait_us(2);"));
    EXPECT_TRUE(reset.before("wait_us(2);", "pmc[PMC_CNTRL / 4] = pmc[PMC_CNTRL / 4] | PMC_MAIN_RST;"));
    EXPECT_TRUE(reset.before("pmc[PMC_CNTRL / 4] = pmc[PMC_CNTRL / 4] | PMC_MAIN_RST;", "\"wfi\""));
    EXPECT_FALSE(reset.has("mach_absolute_time"));
    EXPECT_FALSE(reset.has("nanoseconds_to_absolutetime"));
}

TEST(SwitchPanicSave, CarMappingAndEarlyWaitAvoidKernelClockState)
{
    const std::string source = SourceText("../SwitchPanicSave.c");
    const CodeText init = Block(source, "void switch_panic_save_init(void)");
    EXPECT_TRUE(init.has("ml_io_map((vm_offset_t)CAR_PA, 4096)"));

    const CodeText early = Block(source, "void switch_panic_save_early(");
    EXPECT_TRUE(early.has("car = (volatile uint32_t *)(uintptr_t)CAR_PA;"));
    EXPECT_TRUE(early.has("wait_us("));
    EXPECT_FALSE(early.has("mach_absolute_time"));
    EXPECT_FALSE(early.has("nanoseconds_to_absolutetime"));

    const CodeText earlyMap(SourceText("../SwitchPanicEarly.c"));
    EXPECT_TRUE(earlyMap.has("#define MMIO_PA UINT64_C(0x40000000)"));

    const CodeText pending = Block(source, "const char *switch_panic_save_pending(");
    EXPECT_TRUE(pending.has("reboot_arg ? reboot_arg : SCREEN_REBOOT_S"));
}

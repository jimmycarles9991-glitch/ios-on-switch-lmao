// SPDX-License-Identifier: GPL-2.0
// tegra_dfll_cpu_prepare() (dfll.c, platform.c and the shim's clock tree as the kernel builds them) against
// tegra_dfll_fake.h's console: a failure the next call can get past is tried again, from where it stopped; only this
// chip's revision, its speedo fuses and the CVB row and tables they select end it for the boot (-ENODEV).
#include "drivers/TegraDFLL/TegraDFLL.h"
#include "drivers/TegraDFLL/test/tegra_dfll_fake.h"

#include "bazel/testing/fresh_process.h"

#include <gtest/gtest.h>

#include <string>

namespace
{
// Linux's errno values, as TegraDFLL returns them (macOS's EAGAIN is 35)
constexpr int kEAGAIN = -11, kENOMEM = -12, kEBUSY = -16, kENODEV = -19, kEINVAL = -22;
constexpr int kNack = -4;             // TEGRA_I2C_ENACK
constexpr int kLostArbitration = -5;  // TEGRA_I2C_EARB

std::string gWhy;

int prepare()
{
	char why[160] = "";
	const int err = tegra_dfll_cpu_prepare(why, sizeof(why));
	gWhy = why;
	return err;
}

volatile uint32_t &reg(tegra_dfll_fake_block block, uint32_t offset)
{
	return *tegra_dfll_fake_reg(block, offset);
}

// Fails for `broken` with `expected` and `why`, works once `fix` has run, and stays prepared.
template <class Break, class Fix>
void triedAgain(int expected, const char *why, Break broken, Fix fix)
{
	tegra_dfll_fake_console();
	broken();
	EXPECT_EQ(prepare(), expected) << gWhy;
	EXPECT_NE(gWhy.find(why), std::string::npos) << gWhy;
	fix();
	EXPECT_EQ(prepare(), 0) << gWhy;
	const unsigned reads = tegra_dfll_fake.pmic_reads, probes = tegra_dfll_fake.probes;
	EXPECT_EQ(prepare(), 0) << gWhy;
	EXPECT_EQ(tegra_dfll_fake.pmic_reads, reads);
	EXPECT_EQ(tegra_dfll_fake.probes, probes);
	EXPECT_EQ(tegra_dfll_fake.probes, 1u);
}

// -ENODEV for `broken`, and still -ENODEV once it is put right: nothing is tried again.
template <class Break, class Fix> void finalForTheBoot(Break broken, Fix fix, const char *why)
{
	tegra_dfll_fake_console();
	broken();
	EXPECT_EQ(prepare(), kENODEV) << gWhy;
	EXPECT_NE(gWhy.find(why), std::string::npos) << gWhy;
	fix();
	const unsigned reads = tegra_dfll_fake.pmic_reads, probes = tegra_dfll_fake.probes;
	EXPECT_EQ(prepare(), kENODEV) << gWhy;
	EXPECT_NE(gWhy.find(why), std::string::npos) << gWhy;
	EXPECT_EQ(tegra_dfll_fake.pmic_reads, reads);
	EXPECT_EQ(tegra_dfll_fake.probes, probes);
}
}  // namespace

TEST(TegraDfllPrepare, APmicThatLostTheBusIsTriedAgain)
{
	switch_ios::test::ExpectPassesInFreshProcess([] {
		triedAgain(
			kEAGAIN, "MAX77812 at I2C5 0x31 does not answer (TegraI2C -5)",
			[] {
				tegra_dfll_fake.pmic_answer = kLostArbitration;
			},
			[] {
				tegra_dfll_fake.pmic_answer = 0;
			});
	});
}

// One transfer per read: a lost arbitration is that read's failure, not a reason to read again (TegraI2C already
// repeats a transfer that lost arbitration on its own bus).
TEST(TegraDfllPrepare, ALostArbitrationFailsThatReadAlone)
{
	switch_ios::test::ExpectPassesInFreshProcess([] {
		tegra_dfll_fake_console();
		tegra_dfll_fake.pmic_answer = kLostArbitration;
		EXPECT_EQ(prepare(), kEAGAIN);
		EXPECT_EQ(tegra_dfll_fake.pmic_reads, 1u);
	});
}

TEST(TegraDfllPrepare, APmicThatDidNotAcknowledgeIsTriedAgain)
{
	switch_ios::test::ExpectPassesInFreshProcess([] {
		triedAgain(
			kEAGAIN, "MAX77812 at I2C5 0x31 does not answer (TegraI2C -4)",
			[] {
				tegra_dfll_fake.pmic_answer = kNack;
			},
			[] {
				tegra_dfll_fake.pmic_answer = 0;
			});
	});
}

TEST(TegraDfllPrepare, I2C5NotClockedYetIsTriedAgain)
{
	switch_ios::test::ExpectPassesInFreshProcess([] {
		triedAgain(
			kEAGAIN, "I2C5 clock is off",
			[] {
				reg(TEGRA_DFLL_FAKE_CAR, 0x014) = 0;
			},
			[] {
				reg(TEGRA_DFLL_FAKE_CAR, 0x014) = 1u << 15;
			});
	});
}

TEST(TegraDfllPrepare, ACpuClockInNeitherRunNorIdleIsTriedAgain)
{
	switch_ios::test::ExpectPassesInFreshProcess([] {
		triedAgain(
			kEBUSY, "in neither RUN nor IDLE state",
			[] {
				reg(TEGRA_DFLL_FAKE_CAR, 0x368) = (1u << 31) | (8u << 4);
			},
			[] {
				reg(TEGRA_DFLL_FAKE_CAR, 0x368) = (1u << 29) | (8u << 4);
			});
	});
}

TEST(TegraDfllPrepare, ADfllLeftRunningIsTriedAgainWithItsGateSetAgain)
{
	switch_ios::test::ExpectPassesInFreshProcess([] {
		triedAgain(
			kEBUSY, "the DFLL is already running",
			[] {
				reg(TEGRA_DFLL_FAKE_DFLL, 0x000) = 2;
			},
			[] {
				reg(TEGRA_DFLL_FAKE_DFLL, 0x000) = 0;
				tegra_dfll_fake_cl_dvfs_reset();  // an SC7 between the two
			});
		EXPECT_EQ(reg(TEGRA_DFLL_FAKE_CAR, 0x364) & (1u << 27), 1u << 27);  // CLK_OUT_ENB_W
		EXPECT_EQ(reg(TEGRA_DFLL_FAKE_CAR, 0x35c) & (1u << 27), 0u);        // RST_DEVICES_W
	});
}

TEST(TegraDfllPrepare, AMappingThatFailedIsMadeAgainAloneNextTime)
{
	switch_ios::test::ExpectPassesInFreshProcess([] {
		triedAgain(
			kENOMEM, "cannot map",
			[] {
				tegra_dfll_fake_refuse_map[TEGRA_DFLL_FAKE_DFLL] = true;
			},
			[] {
				tegra_dfll_fake_refuse_map[TEGRA_DFLL_FAKE_DFLL] = false;
			});
		EXPECT_EQ(tegra_dfll_fake.maps, 6u);
	});
}

TEST(TegraDfllPrepare, AProbeThatRanOutOfMemoryProbesAgain)
{
	switch_ios::test::ExpectPassesInFreshProcess([] {
		tegra_dfll_fake_console();
		tegra_dfll_fake.probe_answer = kENOMEM;
		EXPECT_EQ(prepare(), kENOMEM) << gWhy;
		tegra_dfll_fake.probe_answer = 0;
		EXPECT_EQ(prepare(), 0) << gWhy;
		EXPECT_EQ(tegra_dfll_fake.probes, 2u);
		EXPECT_EQ(tegra_dfll_fake.pmic_reads, 1u);
	});
}

TEST(TegraDfllPrepare, ARetryThatFailsAlikePrintsNothing)
{
	switch_ios::test::ExpectPassesInFreshProcess([] {
		tegra_dfll_fake_console();
		tegra_dfll_fake.pmic_answer = kLostArbitration;
		EXPECT_EQ(prepare(), kEAGAIN);
		EXPECT_EQ(tegra_dfll_fake.lines, 2u);
		EXPECT_EQ(prepare(), kEAGAIN);
		EXPECT_EQ(tegra_dfll_fake.lines, 2u);
		tegra_dfll_fake.pmic_answer = kNack;
		EXPECT_EQ(prepare(), kEAGAIN);
		EXPECT_EQ(tegra_dfll_fake.lines, 4u);
		tegra_dfll_fake.pmic_answer = 0;
		EXPECT_EQ(prepare(), 0);
	});
}

TEST(TegraDfllPrepare, NotAT210B01IsFinal)
{
	switch_ios::test::ExpectPassesInFreshProcess([] {
		finalForTheBoot(
			[] {
				reg(TEGRA_DFLL_FAKE_APBMISC, 0x004) = 0x12110;
			},
			[] {
				reg(TEGRA_DFLL_FAKE_APBMISC, 0x004) = 0x12120;
			},
			"not a T210B01 A01");
	});
}

TEST(TegraDfllPrepare, AnUnfusedSpeedoIsFinal)
{
	switch_ios::test::ExpectPassesInFreshProcess([] {
		finalForTheBoot(
			[] {
				tegra_dfll_fake.speedo_value = 0;
			},
			[] {
				tegra_dfll_fake.speedo_value = 1659;
			},
			"CPU speedo not fused");
	});
}

TEST(TegraDfllPrepare, NoCvbRowForThisChipIsFinalAndNeverProbes)
{
	switch_ios::test::ExpectPassesInFreshProcess([] {
		finalForTheBoot(
			[] {
				tegra_dfll_fake.process_id = 1;
			},
			[] {
				tegra_dfll_fake.process_id = 0;
			},
			"no T210B01 CVB row for CPU speedo id 0, process 1");
		EXPECT_EQ(tegra_dfll_fake.probes, 0u);
	});
}

TEST(TegraDfllPrepare, AProbeTheTablesRefuseIsFinal)
{
	switch_ios::test::ExpectPassesInFreshProcess([] {
		finalForTheBoot(
			[] {
				tegra_dfll_fake.probe_answer = kEINVAL;
			},
			[] {
				tegra_dfll_fake.probe_answer = 0;
			},
			"the L4T DFLL probe failed (-22)");
		EXPECT_EQ(tegra_dfll_fake.probes, 1u);
	});
}

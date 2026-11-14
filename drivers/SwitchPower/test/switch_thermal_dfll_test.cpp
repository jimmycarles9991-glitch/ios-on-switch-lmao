// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchThermalDfll.h, SwitchThermal's DFLL mode, run against a stand-in SwitchThermal and a DFLL whose answers the
// test sets: a failure is one line and fails that raise, and the next raise runs the same path at once (no wait, no PLL
// ladder in between); only a chip the DFLL does not fit (-ENODEV from prepare) ends the DFLL mode; a refused rate
// fails that request alone. A source guard holds SwitchThermal.cpp to the same rule for its own steps.
#include <gtest/gtest.h>

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <regex>
#include <string>
#include <vector>

#include "../../TegraDFLL/TegraDFLL.h"
#include "bazel/testing/source_text.h"

namespace {
constexpr int kLinuxEAGAIN = -11, kLinuxEIO = -5;
constexpr unsigned long kDfllRateHz = 1785000000UL;

std::vector<std::string> gLines;
int gPrepare, gEnable, gResume;
unsigned gPrepares, gEnables, gResumes;
unsigned long gEnabledHz;

bool has(const char *text)
{
    for (const auto &line : gLines)
        if (line.find(text) != std::string::npos)
            return true;
    return false;
}
}  // namespace

__attribute__((format(printf, 1, 2))) void IOLog(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    gLines.emplace_back(line);
}

extern "C" {
int tegra_dfll_cpu_prepare(char *why, size_t len)
{
    ++gPrepares;
    snprintf(why, len, "prepare %d", gPrepare);
    return gPrepare;
}

int tegra_dfll_cpu_enable(unsigned long hz, char *why, size_t len)
{
    ++gEnables;
    gEnabledHz = hz;
    snprintf(why, len, "enable %d", gEnable);
    return gEnable;
}

int tegra_dfll_cpu_resume(char *why, size_t len)
{
    ++gResumes;
    snprintf(why, len, "resume %d", gResume);
    return gResume;
}

void tegra_dfll_cpu_describe(unsigned long, char *buf, size_t len)
{
    snprintf(buf, len, "closed loop");
}
}

namespace switchclk {
struct Cpu {
    unsigned kHz;
};

Cpu readCpu(volatile uint8_t *)
{
    return {1785000};
}
}  // namespace switchclk

class SwitchThermal {
public:
    enum class CpuDown {
        Hold,
        Again,
        ForGood
    };

    volatile uint8_t *car = nullptr;
    bool dfllMode = true, dfllPrepared = false, governed = true, dfllResume = false;
    unsigned long dfllRefusedHz = 0;
    int dfllRefusedErr = 0;
    bool perfRaised = false;
    unsigned perfRaises = 0, dfllFloor = 0, dfllCap = 0;
    bool governorStarts = true;
    unsigned restores = 0, ladderRaises = 0;
    CpuDown lastDown = CpuDown::Hold;

    void resumeDfll();
    void dfllRefused(unsigned long hz, int err);
    bool raiseDfll(int soc);

    // raiseCpu's DFLL half as SwitchThermal.cpp runs it, then the PLL ladder as one step.
    bool raiseCpu(int soc)
    {
        if (dfllMode) {
            if (raiseDfll(soc))
                return true;
            if (dfllMode)
                return false;
        }
        perfRaised = true;
        ++ladderRaises;
        return true;
    }

    void restoreCpu(const char *, CpuDown down)
    {
        if (!perfRaised)
            return;
        perfRaised = false;
        lastDown = down;
        ++restores;
    }

    bool startGovernor(char *why, size_t len)
    {
        snprintf(why, len, "%s", governorStarts ? "CPU governor: 9 points" : "no work loop source");
        return governorStarts;
    }

    void updateDfllThermal(int)
    {}

    void publish(const char *, int64_t)
    {}

    bool onDfll() const
    {
        return perfRaised && dfllMode;
    }
};

#include "../SwitchThermalDfll.h"

namespace {
class ThermalDfll : public ::testing::Test {
protected:
    SwitchThermal thermal;

    void SetUp() override
    {
        gLines.clear();
        gPrepare = gEnable = gResume = 0;
        gPrepares = gEnables = gResumes = 0;
    }
};

// The DFLL closes its loop at the top row and is asked nothing else before the governor starts: clk-dfll.c's Vmin
// calibration lands each rate from either direction (test/tegra_dfll_landing_test.cpp), so no rate is re-asked.
TEST_F(ThermalDfll, TheLoopClosesAtTheTopRow)
{
    gEnabledHz = 0;
    EXPECT_TRUE(thermal.raiseCpu(40000));
    EXPECT_EQ(gEnabledHz, kDfllRateHz);
    EXPECT_EQ(gEnables, 1u);
    EXPECT_TRUE(has("SwitchPerf: CPU on the DFLL at 1785 MHz, on demand from here"));
}

TEST_F(ThermalDfll, AFailedPrepareFailsThatRaiseAndTheNextRaiseRunsTheSamePath)
{
    gPrepare = kLinuxEAGAIN;
    EXPECT_FALSE(thermal.raiseCpu(40000));
    EXPECT_FALSE(thermal.perfRaised);
    EXPECT_EQ(thermal.ladderRaises, 0u);
    EXPECT_TRUE(thermal.dfllMode);
    ASSERT_EQ(gLines.size(), 1u);
    EXPECT_EQ(gLines[0], "SwitchPerf: DFLL not raised (-11: prepare -11); the CPU stays at the inherited clock until "
                         "the next raise\n");
    gPrepare = 0;
    EXPECT_TRUE(thermal.raiseCpu(40000));
    EXPECT_TRUE(thermal.onDfll());
    EXPECT_TRUE(thermal.governed);
    EXPECT_EQ(gPrepares, 2u);
    EXPECT_EQ(thermal.ladderRaises, 0u);
}

TEST_F(ThermalDfll, AFailedEnableOrGovernorStartFailsThatRaiseOnly)
{
    gEnable = kLinuxEIO;
    EXPECT_FALSE(thermal.raiseCpu(40000));
    EXPECT_FALSE(thermal.perfRaised);
    gEnable = 0;
    thermal.governorStarts = false;
    EXPECT_FALSE(thermal.raiseCpu(40000));
    EXPECT_FALSE(thermal.perfRaised);
    EXPECT_EQ(thermal.restores, 1u);
    EXPECT_TRUE(thermal.lastDown == SwitchThermal::CpuDown::Again);
    EXPECT_TRUE(has("SwitchPerf: CPU governor not started (no work loop source); the CPU stays at the inherited clock "
                    "until the next raise"));
    thermal.governorStarts = true;
    EXPECT_TRUE(thermal.raiseCpu(40000));
    EXPECT_TRUE(thermal.onDfll());
    EXPECT_TRUE(thermal.governed);
    EXPECT_EQ(gEnables, 3u);
    EXPECT_EQ(gPrepares, 1u);
    EXPECT_EQ(thermal.ladderRaises, 0u);
}

TEST_F(ThermalDfll, EveryRaiseAsksTheDfllAgainHoweverManyFailedBefore)
{
    gPrepare = kLinuxEAGAIN;
    for (unsigned i = 0; i != 20; ++i)
        EXPECT_FALSE(thermal.raiseCpu(40000));
    EXPECT_EQ(gPrepares, 20u);
    EXPECT_EQ(gLines.size(), 20u);
    EXPECT_EQ(thermal.ladderRaises, 0u);
    gPrepare = 0;
    EXPECT_TRUE(thermal.raiseCpu(40000));
    EXPECT_TRUE(thermal.onDfll());
}

TEST_F(ThermalDfll, AChipTheDfllDoesNotFitRunsThePllLadderForTheBoot)
{
    gPrepare = -ENODEV;
    EXPECT_TRUE(thermal.raiseCpu(40000));
    EXPECT_EQ(thermal.ladderRaises, 1u);
    EXPECT_FALSE(thermal.dfllMode);
    EXPECT_TRUE(has("SwitchPerf: the DFLL does not fit this chip (prepare -19); PLL mode for this boot"));
    gPrepare = 0;
    thermal.perfRaised = false;
    EXPECT_TRUE(thermal.raiseCpu(40000));
    EXPECT_EQ(gPrepares, 1u);
    thermal.resumeDfll();
    EXPECT_EQ(gResumes, 0u);
}

TEST_F(ThermalDfll, RegistersSc7LostAreRestoredByTheNextRaiseBeforeItEnables)
{
    thermal.dfllPrepared = true;
    gResume = -EBUSY;
    thermal.resumeDfll();
    EXPECT_TRUE(thermal.dfllResume);
    EXPECT_TRUE(thermal.dfllMode);
    EXPECT_TRUE(has("SwitchPerf: DFLL registers not restored after sleep (-16: resume -16); the next raise restores "
                    "them"));
    EXPECT_FALSE(thermal.raiseCpu(40000));
    EXPECT_EQ(gResumes, 2u);
    EXPECT_EQ(gEnables, 0u);
    gResume = 0;
    EXPECT_TRUE(thermal.raiseCpu(40000));
    EXPECT_EQ(gResumes, 3u);
    EXPECT_FALSE(thermal.dfllResume);
    EXPECT_EQ(gEnables, 1u);
    EXPECT_TRUE(thermal.onDfll());
}

TEST_F(ThermalDfll, ARefusedRateFailsThatRequestAloneWithOneLine)
{
    ASSERT_TRUE(thermal.raiseCpu(40000));
    gLines.clear();
    for (unsigned i = 0; i != 5; ++i)
        thermal.dfllRefused(1785000000UL, -22);
    thermal.dfllRefused(1683000000UL, -22);
    EXPECT_TRUE(thermal.onDfll());
    EXPECT_TRUE(thermal.governed);
    EXPECT_EQ(thermal.restores, 0u);
    const std::vector<std::string> want = {
        "SwitchPerf: the DFLL refused 1785 MHz (-22); it keeps the rate it runs\n",
        "SwitchPerf: the DFLL refused 1683 MHz (-22); it keeps the rate it runs\n",
    };
    EXPECT_EQ(gLines, want);
}

// SwitchThermal.cpp's own steps: nothing settles the CPU for the boot but shutting down, a CVB that rules every rung
// out, and the perfSteps test stop; no DFLL failure leaves the DFLL mode or waits.
TEST(ThermalDfllSource, SwitchThermalFailsOneStepAndNeverLatches)
{
    const std::string source = switch_ios::test::SourceText("../SwitchThermal.cpp");
    const std::regex settled(R"(perfSettled = true;)");
    EXPECT_EQ(std::distance(std::sregex_iterator(source.begin(), source.end(), settled), std::sregex_iterator()), 3);
    const std::string down = switch_ios::test::Body(source, "void SwitchThermal::restoreCpu(");
    EXPECT_NE(down.find("if (down == CpuDown::ForGood)\n        perfSettled = true;"), std::string::npos);
    EXPECT_EQ(down.find("dfllMode ="), std::string::npos);
    const std::string raise = switch_ios::test::Body(source, "bool SwitchThermal::raiseCpu(");
    EXPECT_NE(raise.find("if (!perfTop) {\n        perfSettled = true;"), std::string::npos);
    EXPECT_NE(raise.find("if (dfllMode)\n            return false;"), std::string::npos);
    EXPECT_EQ(raise.find("dfllMode = "), std::string::npos);
    const std::string fired = switch_ios::test::Body(source, "void SwitchThermal::governorFired(") +
                              switch_ios::test::Body(source, "void SwitchThermal::applyGovernor(");
    EXPECT_NE(fired.find("dfllRefused(hz, err)"), std::string::npos);
    EXPECT_EQ(fired.find("setTimeoutMS"), std::string::npos);
    EXPECT_NE(switch_ios::test::Body(source, "void SwitchThermal::resume(").find("resumeDfll()"), std::string::npos);
    EXPECT_NE(switch_ios::test::Body(source, "void SwitchThermal::failSafe(").find("CpuDown::Again"),
              std::string::npos);
    EXPECT_NE(switch_ios::test::Body(source, "bool SwitchThermal::preparePerf(").find("perfSettled = fact;"),
              std::string::npos);
    EXPECT_EQ(source.find("PLL mode for this boot"), std::string::npos);
}

// Nothing a governor signal, ALERT or SOC_THERM waits behind on the work loop sleeps: ALERT's conversion is a timer
// shot (alertReleased), and the DFLL landing sleeps on its writer's thread between steps it hands the loop one by one.
TEST(ThermalDfllSource, NoSleepWhereTheGovernorOrAlertWait)
{
    const std::string source = switch_ios::test::SourceText("../SwitchThermal.cpp") +
                               switch_ios::test::SourceText("../SwitchThermalDfllLanding.h");
    for (const char *signature :
         {"void SwitchThermal::governorFired(", "void SwitchThermal::applyGovernor(", "void SwitchThermal::alertFired(",
          "void SwitchThermal::alertReleased(", "void SwitchThermal::dfllLandingStep("}) {
        const std::string body = switch_ios::test::Body(source, signature);
        EXPECT_FALSE(body.empty()) << signature;
        EXPECT_EQ(body.find("IOSleep"), std::string::npos) << signature;
    }
    const std::string sysctl = switch_ios::test::Body(source, "int SwitchThermal::dfllLandingSysctl(");
    ASSERT_FALSE(sysctl.empty());
    EXPECT_EQ(sysctl.find("runAction"), std::string::npos) << "the whole landing on the loop sleeps under its gate";
}
}  // namespace

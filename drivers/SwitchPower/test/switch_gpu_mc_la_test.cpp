// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include "../SwitchGpuMcLa.h"

namespace {
struct MockMcRegs {
    std::array<uint32_t, 1024> values{};
    unsigned writes = 0;

    struct Word {
        MockMcRegs *regs;
        unsigned index;

        operator uint32_t() const
        {
            return regs->values[index];
        }

        void operator=(uint32_t value)
        {
            regs->values[index] = value;
            ++regs->writes;
        }
    };

    Word operator[](unsigned index)
    {
        return {this, index};
    }
};

std::string readText(const char *path)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

TEST(SwitchGpuMcLa, AllowanceMatchesL4tTegra21xScalingAcrossRates)
{
    const auto at1600 = switch_mc::gpuMcPlan(1600000);
    EXPECT_EQ(at1600.gpusrd, 25u);
    EXPECT_EQ(at1600.gpuswr, 128u);
    EXPECT_EQ(at1600.ptsaMin, 0x3eu);
    EXPECT_EQ(at1600.ptsaMax, 0u);

    const auto at1331 = switch_mc::gpuMcPlan(1331200);
    EXPECT_EQ(at1331.gpusrd, 25u);
    EXPECT_EQ(at1331.gpuswr, 128u);

    const auto at800 = switch_mc::gpuMcPlan(800000);
    EXPECT_EQ(at800.gpusrd, 25u);
    EXPECT_EQ(at800.gpuswr, 128u);

    const auto at665 = switch_mc::gpuMcPlan(665600);
    EXPECT_EQ(at665.gpusrd, 30u);
    EXPECT_EQ(at665.gpuswr, 153u);

    const auto at408 = switch_mc::gpuMcPlan(408000);
    EXPECT_EQ(at408.gpusrd, 49u);
    EXPECT_EQ(at408.gpuswr, 250u);

    const auto at204 = switch_mc::gpuMcPlan(204000);
    EXPECT_EQ(at204.gpusrd, 98u);
    EXPECT_EQ(at204.gpuswr, 255u);
}

TEST(SwitchGpuMcLa, ProgramWritesGpu0AndGkPtsaWithoutTouchingOtherRegs)
{
    MockMcRegs regs;
    regs.values[switch_mc::kMcLatencyAllowanceGpu0 / 4] = 0xaa00bb00u;
    regs.values[switch_mc::kMcGkPtsaRate / 4] = 0x123u;
    regs.values[switch_mc::kMcGkPtsaMin / 4] = 0x11u;
    regs.values[switch_mc::kMcGkPtsaMax / 4] = 0x22u;
    regs.values[switch_mc::kMcEmemArbOutstandingReq / 4] = 0x80000040u;
    regs.values[switch_mc::kMcPtsaGrantDecrement / 4] = 0x1040u;

    switch_mc::programGpuMc(regs, 1600000);
    EXPECT_EQ(regs.writes, 3u);
    EXPECT_EQ(regs.values[switch_mc::kMcLatencyAllowanceGpu0 / 4], 0xaa80bb19u);
    EXPECT_EQ(regs.values[switch_mc::kMcGkPtsaMin / 4], 0x3eu);
    EXPECT_EQ(regs.values[switch_mc::kMcGkPtsaMax / 4], 0u);
    EXPECT_EQ(regs.values[switch_mc::kMcGkPtsaRate / 4], 0x123u);
    EXPECT_EQ(regs.values[switch_mc::kMcEmemArbOutstandingReq / 4], 0x80000040u);
    EXPECT_EQ(regs.values[switch_mc::kMcPtsaGrantDecrement / 4], 0x1040u);

    switch_mc::programGpuMc(regs, 204000);
    EXPECT_EQ(regs.values[switch_mc::kMcLatencyAllowanceGpu0 / 4], 0xaaffbb62u);
}

TEST(SwitchGpuMcLa, CaptureReadsAllAuditRegisters)
{
    MockMcRegs regs;
    regs.values[switch_mc::kMcLatencyAllowanceGpu0 / 4] = 0x00800019u;
    regs.values[switch_mc::kMcLatencyAllowanceGpu20 / 4] = 0x00800019u;
    regs.values[switch_mc::kMcGkPtsaRate / 4] = 0x10u;
    regs.values[switch_mc::kMcGkPtsaMin / 4] = 0x3eu;
    regs.values[switch_mc::kMcGkPtsaMax / 4] = 0x00u;
    regs.values[switch_mc::kMcGk2PtsaRate / 4] = 0x20u;
    regs.values[switch_mc::kMcGk2PtsaMin / 4] = 0x3eu;
    regs.values[switch_mc::kMcGk2PtsaMax / 4] = 0x00u;
    regs.values[switch_mc::kMcEmemArbOutstandingReq / 4] = 0x800000c0u;
    regs.values[switch_mc::kMcEmemArbOutstandingReqRing3 / 4] = 0x80000040u;
    regs.values[switch_mc::kMcEmemArbOutstandingReqNiso / 4] = 0x80000080u;
    regs.values[switch_mc::kMcEmemArbMisc0 / 4] = 0x70000001u;
    regs.values[switch_mc::kMcEmemArbRing3Throttle / 4] = 0x001f001fu;
    regs.values[switch_mc::kMcEmemArbNisoThrottle / 4] = 0x00000000u;
    regs.values[switch_mc::kMcPtsaGrantDecrement / 4] = 0x1555u;

    const auto snap = switch_mc::captureGpuMc(regs);
    EXPECT_EQ(snap.gpu0, 0x00800019u);
    EXPECT_EQ(snap.gpu2, 0x00800019u);
    EXPECT_EQ(snap.gkRate, 0x10u);
    EXPECT_EQ(snap.gkMin, 0x3eu);
    EXPECT_EQ(snap.gkMax, 0x00u);
    EXPECT_EQ(snap.gk2Rate, 0x20u);
    EXPECT_EQ(snap.gk2Min, 0x3eu);
    EXPECT_EQ(snap.gk2Max, 0x00u);
    EXPECT_EQ(snap.outstandingReq, 0x800000c0u);
    EXPECT_EQ(snap.outstandingRing3, 0x80000040u);
    EXPECT_EQ(snap.outstandingNiso, 0x80000080u);
    EXPECT_EQ(snap.arbMisc0, 0x70000001u);
    EXPECT_EQ(snap.ring3Throttle, 0x001f001fu);
    EXPECT_EQ(snap.nisoThrottle, 0x00000000u);
    EXPECT_EQ(snap.grantDec, 0x1555u);
}

TEST(SwitchGpuMcLa, SwitchEmcWiresGpuMcSetupAndTransitions)
{
    const std::string emc = readText(SWITCH_EMC_CPP);
    EXPECT_NE(emc.find("#include \"SwitchGpuMcLa.h\""), std::string::npos);
    EXPECT_NE(emc.find("programGpuMc(khz[cur], true);"), std::string::npos);
    EXPECT_NE(emc.find("switch_mc::programGpuMc(mcRegs, dramKhz);"), std::string::npos);

    const std::string clk = readText(SWITCH_EMC_CLOCK_H);
    EXPECT_NE(clk.find("programGpuMc(khz[cur]);"), std::string::npos);
    EXPECT_NE(clk.find("self->programGpuMc(self->khz[self->cur]);"), std::string::npos);

    const std::string fixed = readText(SWITCH_EMC_FIXED_H);
    EXPECT_NE(fixed.find("programGpuMc(now);"), std::string::npos);
}
}  // namespace

// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchDcHost1xClassProbe.h: debug kernels only, its setup proven (the scratch word's readback, a control stream)
// before the class 0x70 stream, which is built from SETCLASS, INCR and IMM (the opcodes T210's host1x has) and sent
// through host1x's job API, and its words what the probe means them to be.
#include <gtest/gtest.h>

#include <string>

#include "../../TegraHost1x/TegraHost1x.h"
#include "bazel/testing/source_text.h"

namespace {

using switch_ios::test::Between;
using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::SourceText;

TEST(DcHost1xClassProbe, ItIsDebugOnlyAndUsesTheJobApi)
{
    const std::string probe = SourceText("../SwitchDcHost1xClassProbe.h");
    const size_t gate = probe.find("#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)");
    ASSERT_NE(gate, std::string::npos);
    EXPECT_LT(gate, probe.find("void dcClassProbe()"));
    EXPECT_EQ(Between(probe, "#else\n", "#endif"),
              "static void dcClassRegister()\n{}\n\nstatic bool dcChannelProbed()\n{\n    return false;\n}\n");
    EXPECT_NE(probe.find("switch_host1x_job_copy(kDcClassChannel, words, n, dcClassSyncpts[2], s.channelAt)"),
              std::string::npos);
    EXPECT_NE(probe.find("kDcClassChannel = TEGRA_HOST1X_CHANNEL_DISPLAY"), std::string::npos)
        << "a torn-down NVJPG channel drops NVJPG's queued jobs";
    const std::string run = Between(probe, "void dcClassRun()", "\n}\n");
    EXPECT_LT(run.find("dcClassScratchHolds(before, marker)"), run.find("dcClassSubmit(nullptr, 0, control)"));
    EXPECT_LT(run.find("dcClassSubmit(nullptr, 0, control)"), run.find("dcClassSubmit(body,"));
    EXPECT_LT(run.find("reason=control"), run.find("dcClassSubmit(body,"));
    EXPECT_EQ(probe.find("tegra_host1x_submit"), std::string::npos);
    EXPECT_EQ(probe.find("_nonincr("), std::string::npos);
    const std::string display = SourceText("../NativeLegacyDisplay.cpp");
    EXPECT_NE(display.find("#include \"NativeColumnScanDisplay.h\"\n#include \"SwitchDcHost1xClassProbe.h\""),
              std::string::npos);
    EXPECT_NE(display.find("            columnScanRegister();\n            dcClassRegister();"), std::string::npos);
}

TEST(DcHost1xClassProbe, ItRunsOnTheProbesSharedSyncpointsAndLetsThemGo)
{
    const std::string probe = SourceText("../SwitchDcHost1xClassProbe.h");
    EXPECT_EQ(CodeText(probe).count("tegra_host1x_syncpt_alloc"), 0u);
    const CodeText start = Block(probe, "void dcClassProbe()");
    EXPECT_TRUE(start.before("tegra_host1x_start()", "switch_host1x_probe_syncpts_claim(dcClassSyncpts)"));
    EXPECT_TRUE(start.before("switch_host1x_probe_syncpts_claim(dcClassSyncpts)", "dcClassRun();"));
    EXPECT_TRUE(start.has("dcClassRun(); switch_host1x_probe_syncpts_release();"));
}

// The probe shares DC A's host1x channel with windows B and C's stream: it never runs under one queued, and none is
// armed while either probe holds the channel, so a stop of the channel under a stream is the kernel's own failure.
TEST(DcHost1xClassProbe, ItNeverSharesTheChannelWithAStream)
{
    const std::string probe = SourceText("../SwitchDcHost1xClassProbe.h");
    const CodeText start = Block(probe, "void dcClassProbe()");
    EXPECT_TRUE(start.before("if (dcClassStreamOut()) {", "switch_host1x_probe_syncpts_claim(dcClassSyncpts)"));
    EXPECT_TRUE(Block(probe, "bool dcClassStreamOut()")
                    .has("q.out = !q.display->mediaFlipIdle() || "
                         "__atomic_load_n(&mediaHostGeneration, __ATOMIC_ACQUIRE) != 0u;"));
    EXPECT_TRUE(
        Block(probe, "static bool dcChannelProbed()").has("return __atomic_load_n(&dcClassBusy, __ATOMIC_ACQUIRE);"));
    EXPECT_TRUE(Block(SourceText("../SwitchMediaCompletionFlipDisplay.h"),
                      "bool mediaFlipMay(const PlaneSet &p, PlaneSet &assigned)")
                    .has("host1xFlipBusy() || dcChannelProbed())"));
}

TEST(DcHost1xClassProbe, OnlyTheProbesEntitlementRunsIt)
{
    const CodeText write = Block(SourceText("../SwitchDcHost1xClassProbe.h"),
                                 "static int dcClassSysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)");
    EXPECT_TRUE(write.startsWith("if (req->newptr) { if (!switch_task_entitled(current_task(), "
                                 "SWITCH_GPU_PROBES_ENTITLEMENT)) return EPERM;"));
}

// DC A's word offsets in class 0x70 are its register indices (head 0); both fit the opcodes' 12-bit offset field.
TEST(DcHost1xClassProbe, TheStreamsWords)
{
    EXPECT_EQ(tegra_host1x_op_setclass(0x70, 0, 0), 0x70u << 6);
    EXPECT_EQ(tegra_host1x_op_incr(0x325, 1), 0x13250001u);
    EXPECT_EQ(tegra_host1x_op_imm(0x000, (TEGRA_HOST1X_COND_IMMEDIATE << 8) | 170), 0x400000aau);
    uint32_t wait[SWITCH_HOST1X_WAIT_WORDS];
    EXPECT_EQ(tegra_host1x_op_wait_syncpt(wait, 171, 5), 3u);
    EXPECT_EQ(wait[0], tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_HOST1X, 0x4e, 5));
    EXPECT_EQ(wait[1], 5u);
    EXPECT_EQ(wait[2], 171u);
}

}  // namespace

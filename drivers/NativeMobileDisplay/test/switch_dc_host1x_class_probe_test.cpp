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
using switch_ios::test::SourceText;

TEST(DcHost1xClassProbe, ItIsDebugOnlyAndUsesTheJobApi)
{
    const std::string probe = SourceText("../SwitchDcHost1xClassProbe.h");
    const size_t gate = probe.find("#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)");
    ASSERT_NE(gate, std::string::npos);
    EXPECT_LT(gate, probe.find("void dcClassProbe()"));
    EXPECT_EQ(Between(probe, "#else\n", "#endif"), "static void dcClassRegister()\n{}\n");
    EXPECT_NE(probe.find("switch_host1x_job_copy(kDcClassChannel, words, n, dcClassSyncpts[2], s.channelAt)"),
              std::string::npos);
    const std::string run = Between(probe, "void dcClassProbe()", "\n}\n");
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

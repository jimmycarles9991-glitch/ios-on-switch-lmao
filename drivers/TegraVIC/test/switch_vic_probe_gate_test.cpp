// SPDX-License-Identifier: BSD-2-Clause-Patent
// Selector 6, the kernel experiments of TegraVIC and the display (gate, landscape run, window and mlock probes), is in
// debug kernels alone and answers only a client holding SWITCH_GPU_PROBES_ENTITLEMENT (HW Test's two apps), before it
// reads the request.
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

namespace {

using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::Configured;
using switch_ios::test::SourceText;

const std::string &client()
{
    static const std::string source = SourceText("../TegraVICUserClient.cpp");
    return source;
}

TEST(VICProbeGate, ReleaseKernelsRefuseSelectorSix)
{
    const CodeText release(Configured(client(), "TEGRA_VIC_STATS", false));
    EXPECT_EQ(release.count("kernelTest("), 0u);
    EXPECT_TRUE(release.has("case 6: // kernel tests (Apple's: release kernels refuse) return kIOReturnNotPermitted;"));
}

TEST(VICProbeGate, EveryExperimentAsksForTheProbesEntitlementFirst)
{
    const std::string debug = Configured(client(), "TEGRA_VIC_STATS", true);
    EXPECT_TRUE(
        CodeText(debug).has("case 6: // kernel tests (Apple's: release kernels refuse) return kernelTest(conn, a);"));
    EXPECT_TRUE(Block(debug, "static IOReturn kernelTest(const Conn *conn, const IOExternalMethodArguments *a)")
                    .startsWith("if (!switch_task_entitled(conn->task, SWITCH_GPU_PROBES_ENTITLEMENT)) return "
                                "kIOReturnNotPrivileged;"));
    EXPECT_EQ(CodeText(debug).count("kernelTest("), 2u);
}

TEST(VICProbeGate, OnlyABooleanTrueGrantsAnEntitlement)
{
    const CodeText kit(SourceText("../../../kits/SwitchEntitlement/SwitchEntitlement.h"));
    EXPECT_TRUE(kit.has("OSObject *value = IOUserClient::copyClientEntitlement(task, entitlement);"
                        " const bool entitled = value == kOSBooleanTrue; OSSafeReleaseNULL(value); return entitled;"));
}

}  // namespace

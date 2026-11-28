// SPDX-License-Identifier: BSD-2-Clause-Patent
// Review-dma2 D5: on a debug kernel, selector 6's probe table (the MC fault probe among them, which arms the MC error
// filter) runs only for a process holding the probes entitlement, which HW Test's apps hold.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;

TEST(VicProbeGateSource, TheProbeTableRunsOnlyForTheProbesEntitlement)
{
    const std::string client = SourceText("../TegraVICUserClient.cpp");
    const std::string dispatch =
        Body(client, "static IOReturn kernelTest(const Conn *conn, const IOExternalMethodArguments *a)\n");
    const size_t gate =
        dispatch.find("if (!switch_vic_probes_entitled(conn->task))\n        return kIOReturnNotPrivileged;");
    ASSERT_NE(gate, std::string::npos);
    EXPECT_LT(gate, dispatch.find("return switch_vic_kernel_test_call(a);"));
    EXPECT_THAT(SourceText("../TegraVIC.h"),
                HasSubstr("#define SWITCH_VIC_PROBES_ENTITLEMENT \"com.brunostjohn.switch.gpu-probes\""));
    for (const char *app :
         {"../../../apps/hwtest/SwitchHWTest.entitlements", "../../../apps/hwtest/SwitchHWTest27.entitlements"})
        EXPECT_THAT(SourceText(app), HasSubstr("<key>com.brunostjohn.switch.gpu-probes</key>")) << app;
}

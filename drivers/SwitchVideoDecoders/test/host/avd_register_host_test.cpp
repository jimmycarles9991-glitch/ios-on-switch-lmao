// avd_register_host.c: where AVDRegister puts hardware entries in this Mac's VideoToolbox registry.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "run_host.h"

extern "C" int avd_register_host_main(void);

TEST(SwitchVideoDecoders, AvdRegistersHardwareEntriesWhereSessionsReachNvdec)
{
    const HostRun run = RunHost(
        [](int, char **) {
            return avd_register_host_main();
        },
        {"avd_register_host"});
    EXPECT_EQ(run.code, 0) << run.out;
    EXPECT_THAT(run.out, testing::HasSubstr("PASS avd-register"));
}

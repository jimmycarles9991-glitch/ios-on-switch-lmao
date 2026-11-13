// mpeg4_host.c: MPEG4NVDEC against FFmpeg's own trace of the two clips.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "run_host.h"

extern "C" int mpeg4_host_main(int argc, char **argv);

TEST(SwitchVideoDecoders, Mpeg4AgainstFFmpeg)
{
    const HostRun run = RunHost(mpeg4_host_main, {"mpeg4_host"});
    EXPECT_EQ(run.code, 0) << run.out;
    EXPECT_THAT(run.out, testing::HasSubstr("PASS mpeg4: clip A 18 VOPs, clip B 32 VOPs"));
}

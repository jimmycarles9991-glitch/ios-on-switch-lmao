// h264_host.c --clip: ../VDECClip.h's clip through H264NVDEC, every submit through the kernel's checker.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "run_host.h"

extern "C" int h264_host_main(int argc, char **argv);

TEST(SwitchVideoDecoders, H264ClipThroughTheKernelChecker)
{
    const HostRun run = RunHost(h264_host_main, {"h264_host", "--clip"});
    EXPECT_EQ(run.code, 0) << run.out;
    EXPECT_THAT(run.out, testing::HasSubstr("PASS h264 clip: 30 access units, 30 pictures"));
}

TEST(SwitchVideoDecoders, H264MbaffClipThroughTheKernelChecker)
{
    const HostRun run = RunHost(h264_host_main, {"h264_host", "--mbaff"});
    EXPECT_EQ(run.code, 0) << run.out;
    EXPECT_THAT(run.out, testing::HasSubstr("PASS h264 mbaff clip: 30 access units, 30 pictures"));
}

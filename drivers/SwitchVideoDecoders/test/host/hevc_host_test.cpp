// hevc_host.c: HEVCSyntax/HEVCNVDEC against FFmpeg's own trace.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "run_host.h"

extern "C" int hevc_host_main(int argc, char **argv);

TEST(SwitchVideoDecoders, HevcAgainstFfmpeg)
{
    const HostRun run = RunHost(hevc_host_main, {"hevc_host"});
    EXPECT_EQ(run.code, 0) << run.out;
    EXPECT_THAT(run.out, testing::HasSubstr("syntax: 65 slice segments match FFmpeg"));
    EXPECT_THAT(run.out,
                testing::HasSubstr("decode order: clip A, 20 pictures each out before its decode call returned"));
    EXPECT_THAT(run.out, testing::HasSubstr("PASS hevc"));
}

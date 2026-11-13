// mpeg2_host.c: MPEG2NVDEC against FFmpeg's trace_headers of the three clips.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "run_host.h"

extern "C" int mpeg2_host_main(int argc, char **argv);

TEST(SwitchVideoDecoders, Mpeg2AgainstFFmpeg)
{
    const HostRun run = RunHost(mpeg2_host_main, {"mpeg2_host"});
    EXPECT_EQ(run.code, 0) << run.out;
    EXPECT_THAT(run.out, testing::HasSubstr("PASS mpeg2: clip A 18, clip B 12 (interlaced), clip C 15 (MPEG-1)"));
}

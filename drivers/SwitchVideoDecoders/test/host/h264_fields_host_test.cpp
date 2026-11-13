// h264_fields_host.c: field-coded streams through H264NVDEC, each P picture's reference list as NVDEC builds it from
// the dpb[] table against FFmpeg's, every submit through the kernel's checker.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "run_host.h"

extern "C" int h264_fields_host_main(int argc, char **argv);

TEST(SwitchVideoDecoders, H264FieldsAgainstFFmpegsReferenceLists)
{
    const HostRun run = RunHost(h264_fields_host_main, {"h264_fields_host"});
    EXPECT_EQ(run.code, 0) << run.out;
    EXPECT_THAT(run.out, testing::HasSubstr("PASS h264 fields: 7 streams, 63 pictures, 37 frames, 0 mismatches"));
}

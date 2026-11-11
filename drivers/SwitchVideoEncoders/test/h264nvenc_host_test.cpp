// h264nvenc_host.c: H264NVENC.c's setups carry the coded size, and the kernel's checker refuses the display size.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <string>

extern "C" int h264nvenc_host_main(void);

TEST(SwitchVideoEncoders, H264NvencSurfacesAreTheCodedSize)
{
    testing::internal::CaptureStdout();
    const int code = h264nvenc_host_main();
    const std::string out = testing::internal::GetCapturedStdout();
    std::fputs(out.c_str(), stdout);
    EXPECT_EQ(code, 0) << out;
    EXPECT_THAT(out, testing::HasSubstr("PASS h264nvenc"));
}

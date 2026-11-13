// nvencdirect_host.c: a native NVDEC picture as NVENC's input in place, through the kernel's checker.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <string>

extern "C" int nvencdirect_host_main(void);

TEST(SwitchVideoEncoders, NvencReadsNativeDecoderPicturesInPlace)
{
    testing::internal::CaptureStdout();
    const int code = nvencdirect_host_main();
    const std::string out = testing::internal::GetCapturedStdout();
    std::fputs(out.c_str(), stdout);
    EXPECT_EQ(code, 0) << out;
    EXPECT_THAT(out, testing::HasSubstr("PASS nvencdirect"));
}

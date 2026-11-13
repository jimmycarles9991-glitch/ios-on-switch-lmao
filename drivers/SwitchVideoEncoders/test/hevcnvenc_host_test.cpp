// hevcnvenc_host.c: HEVCNVENC.c's IDR and P setups against NVIDIA's T210 driver's, and the kernel's H.265 checker.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <string>

extern "C" int hevcnvenc_host_main(void);

TEST(SwitchVideoEncoders, HevcNvencSetupMatchesTheT210Driver)
{
    testing::internal::CaptureStdout();
    const int code = hevcnvenc_host_main();
    const std::string out = testing::internal::GetCapturedStdout();
    std::fputs(out.c_str(), stdout);
    EXPECT_EQ(code, 0) << out;
    EXPECT_THAT(out, testing::HasSubstr("PASS hevcnvenc"));
}

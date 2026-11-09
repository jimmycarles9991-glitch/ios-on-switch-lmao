// hevcenc_host.c: HEVCEncSyntax.c's parameter sets and slice headers, parsed back and taken by CoreMedia.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <string>

extern "C" int hevcenc_host_main(void);

TEST(SwitchVideoEncoders, HevcParameterSetsParseBack)
{
    testing::internal::CaptureStdout();
    const int code = hevcenc_host_main();
    const std::string out = testing::internal::GetCapturedStdout();
    std::fputs(out.c_str(), stdout);
    EXPECT_EQ(code, 0) << out;
    EXPECT_THAT(out, testing::HasSubstr("PASS hevcenc"));
}

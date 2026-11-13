// nvenclatch_host.c: what the kernel writes in the colocated and reference methods a job leaves out.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <string>

extern "C" int nvenclatch_host_main(void);

TEST(SwitchVideoEncoders, NvencLatchedMethodsNeverKeepAnEarlierAddress)
{
    testing::internal::CaptureStdout();
    const int code = nvenclatch_host_main();
    const std::string out = testing::internal::GetCapturedStdout();
    std::fputs(out.c_str(), stdout);
    EXPECT_EQ(code, 0) << out;
    EXPECT_THAT(out, testing::HasSubstr("PASS nvenclatch"));
}

// nvjpg_host.c's checks (the libjpeg-turbo matrix and targets; BUILD.bazel builds it with
// libjpeg-turbo): it prints "PASS nvjpg (...)" first and returns 0, or prints each failure ("FAIL ...") and returns 1.
#include <gtest/gtest.h>

#include <string>

extern "C" int nvjpg_host_main(int argc, char **argv);

TEST(TegraNVJPG, ParserAndTargets)
{
    char name[] = "nvjpg_host";
    char *argv[] = {name, nullptr};
    testing::internal::CaptureStdout();
    const int code = nvjpg_host_main(1, argv);
    const std::string out = testing::internal::GetCapturedStdout();
    EXPECT_EQ(code, 0) << out;
    EXPECT_EQ(out.rfind("PASS nvjpg", 0), 0u) << out;
    EXPECT_NE(out.find("libjpeg-turbo"), std::string::npos) << "the libjpeg-turbo comparisons ran: " << out;
}

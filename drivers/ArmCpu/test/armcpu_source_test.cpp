// XNU takes ml_processor_info_t's powergate_latency as cpu_idle_latency (both trees' machine_routines.c), and
// ml_delay_should_spin() spins short IODelay() calls shorter than cpu_idle_latency instead of timer-sleeping.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <regex>
#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::SourceText;
using testing::HasSubstr;

TEST(ArmCpuSource, RegistersEachCoreWithShortSpinLatency)
{
    const std::string source = SourceText("../ArmCpu.cpp");
    EXPECT_THAT(source, HasSubstr("ml_processor_register(&info"));
    const std::regex latency(R"(info\.powergate_latency = 25000;)");
    EXPECT_EQ(std::distance(std::sregex_iterator(source.begin(), source.end(), latency), std::sregex_iterator()), 2);
}

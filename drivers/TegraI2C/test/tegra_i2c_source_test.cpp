// TegraI2C's bus table (kBus): I2C3 alone has chained transactions on. The touch scenarios (drivers/SwitchTouch/test)
// model I2C3's controller; nothing models the others, so this guards that a table edit does not chain them unseen.
// And a controller's clock comes up in the Tegra X1 TRM's order (5.1.7), which the touch model's CAR cannot see.
#include <gtest/gtest.h>

#include <regex>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"

TEST(TegraI2CSource, OnlyI2C3IsChained)
{
    const std::string table = switch_ios::test::Between(switch_ios::test::SourceText("../TegraI2C.cpp"),
                                                        "constexpr BusInfo kBus[kBuses] = {", "};");
    // One entry per bus: "{base, ..., chained, request, channel},  // name".
    std::vector<std::string> chained;
    const std::regex entry(R"(\{\s*(0x[0-9a-fA-F]+),[^{}]*?,\s*(true|false),[^{}]*\})");
    unsigned buses = 0;
    for (std::sregex_iterator it(table.begin(), table.end(), entry), end; it != end; ++it) {
        ++buses;
        if ((*it)[2] == "true")
            chained.push_back((*it)[1]);
    }
    EXPECT_EQ(buses, 5u) << "the five Tegra210 I2C controllers";
    EXPECT_EQ(chained, std::vector<std::string>{"0x0500"}) << "I2C3 alone is chained";
}

// TRM 5.1.7 "To set up a non-boot device": reset asserted, clock enabled, divisor, 2 us, source, 2 us, reset released;
// the clock is never gated while it is set up.
TEST(TegraI2CSource, AControllerClockComesUpInTheTrmOrder)
{
    const std::string body = switch_ios::test::Body(switch_ios::test::SourceText("../TegraI2C.cpp"),
                                                    "void clockEnable(const BusInfo &info)");
    const std::vector<std::string> steps = {
        "w32(gCar, info.reset, bit);",
        "w32(gCar, info.enable, bit);",
        "w32(gCar, info.source, (r32(gCar, info.source) & ~0xffffu) | divisor);",
        "IODelay(2);",
        "w32(gCar, info.source, (6u << 29) | divisor);",
        "IODelay(2);",
        "w32(gCar, info.reset + 4, bit);",
    };
    size_t at = 0;
    for (const auto &step : steps) {
        const size_t next = body.find(step, at);
        ASSERT_NE(next, std::string::npos) << step << " after " << at << " in\n" << body;
        at = next + step.size();
    }
    EXPECT_EQ(body.find("info.enable + 4"), std::string::npos) << "the clock gated during its set-up";
}

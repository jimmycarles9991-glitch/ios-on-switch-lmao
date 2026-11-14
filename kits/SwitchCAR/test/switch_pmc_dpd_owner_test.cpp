// PMC IO_DPD_REQ and IO_DPD2_REQ share one state machine that drops a request written before the previous one finished,
// SEL_DPD_TIM APB clocks plus five after it (TRM 12.6.111). The kit's pad requests are their one writer and leave that
// time after each, so a request never lands on another, whichever driver asks and at whatever APB clock.
#include "kits/SwitchCAR/SwitchPMC.h"
#include "switch_car_owner_model.h"
#include "switch_car_owner_stubs.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"

namespace {
using namespace switch_car_owner_model;

constexpr uint32_t kClkSystemRate = 0x030;
constexpr unsigned kDsi = 2, kDsib = 32 + 7, kPexBias = 4, kPexClk1 = 5;

class DpdOwner : public testing::Test {
protected:
    void SetUp() override
    {
        reset();
    }
};

// A display power-up as TegraDisplayPower asks it: DSI already out of deep power-down, DSIB still in it.
TEST_F(DpdOwner, ARequestThatChangesNothingDoesNotSwallowTheNextPads)
{
    reg(kCar + kClkSystemRate) = 2;  // hekate: SCLK 204 MHz, PCLK / 3
    apbKHz(68000);
    reg(kPmc + kDpd2Status) = 1u << 7;
    EXPECT_TRUE(switch_pmc_io_pad_on(kDsi));
    EXPECT_TRUE(switch_pmc_io_pad_on(kDsib));
    EXPECT_EQ(reg(kPmc + kDpd2Status), 0u);
    EXPECT_EQ(droppedDpdRequests(), 0u);
}

TEST_F(DpdOwner, BackToBackPadsAtTheSlowestApbClockAreAllTaken)
{
    reg(kCar + kClkSystemRate) = 0x33;  // HCLK and PCLK each / 4, from CLK_M
    apbKHz(38400 / 16);
    reg(kPmc + kDpdStatus) = 1u << kPexBias | 1u << kPexClk1;
    EXPECT_TRUE(switch_pmc_io_pad_on(kPexBias));
    EXPECT_TRUE(switch_pmc_io_pad_on(kPexClk1));
    EXPECT_TRUE(switch_pmc_io_pad_off(kDsi));
    EXPECT_EQ(reg(kPmc + kDpdStatus), 1u << kDsi);
    EXPECT_EQ(droppedDpdRequests(), 0u);
}

TEST(DpdOwnerSource, OnlyTheKitWritesTheRequestRegisters)
{
    const std::regex call(R"((write32\w*|rmw32|\bw32|\.modify|switch_pmc_write)\s*\()");
    const std::regex named(R"(IO_DPD2?_REQ|IoDpd2?Req|DPD2?_REQ)");
    std::vector<std::string> writers;
    for (const char *tree : {"../../../drivers/", "../../"}) {
        const std::filesystem::path root = switch_ios::test::PackagePath(tree);
        for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
            const std::string name = entry.path().generic_string();
            const std::string ext = entry.path().extension().string();
            if ((ext != ".c" && ext != ".cpp" && ext != ".h") || name.find("/test/") != std::string::npos ||
                name.ends_with("kits/SwitchCAR/SwitchCAR.cpp"))
                continue;
            std::ifstream file(entry.path(), std::ios::binary);
            std::string line;
            for (unsigned number = 1; std::getline(file, line); ++number) {
                if (line.find("DPD") == std::string::npos && line.find("Dpd") == std::string::npos)
                    continue;
                if (std::regex_search(line, call) && std::regex_search(line, named))
                    writers.push_back(name + ":" + std::to_string(number) + ": " + line);
            }
        }
    }
    EXPECT_THAT(writers, testing::IsEmpty());
}
}  // namespace

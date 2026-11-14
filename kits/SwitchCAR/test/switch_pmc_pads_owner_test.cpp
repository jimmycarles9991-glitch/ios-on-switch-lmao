// PMC NO_IOPOWER (0x44), PWR_DET (0x48) and PWR_DET_VAL (0xe4) hold one bit per I/O rail for rails different drivers
// own (SDMMC1, audio, UART, GPIO), with no set/clear form (TRM 12.6.18, 12.6.19, 12.6.58). The kit's switch_pmc_rmw is
// their one writer: two drivers changing their rails at once keep both changes.
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

constexpr uint32_t kNoIoPower = 0x44, kPwrDetVal = 0xe4;
constexpr uint32_t kSdmmc1 = 1u << 12, kAudioHv = 1u << 18, kUart = 1u << 2, kGpio = 1u << 21;

class PadsOwner : public testing::Test {
protected:
    void SetUp() override
    {
        reset();
    }
};

TEST_F(PadsOwner, AJoyConRailPoweredDuringAnSdPowerOffKeepsItsPower)
{
    reg(kPmc + kNoIoPower) = kUart;
    landsAtTheNextRead(kPmc + kNoIoPower, [] {
        switch_pmc_rmw(kNoIoPower, kUart, 0);
    });
    switch_pmc_rmw(kNoIoPower, 0, kSdmmc1);
    EXPECT_TRUE(secondRan());
    EXPECT_EQ(reg(kPmc + kNoIoPower), kSdmmc1) << "the UART pads must stay powered";
}

TEST_F(PadsOwner, AnSdVoltageChangeKeepsTheGpioRailsSetting)
{
    reg(kPmc + kPwrDetVal) = kSdmmc1 | kGpio | kAudioHv;
    landsAtTheNextRead(kPmc + kPwrDetVal, [] {
        switch_pmc_rmw(kPwrDetVal, kGpio, 0);
    });
    switch_pmc_rmw(kPwrDetVal, kSdmmc1, 0);
    EXPECT_TRUE(secondRan());
    EXPECT_EQ(reg(kPmc + kPwrDetVal), kAudioHv);
}

// Every driver names these registers only to read them or to hand a change to switch_pmc_rmw.
TEST(PadsOwnerSource, OnlyTheKitWritesTheRailWords)
{
    const std::regex call(R"((write32\w*|rmw32|\bw32|\.modify|pmcWrite|switch_pmc_write)\s*\()");
    const std::regex named(R"(NO_IOPOWER|PWR_DET|NoIoPower|PwrDet)");
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
                if (line.find("IOPOWER") == std::string::npos && line.find("PWR_DET") == std::string::npos &&
                    line.find("IoPower") == std::string::npos && line.find("PwrDet") == std::string::npos)
                    continue;
                if (std::regex_search(line, call) && std::regex_search(line, named))
                    writers.push_back(name + ":" + std::to_string(number) + ": " + line);
            }
        }
    }
    EXPECT_THAT(writers, testing::IsEmpty());
}
}  // namespace

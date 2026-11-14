// The fuse cache answers only with the FUSE clock on (CLK_OUT_ENB_H bit 7) and MISC_CLK_ENB's CFG_ALL_VISIBLE (bit 28)
// set. MISC_CLK_ENB has no set/clear form (TRM 5.2.13): the kit is its one writer (switch_car_fuse_readable), and
// nobody turns either back off.
#include "kits/SwitchCAR/SwitchCAR.h"
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

constexpr uint32_t kMiscClkEnb = 0x048, kClkOutEnbH = 0x014, kClkEnbHSet = 0x328, kClkEnbHClr = 0x32c;
constexpr uint32_t kFuseClock = 1u << 7, kVisible = 1u << 28, kDev1OscDiv = 1u << 22;

class FuseOwner : public testing::Test {
protected:
    void SetUp() override
    {
        reset();
    }
};

TEST_F(FuseOwner, TheFuseClockGoesOnThroughItsSetRegisterAlone)
{
    switch_car_fuse_readable();
    for (const auto &[address, value] : writes()) {
        EXPECT_NE(address, kCar + kClkOutEnbH);
        EXPECT_NE(address, kCar + kClkEnbHClr);
    }
    EXPECT_THAT(writes(), testing::Contains(std::make_pair(kCar + kClkEnbHSet, kFuseClock)));
}

TEST_F(FuseOwner, AnotherKitWriterOfMiscClkEnbAndTheVisibleBitBothStay)
{
    reg(kCar + kMiscClkEnb) = 1u;
    landsAtTheNextRead(kCar + kMiscClkEnb, [] {
        switch_car_rmw(kMiscClkEnb, 0, kDev1OscDiv);
    });
    switch_car_fuse_readable();
    EXPECT_TRUE(secondRan());
    EXPECT_EQ(reg(kCar + kMiscClkEnb), 1u | kDev1OscDiv | kVisible);
}

TEST_F(FuseOwner, NoWriteEverClearsTheVisibleBit)
{
    switch_car_fuse_readable();
    switch_car_rmw(kMiscClkEnb, kDev1OscDiv, 0);
    switch_car_fuse_readable();
    for (const auto &[address, value] : writes())
        if (address == kCar + kMiscClkEnb)
            EXPECT_TRUE(value & kVisible) << std::hex << value;
}

struct Site {
    std::string where, line;
};

std::vector<Site> sitesMatching(const std::vector<std::string> &names, const std::regex &call, const std::regex &target)
{
    std::vector<Site> sites;
    for (const char *tree : {"../../../drivers/", "../../"}) {
        const std::filesystem::path root = switch_ios::test::PackagePath(tree);
        for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
            const std::string name = entry.path().generic_string();
            const std::string ext = entry.path().extension().string();
            if ((ext != ".c" && ext != ".cpp" && ext != ".h") || name.find("/test/") != std::string::npos)
                continue;
            std::ifstream file(entry.path(), std::ios::binary);
            std::string line;
            for (unsigned number = 1; std::getline(file, line); ++number) {
                bool named = false;
                for (const std::string &n : names)
                    named = named || line.find(n) != std::string::npos;
                if (!named)
                    continue;
                if (std::regex_search(line, call) && std::regex_search(line, target))
                    sites.push_back({name + ":" + std::to_string(number), line});
            }
        }
    }
    return sites;
}

TEST(FuseOwnerSource, OnlyTheKitWritesMiscClkEnb)
{
    const std::regex call(R"((write32\w*|rmw32|\bw32|\.modify|nvkm_wr32|nvkm_mask)\s*\()");
    const std::regex misc(R"(MISC_CLK_ENB|MiscClkEnb)");
    std::vector<std::string> writers;
    for (const Site &site : sitesMatching({"MISC_CLK_ENB", "MiscClkEnb"}, call, misc))
        if (site.where.find("kits/SwitchCAR/SwitchCAR.cpp") == std::string::npos)
            writers.push_back(site.where + ": " + site.line);
    EXPECT_THAT(writers, testing::IsEmpty());
}

// SwitchUSB's XudcHardware.cpp turns the clock back off after its own fuse reads; that is the one left.
TEST(FuseOwnerSource, NobodyElseTurnsTheFuseClockOff)
{
    const std::regex call(R"((write32\w*|\bw32|switch_car_clock_disable)\s*\()");
    const std::regex fuse(
        R"((CLK_ENB_H_CLR|ClkEnbHClr)[^;]*(FUSE|Fuse)|switch_car_clock_disable\s*\(\s*(kFuseClock|39)\b)");
    std::vector<std::string> clearers;
    for (const Site &site : sitesMatching({"CLK_ENB_H_CLR", "ClkEnbHClr", "switch_car_clock_disable"}, call, fuse))
        if (site.where.find("SwitchUSB/XudcHardware.cpp") == std::string::npos)
            clearers.push_back(site.where + ": " + site.line);
    EXPECT_THAT(clearers, testing::IsEmpty());
}
}  // namespace

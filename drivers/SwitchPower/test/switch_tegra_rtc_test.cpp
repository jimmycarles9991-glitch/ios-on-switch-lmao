// SwitchTegraRTC.h: the Tegra RTC's one reading (MILLI_SECONDS, which latches SECONDS into SHADOW_SECONDS, then
// SHADOW_SECONDS; TRM 11.2.4-11.2.5) against a model of that latch, and the source of its only two callers.
#include "drivers/SwitchPower/SwitchTegraRTC.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"

namespace {
constexpr uintptr_t kPmcPage = 0x7000e000;
constexpr uint32_t kSeconds = 0x008, kShadowSeconds = 0x00c, kMilliSeconds = 0x010;

struct Rtc {
    uint32_t seconds = 0, millis = 0, shadow = 0;
    std::function<void()> afterMilliRead;
} gRtc;

TEST(TegraRtc, TheSecondsAreTheOnesLatchedWithTheMilliseconds)
{
    gRtc = {41, 999, 0, [] {
                gRtc.seconds = 42;
                gRtc.millis = 0;
            }};
    const switch_tegra_rtc_reading reading =
        switch_tegra_rtc_read(reinterpret_cast<const volatile uint8_t *>(kPmcPage));
    EXPECT_EQ(reading.seconds, 41u);
    EXPECT_EQ(reading.millis, 999u);
}

// Any other MILLI_SECONDS read between the two would replace the latched seconds: its callers cannot overlap.
TEST(TegraRtcSource, OneAccessorReadsTheLatchedPair)
{
    std::vector<std::string> readers;
    for (const char *tree : {"../../", "../../../kits/"}) {
        const std::filesystem::path root = switch_ios::test::PackagePath(tree);
        for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
            const std::string name = entry.path().generic_string();
            const std::string ext = entry.path().extension().string();
            if ((ext != ".c" && ext != ".cpp" && ext != ".h") || name.find("/test/") != std::string::npos ||
                name.ends_with("SwitchPower/SwitchTegraRTC.h"))
                continue;
            std::ifstream file(entry.path(), std::ios::binary);
            std::string line;
            for (unsigned number = 1; std::getline(file, line); ++number) {
                const size_t code = line.find_first_not_of(" \t");
                if (code == std::string::npos || line.compare(code, 2, "//") == 0 || line[code] == '*')
                    continue;
                if (line.find("SHADOW_SECONDS") != std::string::npos || line.find("MILLI_SECONDS") != std::string::npos)
                    readers.push_back(name + ":" + std::to_string(number) + ": " + line);
            }
        }
    }
    EXPECT_THAT(readers, testing::IsEmpty());
}

TEST(TegraRtcSource, SleepReadsItOnlyAloneOnTheBootCpu)
{
    const std::string suspend = switch_ios::test::SourceText("../../SwitchSuspend/SwitchSuspend.cpp");
    EXPECT_EQ(switch_ios::test::Count(suspend, "switch_tegra_rtc_read("), 1u);
    EXPECT_EQ(switch_ios::test::Count(suspend, "rtcMS()"), 3u);
    EXPECT_EQ(switch_ios::test::Count(switch_ios::test::Body(suspend, "void SwitchSuspend::quiesce()"), "rtcMS()"), 1u);
    EXPECT_EQ(switch_ios::test::Count(switch_ios::test::Body(suspend, "void SwitchSuspend::activate()"), "rtcMS()"),
              1u);
    const std::string monotonic = switch_ios::test::SourceText("../SwitchMonotonicClock.cpp");
    const std::string latch = switch_ios::test::Body(monotonic, "LatchedTime latch()");
    EXPECT_THAT(latch, testing::HasSubstr("IOSimpleLockLockDisableInterrupt(gLatch)"));
    EXPECT_THAT(latch, testing::HasSubstr("switch_tegra_rtc_read(gRtc)"));
}
}  // namespace

extern "C" {
uint32_t switch_mmio_model_read32(uintptr_t address)
{
    switch (address - kPmcPage) {
    case kSeconds:
        return gRtc.seconds;
    case kShadowSeconds:
        return gRtc.shadow;
    case kMilliSeconds: {
        const uint32_t millis = gRtc.millis;
        gRtc.shadow = gRtc.seconds;
        if (gRtc.afterMilliRead)
            gRtc.afterMilliRead();
        return millis;
    }
    default:
        return 0;
    }
}

void switch_mmio_model_write32(uintptr_t, uint32_t)
{}

uint16_t switch_mmio_model_read16(uintptr_t)
{
    return 0;
}

void switch_mmio_model_write16(uintptr_t, uint16_t)
{}

uint8_t switch_mmio_model_read8(uintptr_t)
{
    return 0;
}

void switch_mmio_model_write8(uintptr_t, uint8_t)
{}
}

// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchConsoleReleaseDisplay.h: when the data migration's console hold drops, the compositor's current frame (the one
// Core Animation believes is on screen) goes to the screen without waiting for its next swap, once, and never retires
// again; the first swap after it takes it back out of the display's books.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::SourceText;
using testing::HasSubstr;

#define IOLog(...) (++logs)

namespace {
unsigned logs;

uint64_t mach_absolute_time()
{
    return 5;
}

struct OriginalLegacySwapRequest {
    int id;
};

struct NativeLegacyDisplay {
    const OriginalLegacySwapRequest *current = nullptr;  // first: the family's current swap at swapQueueCurrent
    static constexpr size_t swapQueueCurrent = 0;
    std::set<const OriginalLegacySwapRequest *> taken, validated;
    unsigned kicks = 0;
    bool latchRetire = true;

    void untake(const OriginalLegacySwapRequest *r)
    {
        taken.erase(r);
    }

    void removeValidated(const OriginalLegacySwapRequest *r)
    {
        validated.erase(r);
    }

    bool addValidated(const OriginalLegacySwapRequest *r, uint64_t)
    {
        return validated.insert(r).second;
    }

    uint64_t seqOf(const OriginalLegacySwapRequest *r) const
    {
        return taken.count(r) ? 1 : 0;
    }

    void kickSwapQueue()
    {
        ++kicks;
    }

#include "../SwitchConsoleReleaseDisplay.h"
};

class ConsoleRelease : public testing::Test {
protected:
    NativeLegacyDisplay display;
    OriginalLegacySwapRequest shown{1}, next{2};

    void SetUp() override
    {
        logs = 0;
    }
};

TEST_F(ConsoleRelease, WithoutAReleaseTheQueueIsUntouched)
{
    display.current = &shown;
    EXPECT_EQ(display.consoleReleasedFrame(nullptr), nullptr);
    EXPECT_EQ(display.consoleReleasedFrame(&next), &next);
    EXPECT_TRUE(display.validated.empty());
}

TEST_F(ConsoleRelease, TheCurrentFrameGoesOnceUntilTaken)
{
    display.current = &shown;
    display.consoleReleasedKick();
    EXPECT_EQ(display.kicks, 1u);
    EXPECT_EQ(display.consoleReleasedFrame(nullptr), &shown);
    EXPECT_TRUE(display.validated.count(&shown));
    EXPECT_EQ(display.consoleReleasedFrame(nullptr), &shown);  // a kick that deferred tries it again
    EXPECT_EQ(logs, 1u);
    display.taken.insert(&shown);
    EXPECT_EQ(display.consoleReleasedFrame(nullptr), nullptr);
    EXPECT_FALSE(display.consoleReleased);
}

TEST_F(ConsoleRelease, TheNextSwapTakesTheReshownFrameOutOfTheBooks)
{
    display.current = &shown;
    display.consoleReleasedKick();
    ASSERT_EQ(display.consoleReleasedFrame(nullptr), &shown);
    display.taken.insert(&shown);
    EXPECT_EQ(display.consoleReleasedFrame(&next), &next);
    EXPECT_FALSE(display.taken.count(&shown));
    EXPECT_FALSE(display.validated.count(&shown));
    EXPECT_EQ(display.consoleReshown, nullptr);
}

TEST_F(ConsoleRelease, AQueuedSwapOrNoCurrentFrameNeedsNoReshow)
{
    display.consoleReleasedKick();
    EXPECT_EQ(display.consoleReleasedFrame(nullptr), nullptr);
    EXPECT_FALSE(display.consoleReleased);
    display.current = &shown;
    display.consoleReleasedKick();
    EXPECT_EQ(display.consoleReleasedFrame(&next), &next);
    EXPECT_FALSE(display.consoleReleased);
    EXPECT_EQ(display.consoleReleasedFrame(nullptr), nullptr);
    EXPECT_EQ(logs, 0u);
}

// Retiring at the end of a copy (no direct scanout) would refuse the reshown swap and leave the next one waiting.
TEST_F(ConsoleRelease, OnlyWhileSwapsRetireAtTheLatch)
{
    display.current = &shown;
    display.latchRetire = false;
    display.consoleReleasedKick();
    EXPECT_EQ(display.consoleReleasedFrame(nullptr), nullptr);
    EXPECT_FALSE(display.consoleReleased);
    EXPECT_TRUE(display.validated.empty());
}

// Both kernels call the display right where the hold drops, and the kick asks before it validates its request.
TEST(ConsoleReleaseSource, BothKernelsKickTheDisplayWhereTheHoldDrops)
{
    for (const char *tree : {"../../../kernel/xnu-10063.141.1/bsd/kern/kern_exec.c",
                             "../../../kernel/xnu-12377.121.6/bsd/kern/kern_exec.c"}) {
        const std::string exec = SourceText(tree);
        const std::string tick = Between(exec, "native_migration_tick(thread_call_param_t", "\n}\n");
        const std::string hook = "switch_console_hold = 0;\n\t\tnative_legacy_console_released();";
        size_t drops = 0;
        for (size_t at = tick.find("switch_console_hold = 0;"); at != std::string::npos;
             at = tick.find("switch_console_hold = 0;", at + 1)) {
            ++drops;
            EXPECT_EQ(tick.compare(at, hook.size(), hook), 0) << tree;
        }
        EXPECT_EQ(drops, 2u) << tree;
        EXPECT_THAT(exec, HasSubstr("__attribute__((weak)) void\nnative_legacy_console_released(void)\n{\n}"));
    }
    // Neither kernel injects CA_FORCE_TIMER: both render servers run Core Animation's CADisplayTimer
    for (const char *tree : {"../../../kernel/xnu-12377.121.6/bsd/kern/kern_exec.c",
                             "../../../kernel/xnu-10063.141.1/bsd/kern/kern_exec.c"})
        EXPECT_THAT(SourceText(tree), testing::Not(HasSubstr("\"CA_FORCE_TIMER=1\""))) << tree;
    const std::string display = SourceText("../NativeLegacyDisplay.cpp");
    const std::string kick = Between(display, "    void kickOnce()\n", "\n    }\n");
    EXPECT_LT(kick.find("consoleReleasedFrame(latchRetire ? firstUntaken() : pendingSwapRequest())"),
              kick.find("if (!request || !isValidated(request))"));
    EXPECT_THAT(SourceText("../SwitchConsoleRelease.h"), HasSubstr("->consoleReleasedKick();"));
}
}  // namespace

// SPDX-License-Identifier: MIT
// refusal_text.h on the host: the reason a command buffer that was not submitted logs and fails with.
#include "../refusal_text.h"
#include <gtest/gtest.h>
#include <string>

namespace {

using gm20b::NotSubmitted;

constexpr unsigned kCannotLock = 0xe00002cc;  // kIOReturnCannotLock
constexpr unsigned kNotReady = 0xe00002d8;    // kIOReturnNotReady

std::string Why(const NotSubmitted &n)
{
    char line[192];
    gm20b::describeRefusal(n, line, sizeof line);
    return line;
}

TEST(RefusalText, TheFirstRefusalThatHoldsIsTheReason)
{
    EXPECT_EQ(Why({"a draw of 0 vertices", 0xe00002bc, kNotReady, "no channel"}), "a draw of 0 vertices");
    EXPECT_EQ(Why({"", 0xe00002bc, kNotReady, "no channel"}), "the kernel refused its job (0xe00002bc)");
    EXPECT_EQ(Why({nullptr, 0, kNotReady, "no channel"}), "a view of a surface's own planes was not made (0xe00002d8)");
    EXPECT_EQ(Why({nullptr, 0, 0, "no channel"}), "no channel");
    EXPECT_EQ(Why({}), "nothing named why");
}

TEST(RefusalText, AReasonLongerThanTheLineIsCutNotOverrun)
{
    const std::string longer(400, 'x');
    char line[16];
    gm20b::describeRefusal({longer.c_str(), 0, 0, nullptr}, line, sizeof line);
    EXPECT_EQ(std::string(line), std::string(15, 'x'));
}

TEST(RefusalText, ARefusalTheCallerTakesUpAgainLogsNothing)
{
    EXPECT_TRUE(gm20b::takenUpAgain({nullptr, kCannotLock, 0, nullptr}, kCannotLock)) << "the CPU holds a use";
    EXPECT_TRUE(gm20b::takenUpAgain({nullptr, kNotReady, kNotReady, nullptr}, kCannotLock)) << "a surface changed";
    EXPECT_FALSE(gm20b::takenUpAgain({nullptr, 0xe00002bc, kNotReady, nullptr}, kCannotLock));
    EXPECT_FALSE(gm20b::takenUpAgain({"a refused state", 0, 0, nullptr}, kCannotLock));
}

TEST(RefusalText, TheFirstSixteenThenEvery256thAreLogged)
{
    unsigned logged = 0;
    for (uint32_t n = 1; n <= 1024; n++)
        logged += gm20b::notSubmittedLineDue(n);
    EXPECT_EQ(logged, 16u + 4u);
    EXPECT_TRUE(gm20b::notSubmittedLineDue(16));
    EXPECT_FALSE(gm20b::notSubmittedLineDue(17));
    EXPECT_TRUE(gm20b::notSubmittedLineDue(256));
}

}  // namespace

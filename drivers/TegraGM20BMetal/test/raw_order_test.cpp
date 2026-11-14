// SPDX-License-Identifier: MIT
#include <IOKit/IOReturn.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "../raw/runtime/raw_order.h"
#include "bazel/testing/source_text.h"

namespace {

TEST(RawOrder, OnlyTheKernelsOwnAnswersAreRefusals)
{
    EXPECT_EQ(raw::orderAnswer(KERN_SUCCESS), raw::OrderAnswer::ordered);
    EXPECT_EQ(raw::orderAnswer(raw::kOrderNoChannel), raw::OrderAnswer::noChannel);
    EXPECT_EQ(raw::orderAnswer(kIOReturnBadArgument), raw::OrderAnswer::refused);
    EXPECT_EQ(raw::orderAnswer(kIOReturnNoResources), raw::OrderAnswer::refused);
    EXPECT_NE(raw::kOrderNoChannel, kIOReturnError);
    EXPECT_NE(raw::kOrderNoChannel, kIOReturnNotResponding);
}

// A refused Order fails the command buffer that needed it, with one error line; nothing latches CPU order, so the next
// command buffer is ordered afresh
TEST(RawOrder, ARefusedOrderFailsItsCommandBufferAndLatchesNothing)
{
    const std::string submit = switch_ios::test::SourceText("../raw/runtime/RawMetalSubmit.mm");
    const std::string refused = switch_ios::test::Body(submit, "NSString *orderRefused(const char *what, int kr)");
    EXPECT_THAT(refused, testing::HasSubstr("os_log_error("));
    EXPECT_THAT(refused, testing::Not(testing::HasSubstr("gpuOrder")));
    EXPECT_THAT(submit, testing::HasSubstr("return failSubmission(cb, orderRefused(\"raw after m2v\", kr));"));
    EXPECT_THAT(submit, testing::HasSubstr("return failSubmission(cb, orderRefused(\"m2v after raw\", kr));"));
    EXPECT_THAT(submit, testing::Not(testing::HasSubstr("orderReturned")));
    const std::string jobs = switch_ios::test::SourceText("../raw/runtime/RawMetalEventJobs.mm");
    EXPECT_THAT(jobs, testing::HasSubstr("NSString *why = orderRefused(\"event job after m2v\", kr);"));
}

}  // namespace

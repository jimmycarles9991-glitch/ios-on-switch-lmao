// SPDX-License-Identifier: MIT
#include <IOKit/IOReturn.h>
#include <gtest/gtest.h>
#include "../raw/runtime/raw_order.h"

namespace {

// RawMetalSubmit.mm's use: a refusal orders on the CPU, a raw submission after the kernel made the channel again takes
// GPU order back
struct DriverOrder {
    bool gpu;
    bool canOrder;
    uint64_t refusedAt = 0;

    void ordered(int kr, uint64_t reopens)
    {
        if (raw::orderAnswer(kr) == raw::OrderAnswer::refused) {
            gpu = false;
            refusedAt = reopens;
        }
    }

    void submitted(uint64_t reopens)
    {
        if (raw::orderReturns(gpu, canOrder, reopens, refusedAt))
            gpu = true;
    }
};

TEST(RawOrder, OnlyTheKernelsOwnAnswersAreRefusals)
{
    EXPECT_EQ(raw::orderAnswer(KERN_SUCCESS), raw::OrderAnswer::ordered);
    EXPECT_EQ(raw::orderAnswer(raw::kOrderNoChannel), raw::OrderAnswer::noChannel);
    EXPECT_EQ(raw::orderAnswer(kIOReturnBadArgument), raw::OrderAnswer::refused);
    EXPECT_EQ(raw::orderAnswer(kIOReturnNoResources), raw::OrderAnswer::refused);
    EXPECT_NE(raw::kOrderNoChannel, kIOReturnError);
    EXPECT_NE(raw::kOrderNoChannel, kIOReturnNotResponding);
}

TEST(RawOrder, AChannelBeingMadeAgainKeepsGpuOrder)
{
    DriverOrder order{true, true};
    order.ordered(raw::kOrderNoChannel, 0);
    order.ordered(raw::kOrderNoChannel, 1);
    EXPECT_TRUE(order.gpu);
    order.ordered(KERN_SUCCESS, 1);
    EXPECT_TRUE(order.gpu);
}

TEST(RawOrder, ARefusalLastsUntilTheKernelMakesTheChannelAgain)
{
    DriverOrder order{true, true};
    order.ordered(kIOReturnNoResources, 2);
    EXPECT_FALSE(order.gpu);
    order.submitted(2);
    EXPECT_FALSE(order.gpu);
    order.submitted(3);
    EXPECT_TRUE(order.gpu);
}

TEST(RawOrder, AKernelWithoutOrderStaysCpuOrdered)
{
    DriverOrder order{false, false};
    order.submitted(5);
    EXPECT_FALSE(order.gpu);
    DriverOrder refused{true, false};
    refused.ordered(kIOReturnBadArgument, 0);
    refused.submitted(1);
    EXPECT_FALSE(refused.gpu);
}

}  // namespace

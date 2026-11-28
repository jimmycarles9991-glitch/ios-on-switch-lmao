// SPDX-License-Identifier: BSD-2-Clause-Patent
// Source guards for TegraSPI's hot path: no redundant irqLock, no full-system dmb sy, no hard-IRQ CAS, and
// skip clearStatus() on clean FIFOs.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(TegraSPISource, HotPathHasNoIrqLockDmbSyOrHardIrqCas)
{
    const std::string spi = SourceText("../TegraSPI.cpp");
    EXPECT_THAT(spi, Not(HasSubstr("irqLock")));
    EXPECT_THAT(spi, Not(HasSubstr("dmb sy")));
    EXPECT_THAT(spi, Not(HasSubstr("IOWorkLoop")));
    EXPECT_THAT(spi, Not(HasSubstr("IOFilterInterruptEventSource")));
    EXPECT_THAT(spi, HasSubstr("registerInterrupt(0,"));
    EXPECT_THAT(spi, HasSubstr("switch_mmio_write32_publish(b.regs, COMMAND1, cmd | CMD1_PIO);"));
    EXPECT_THAT(spi, HasSubstr("ensureCleanFifos(b)"));

    const std::string isr = Body(spi, "void spiInterrupt(");
    ASSERT_FALSE(isr.empty());
    EXPECT_THAT(isr, Not(HasSubstr("__atomic_compare_exchange")));
    EXPECT_LT(isr.find("TRANS_STATUS, trans"), isr.find("kDone, __ATOMIC_RELEASE"));

    const std::string sleep = Body(spi, "int sleepForAction(");
    ASSERT_FALSE(sleep.empty());
    EXPECT_THAT(sleep, HasSubstr("b.irqNub->disableInterrupt(0);"));
    EXPECT_THAT(sleep, Not(HasSubstr("__atomic_compare_exchange")));
}

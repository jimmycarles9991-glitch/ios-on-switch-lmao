// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "../SwitchSdCardDetect.h"
#include "bazel/testing/source_text.h"
#include "source_checks.h"

using switch_ios::test::Body;
using switch_ios::test::Index;
using switch_ios::test::Search;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {
const std::string &driver()
{
    static const std::string source = SourceText("../NativeSwitchBlock.cpp");
    return source;
}

std::string body(const char *signature)
{
    return Body(driver(), signature);
}
}  // namespace

TEST(SdCardDetect, ActiveLowLevelRules)
{
    EXPECT_FALSE(switchSdCardRemoved(-1));
    EXPECT_FALSE(switchSdCardRemoved(0));
    EXPECT_TRUE(switchSdCardRemoved(1));

    EXPECT_TRUE(switchSdShouldPanicOnRemoval(true, false, 1));
    EXPECT_FALSE(switchSdShouldPanicOnRemoval(true, false, 0));
    EXPECT_FALSE(switchSdShouldPanicOnRemoval(true, false, -1));
    EXPECT_FALSE(switchSdShouldPanicOnRemoval(false, false, 1));
    EXPECT_FALSE(switchSdShouldPanicOnRemoval(true, true, 1));
    EXPECT_FALSE(switchSdShouldPanicOnRemoval(false, true, 1));
}

TEST(SdCardDetectSource, TegraGpioDefinesAndMuxesPz1WithPullUp)
{
    const std::string gpioH = SourceText("../../TegraGPIO/TegraGPIO.h");
    EXPECT_THAT(gpioH, HasSubstr("TEGRA_GPIO_PZ1 = 25 * 8 + 1"));
    EXPECT_THAT(gpioH, HasSubstr("TEGRA_GPIO_SD_CD = TEGRA_GPIO_PZ1"));

    const std::string gpioCpp = SourceText("../../TegraGPIO/TegraGPIO.cpp");
    EXPECT_THAT(gpioCpp, HasSubstr("{TEGRA_GPIO_PZ1, 0x280, PULL_UP}"));
}

TEST(SdCardDetectSource, PrimaryInterruptFilterAndLifecycleOrder)
{
    EXPECT_THAT(driver(), Not(HasSubstr("irqLoop")));
    EXPECT_THAT(driver(), Not(HasSubstr("IOWorkLoop")));
    EXPECT_THAT(driver(), Not(HasSubstr("IOFilterInterruptEventSource")));

    const std::string startIrq = body("void NativeSwitchBlock::startInterrupt()");
    EXPECT_THAT(startIrq, HasSubstr("nub->registerInterrupt(0, this, &NativeSwitchBlock::interruptHandler, nullptr)"));
    EXPECT_THAT(startIrq, HasSubstr("irqNub->enableInterrupt(0)"));

    const std::string startCd = body("void NativeSwitchBlock::startCardDetect()");
    EXPECT_THAT(startCd, HasSubstr("tegra_gpio_interrupt_nub(TEGRA_GPIO_SD_CD, TEGRA_GPIO_EDGE_RISING)"));
    EXPECT_THAT(startCd, HasSubstr("nub->registerInterrupt(0, this, &NativeSwitchBlock::cardDetectHandler, nullptr)"));
    EXPECT_LT(Index(startCd, "__atomic_store_n(&cardDetectArmed, true, __ATOMIC_RELEASE)"),
              Index(startCd, "cdNub->enableInterrupt(0)"));
    EXPECT_LT(Index(startCd, "cdNub->enableInterrupt(0)"), Index(startCd, "checkCardPresent(\"boot\")"));

    const std::string setup = body("bool NativeSwitchBlock::setup()");
    EXPECT_LT(Index(setup, "kernel_thread_start("), Index(setup, "startCardDetect();"));

    const std::string handler = body("void NativeSwitchBlock::cardDetectHandler(");
    EXPECT_THAT(handler, HasSubstr("checkCardPresent(\"interrupt\")"));

    const std::string check = body("void NativeSwitchBlock::checkCardPresent(");
    EXPECT_LT(Index(check, "__atomic_load_n(&cardDetectArmed, __ATOMIC_ACQUIRE)"),
              Index(check, "tegra_gpio_read(TEGRA_GPIO_SD_CD)"));
    EXPECT_THAT(check, HasSubstr("switchSdShouldPanicOnRemoval(armed, panic_active() != 0, level)"));

    const std::string removePanic = body("void NativeSwitchBlock::cardRemovedPanic(");
    EXPECT_THAT(removePanic, HasSubstr("panic(\"SwitchSD: SD card removed"));
    EXPECT_THAT(removePanic, Not(HasSubstr("r32(PRESENT)")));

    const std::string failed = body("bool NativeSwitchBlock::failed(");
    EXPECT_LT(Index(failed, "checkCardPresent(\"io error\")"), Index(failed, "recover(true)"));

    const std::string suspend = body("void NativeSwitchBlock::suspend()");
    EXPECT_LT(Index(suspend, "__atomic_store_n(&cardDetectArmed, false, __ATOMIC_RELEASE)"),
              Index(suspend, "cdNub->disableInterrupt(0)"));
    EXPECT_LT(Index(suspend, "cdNub->disableInterrupt(0)"), Index(suspend, "powerOffCard()"));

    const std::string resume = body("void NativeSwitchBlock::resume()");
    EXPECT_LT(Index(resume, "__atomic_store_n(&cardDetectArmed, true, __ATOMIC_RELEASE)"),
              Index(resume, "cdNub->enableInterrupt(0)"));
    EXPECT_LT(Index(resume, "cdNub->enableInterrupt(0)"), Index(resume, "checkCardPresent(\"resume\")"));
    EXPECT_LT(Index(resume, "checkCardPresent(\"resume\")"), Index(resume, "powerOnController()"));

    const std::string shutdown = body("void NativeSwitchBlock::shutdown()");
    EXPECT_LT(Index(shutdown, "__atomic_store_n(&cardDetectArmed, false, __ATOMIC_RELEASE)"),
              Index(shutdown, "cdNub->disableInterrupt(0)"));

    const std::string stop = body("void NativeSwitchBlock::stop(");
    EXPECT_LT(Index(stop, "__atomic_store_n(&cardDetectArmed, false, __ATOMIC_RELEASE)"),
              Index(stop, "cdNub->disableInterrupt(0)"));
    EXPECT_LT(Index(stop, "cdNub->disableInterrupt(0)"), Index(stop, "cdNub->unregisterInterrupt(0)"));
    EXPECT_LT(Index(stop, "irqNub->disableInterrupt(0)"), Index(stop, "irqNub->unregisterInterrupt(0)"));
    EXPECT_FALSE(Search(startCd, R"(\b(IOTimerEventSource|thread_call|IOSleep)\b)"));
}

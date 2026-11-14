// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchPllxStart.h against a recording model of the CAR's PLLX registers: the Tegra X1 TRM's order for starting a PLL
// (5.1.5): out of IDDQ and 5 us before any divider is written, the dividers while IDDQ and ENABLE are 0, lock detection
// on (PLLX_MISC LOCK_ENABLE, 5.2.42) before ENABLE, ENABLE 0 to 1 last, and the lock read after the wait.
#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "../SwitchPllxStart.h"

namespace {
using namespace switchpllx;

struct Step {
    std::string what;
    uint32_t offset, value;
};

struct Model {
    std::map<uint32_t, uint32_t> regs;
    std::vector<Step> log;
    bool lockAfterWait = true;

    uint32_t read(uint32_t offset)
    {
        return regs[offset];
    }

    void write(uint32_t offset, uint32_t value)
    {
        if (offset == kBase && (value & kEnable) && !(regs[kBase] & kEnable)) {
            EXPECT_EQ(regs[kMisc3] & kIddq, 0u) << "ENABLE 0 to 1 with PLLX in IDDQ";
            EXPECT_EQ(regs[kMisc] & kLockEnable, kLockEnable) << "ENABLE before lock detection";
        }
        if (offset == kBase && (value & ~(kEnable | kBypass | kLock)) != (regs[kBase] & ~(kEnable | kBypass | kLock))) {
            EXPECT_EQ(regs[kMisc3] & kIddq, 0u) << "dividers written in IDDQ";
            EXPECT_EQ(regs[kBase] & kEnable, 0u) << "dividers written while enabled";
        }
        regs[offset] = value;
        log.push_back({"write", offset, value});
    }

    void delay(unsigned us)
    {
        log.push_back({"delay", 0, us});
    }

    void waitLock()
    {
        log.push_back({"wait", 0, 0});
        if (lockAfterWait && (regs[kBase] & kEnable) && (regs[kMisc] & kLockEnable))
            regs[kBase] |= kLock;
    }

    size_t indexOf(const std::string &what, uint32_t offset, uint32_t value) const
    {
        for (size_t i = 0; i < log.size(); ++i)
            if (log[i].what == what && log[i].offset == offset && log[i].value == value)
                return i;
        return log.size();
    }
};

constexpr uint32_t kBootPllx = kEnable | kLock | (0u << 20) | (78u << 8) | 2u;  // 38.4 MHz * 78 / 2 / 1 = 1497.6
constexpr uint32_t kRung = (0u << 20) | (87u << 8) | 2u;                        // 1670.4 MHz

uint32_t run(Model &m, uint32_t base)
{
    return start(
        m, base,
        [&m](unsigned us) {
            m.delay(us);
        },
        [&m] {
            m.waitLock();
        });
}

TEST(SwitchPllxStart, AfterSc7OutOfIddqAndFiveMicrosecondsBeforeTheDividers)
{
    Model m;
    m.regs[kBase] = kBootPllx;
    m.regs[kMisc3] = 0x8;  // PLLX_MISC_3 reset value: IDDQ
    m.regs[kMisc] = 0;     // PLLX_MISC reset value: lock detection off
    const uint32_t locked = run(m, kRung);
    EXPECT_EQ(locked, kRung | kEnable | kLock);
    const size_t iddq = m.indexOf("write", kMisc3, 0);
    const size_t wait = m.indexOf("delay", 0, 5);
    const size_t dividers = m.indexOf("write", kBase, kRung);
    const size_t enable = m.indexOf("write", kBase, kRung | kEnable);
    EXPECT_LT(iddq, wait);
    EXPECT_LT(wait, dividers);
    EXPECT_LT(dividers, enable);
    EXPECT_LT(enable, m.indexOf("wait", 0, 0));
    EXPECT_LT(m.indexOf("write", kMisc, kLockEnable), enable);
}

TEST(SwitchPllxStart, OutOfIddqAlreadyNoWaitAndTheSameOrder)
{
    Model m;
    m.regs[kBase] = kBootPllx;
    m.regs[kMisc] = kLockEnable;
    EXPECT_EQ(run(m, kRung | kEnable | kLock), kRung | kEnable | kLock);
    EXPECT_EQ(m.indexOf("delay", 0, 5), m.log.size());
    EXPECT_EQ(m.log.front().offset, kBase);
    EXPECT_EQ(m.log.front().value, kBootPllx & ~kEnable);
}

TEST(SwitchPllxStart, ALockThatDidNotComeIsReturnedForTheCaller)
{
    Model m;
    m.regs[kBase] = kBootPllx;
    m.lockAfterWait = false;
    EXPECT_EQ(run(m, kRung) & kLock, 0u);
}
}  // namespace

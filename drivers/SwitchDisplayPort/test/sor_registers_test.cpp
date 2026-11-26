// SPDX-License-Identifier: GPL-2.0-only
#include "drivers/SwitchDisplayPort/TegraSor1.h"
#include "drivers/SwitchDisplayPort/TegraDpaux1.h"
#include "drivers/SwitchUSB/test/xusb_register_model.h"

#include <gtest/gtest.h>

#include <ios>

namespace {
constexpr uintptr_t kSor = 0x54580000, kDpaux = 0x54040000, kCar = 0x60006000, kPmc = 0x7000e400;
constexpr uintptr_t kSuperState0 = kSor + 0x004, kSuperState1 = kSor + 0x008, kTest = kSor + 0x058;
constexpr uintptr_t kPll0 = kSor + 0x05c, kPll1 = kSor + 0x060, kPll2 = kSor + 0x064, kPll4 = kSor + 0x1c8;
constexpr uintptr_t kPadctl0 = kSor + 0x170, kDpSpare0 = kSor + 0x180, kPadRequest = kPmc + 0x1c0;
constexpr uintptr_t kPadctl2 = kSor + 0x1cc, kLaneSeq = kSor + 0x084, kTpg = kSor + 0x1b4;
constexpr uintptr_t kLinkCtl = kSor + 0x130, kLinkConfig = kSor + 0x160, kCrossbar = kSor + 0x128;
constexpr uintptr_t kDpauxPadctl = kDpaux + 0x124;
constexpr uintptr_t kSorClock = kCar + 0x410, kPllD2 = kCar + 0x4b8;
constexpr uintptr_t kPllDp = kCar + 0x590, kPllDpCfg = kCar + 0x598;
constexpr uintptr_t kClockOut = kCar + 0x280, kClockSet = kCar + 0x284, kClockClear = kCar + 0x288;
constexpr uint32_t kSorBit = 1u << 23, kOutMask = 3u << 14, kOutPad = 1u << 14;
constexpr uint32_t kPll0Boot = 0x7aaa755f, kPll1Boot = 0x48e0a055, kPll4Boot = 0xa5ffffff;
constexpr uint32_t kPadctl2Boot = 0xa5123456;

long firstMaskedWrite(uintptr_t address, uint32_t mask, uint32_t value, long after = -1)
{
    const auto &log = xusbModel().log;
    for (size_t i = static_cast<size_t>(after + 1); i < log.size(); ++i)
        if (log[i].kind == XusbEvent::Write && log[i].address == address && (log[i].value & mask) == value)
            return static_cast<long>(i);
    return -1;
}

bool hasDelay(uint32_t us, long after, long before)
{
    const auto &log = xusbModel().log;
    for (long i = after + 1; i < before; ++i)
        if (log[static_cast<size_t>(i)] == xusbDelay(us))
            return true;
    return false;
}

long maskedUpdate(uintptr_t address, uint32_t boot, uint32_t mask, uint32_t value)
{
    const auto &log = xusbModel().log;
    uint32_t previous = boot;
    for (size_t i = 0; i < log.size(); ++i) {
        const auto &event = log[i];
        if (event.kind != XusbEvent::Write || event.address != address)
            continue;
        if ((event.value & mask) == value) {
            EXPECT_EQ(event.value & ~mask, previous & ~mask);
            return static_cast<long>(i);
        }
        previous = event.value;
    }
    ADD_FAILURE() << "Missing masked update at " << std::hex << address;
    return -1;
}

class SorRegisters : public ::testing::Test {
protected:
    TegraSor1 sor;
    char why[160] = {};
    bool sscWhileEnabled = false, dpEnabled = false;

    void SetUp() override
    {
        xusbModelReset();
        auto &model = xusbModel();
        auto &r = model.registers;
        r[0x70000804] = 0x21u << 8 | 2u << 4;
        r[kPmc + 0x038] = 1u << 17;
        r[kSorClock] = 2u << 14;
        r[kPll0] = kPll0Boot;
        r[kPll1] = kPll1Boot;
        r[kPll2] = 0x01c60000;
        r[kPll4] = kPll4Boot;
        r[kPadctl0] = 1u << 23;
        r[kPadctl2] = kPadctl2Boot;
        model.afterWrite = [this](uintptr_t address, uint32_t value) {
            auto &registers = xusbModel().registers;
            if (address == kPllD2) {
                if (value & (1u << 30))
                    registers[address] |= 1u << 27;
                else
                    registers[address] &= ~(1u << 27);
            }
            if (address == kPllDp) {
                dpEnabled = (value & (1u << 30)) != 0;
                if (dpEnabled)
                    registers[address] |= 1u << 27;
                else
                    registers[address] &= ~(1u << 27);
            }
            if (address == kPllDpCfg)
                sscWhileEnabled |= dpEnabled;
            if (address == kSuperState0 && value == 1u) {
                const uint32_t ss1 = registers[kSuperState1];
                uint32_t test = 0;
                if (ss1 & (1u << 3))
                    test |= 1u << 10;
                test |= (ss1 & 3u) << 8;
                registers[kTest] = test;
            }
            if (address == kLaneSeq || address == kSor + 0x054)
                registers[address] &= ~(1u << 31);
            if (address == kClockSet)
                registers[kClockOut] |= value;
            if (address == kClockClear)
                registers[kClockOut] &= ~value;
            if (address == kCar + 0x29c)
                registers[kCar + 0x298] |= value;
            if (address == kCar + 0x2a0)
                registers[kCar + 0x298] &= ~value;
            if (address == kCar + 0x2a8)
                registers[kCar + 0x2a4] |= value;
            if (address == kCar + 0x2ac)
                registers[kCar + 0x2a4] &= ~value;
            if (address == kPmc + 0x1c0) {
                if ((value >> 30) == 1)
                    registers[kPmc + 0x1c4] &= ~(value & 0x3fffffffu);
                else if ((value >> 30) == 2)
                    registers[kPmc + 0x1c4] |= value & 0x3fffffffu;
            }
        };
    }

    bool start(uint8_t lanes = 2, uint8_t rate = SWITCH_DP_RATE_HBR)
    {
        const switch_dp_link link{rate, lanes};
        switch_dp_tu tu{};
        if (!sor.powerUp(why, sizeof(why)) || !sor.pixelClock(74250, why, sizeof(why)) ||
            !sor.startLink(link, true, why, sizeof(why)) ||
            !switch_dp_tu_config(74250, 1280, 1650, rate, lanes, true, &tu))
            return false;
        sor.enableLink(tu);
        return true;
    }

    void checkAuxClock(bool b01)
    {
        auto &r = xusbModel().registers;
        r[0x70000804] = 0x21u << 8 | (b01 ? 2u : 1u) << 4;
        for (const bool wasRunning : {false, true}) {
            SCOPED_TRACE(wasRunning);
            xusbModel().log.clear();
            const uint32_t initialBase = wasRunning ? 0x48181c01 : 1u << 18;
            const uint32_t initialCfg = wasRunning ? 0xc0000000u : 0;
            r[kPllDp] = initialBase;
            r[kPllDpCfg] = initialCfg;
            r[kCar + 0x770] = 0xa5a5a5a5;
            sscWhileEnabled = false;
            dpEnabled = wasRunning;
            TegraDpaux1 aux;
            ASSERT_TRUE(aux.powerUp(why, sizeof(why))) << why;
            EXPECT_EQ(r[kDpauxPadctl], b01 ? 0x23a2u : 0x24c2u);
            EXPECT_EQ(r[kPllDpCfg], 0x80000000u);
            const uint32_t profile = b01 ? 0x00282a01 : 0x00181c01;
            EXPECT_EQ(xusbWritesTo(kPllDp),
                      (std::vector<uint32_t>{initialBase & ~(1u << 30), profile | 1u << 18, profile,
                                             profile | 1u << 30}));
            EXPECT_EQ(r[kPllDp] & ~(1u << 27), profile | 1u << 30);
            EXPECT_EQ(r[kCar + 0x594], b01 ? 0x40000000u : 0x40000020u);
            EXPECT_EQ(r[kCar + 0x59c], b01 ? 0xf600f200u : 0xf400f0dau);
            EXPECT_EQ(r[kCar + 0x5a0], b01 ? 0x2005f600u : 0x2004f400u);
            if (b01)
                EXPECT_EQ(r[kCar + 0x770], 0u);
            const long stopped = firstMaskedWrite(kPllDpCfg, 0xffffffff, initialCfg & ~(1u << 30));
            const long disabled = firstMaskedWrite(kPllDp, 0xffffffff, initialBase & ~(1u << 30), stopped);
            const long iddq = firstMaskedWrite(kPllDp, 0xffffffff, profile | 1u << 18, disabled);
            const long defaults = firstMaskedWrite(kCar + 0x594, 0xffffffff,
                                                  b01 ? 0x40000000u : 0x40000020u, iddq);
            ASSERT_GE(stopped, 0);
            ASSERT_GE(disabled, 0);
            ASSERT_GE(iddq, 0);
            ASSERT_GE(defaults, 0);
            EXPECT_TRUE(hasDelay(1, stopped, disabled));
            EXPECT_TRUE(hasDelay(1, disabled, iddq));
            EXPECT_TRUE(hasDelay(2, iddq, defaults));
            const long spreadOff = firstMaskedWrite(kPllDpCfg, 0xffffffff, 0x80000000);
            const long configured = firstMaskedWrite(kPllDp, 0xffffffff, profile);
            const long enabled = firstMaskedWrite(kPllDp, 0xffffffff, profile | 1u << 30, configured);
            ASSERT_GE(configured, 0);
            ASSERT_GE(spreadOff, 0);
            ASSERT_GE(enabled, 0);
            EXPECT_LT(spreadOff, configured);
            EXPECT_TRUE(hasDelay(5, configured, enabled));

            xusbModel().log.clear();
            sscWhileEnabled = false;
            aux.setDownspread(false);
            EXPECT_TRUE(xusbWritesTo(kPllDp).empty());
            EXPECT_TRUE(xusbWritesTo(kPllDpCfg).empty());

            aux.setDownspread(true);
            EXPECT_FALSE(sscWhileEnabled);
            EXPECT_EQ(r[kPllDpCfg], 0xc0000000u);
            EXPECT_EQ(r[kPllDp] & (1u << 30 | 1u << 27), (1u << 30) | (1u << 27));
            EXPECT_EQ(xusbWritesTo(kPllDpCfg), (std::vector<uint32_t>{0xc0000000u}));
            const long dsDisable = firstMaskedWrite(kPllDp, 1u << 30, 0);
            const long dsCfg = firstMaskedWrite(kPllDpCfg, 0xffffffff, 0xc0000000u, dsDisable);
            const long dsEnable = firstMaskedWrite(kPllDp, 1u << 30, 1u << 30, dsCfg);
            ASSERT_GE(dsDisable, 0);
            ASSERT_GE(dsCfg, 0);
            ASSERT_GE(dsEnable, 0);

            xusbModel().log.clear();
            aux.setDownspread(true);
            EXPECT_TRUE(xusbWritesTo(kPllDp).empty());
            EXPECT_TRUE(xusbWritesTo(kPllDpCfg).empty());

            aux.setDownspread(false);
            EXPECT_FALSE(sscWhileEnabled);
            EXPECT_EQ(r[kPllDpCfg], 0x80000000u);
            EXPECT_EQ(r[kPllDp] & (1u << 30 | 1u << 27), (1u << 30) | (1u << 27));
            const uint32_t n = (r[kPllDp] >> 8) & 0xffu, m = r[kPllDp] & 0xffu;
            ASSERT_EQ(m, 1u);
            EXPECT_EQ((r[kPllDp] >> 19) & 0x1fu, b01 ? 5u : 3u);
            const int64_t effective = n * 8192 + 4096 + static_cast<int16_t>(r[kCar + 0x5a0]);
            EXPECT_EQ(38400000 * effective / (8192 * m * (b01 ? 6 : 4)), 270000000);
        }
    }
};

TEST_F(SorRegisters, StartsB01AuxClockFromOwnedDefaults)
{
    checkAuxClock(true);
}

TEST_F(SorRegisters, RetainsEristaAuxClockProfile)
{
    checkAuxClock(false);
}

TEST_F(SorRegisters, ProgramsTheB01PixelClockWithoutAFixedPostDivider)
{
    xusbModel().registers[kPllD2] = (1u << 30) | (1u << 18);
    ASSERT_TRUE(sor.powerUp(why, sizeof(why))) << why;
    xusbModel().log.clear();
    ASSERT_TRUE(sor.pixelClock(74250, why, sizeof(why))) << why;
    const auto &r = xusbModel().registers;
    const long disabled = firstMaskedWrite(kPllD2, 1u << 30, 0);
    const long iddqDivs = firstMaskedWrite(kPllD2, 0xffffffff, 0x00481701u | (1u << 18), disabled);
    const long misc0 = firstMaskedWrite(kCar + 0x4bc, 0xffffffff, 0x40000000u, iddqDivs);
    const long iddqClear = firstMaskedWrite(kPllD2, 0xffffffff, 0x00481701u, misc0);
    const long enable = firstMaskedWrite(kPllD2, 0xffffffff, 0x40481701u, iddqClear);
    ASSERT_GE(disabled, 0);
    ASSERT_GE(iddqDivs, 0);
    ASSERT_GE(misc0, 0);
    ASSERT_GE(iddqClear, 0);
    ASSERT_GE(enable, 0);
    EXPECT_TRUE(hasDelay(1, disabled, iddqDivs));
    EXPECT_TRUE(hasDelay(2, iddqDivs, misc0));
    EXPECT_TRUE(hasDelay(5, iddqClear, enable));
    EXPECT_EQ(r.at(kPllD2) & ~(1u << 27), 0x40481701u);
    EXPECT_EQ(r.at(kCar + 0x4bc), 0x40000000u);
    EXPECT_EQ(r.at(kCar + 0x570), 0x90000000u);
    EXPECT_EQ(r.at(kCar + 0x574), 0xf6e0f620u);
    EXPECT_EQ(r.at(kCar + 0x578), 0x0001f680u);
    EXPECT_EQ(r.at(kCar + 0x76c), 0u);
    const int64_t effective = ((r.at(kPllD2) >> 8) & 0xff) * 8192 + 4096 +
                              static_cast<int16_t>(r.at(kCar + 0x578));
    EXPECT_EQ(38400000 * effective / (8192 * 12), 74250000);

    sor.powerDown();
    EXPECT_EQ(r.at(kPllD2) & ((1u << 30) | (1u << 18)), 1u << 18);
}

TEST_F(SorRegisters, AppliesMaskedB01ProdOnTheSafeClock)
{
    ASSERT_TRUE(start()) << why;
    const long safe = firstMaskedWrite(kSorClock, kOutMask, 0);
    const long pad = firstMaskedWrite(kSorClock, kOutMask, kOutPad);
    ASSERT_GE(safe, 0);
    ASSERT_GE(pad, 0);
    for (const long prod : {
             maskedUpdate(kPll0, kPll0Boot, 0x000f0000, 0x00050000),
             maskedUpdate(kPll1, kPll1Boot, 0x00f00100, 0x00400100),
             maskedUpdate(kPll4, kPll4Boot, 0x00400000, 0),
             maskedUpdate(kPadctl2, kPadctl2Boot, 0xff000000, 0x34000000),
         }) {
        EXPECT_LT(safe, prod);
        EXPECT_LT(prod, pad);
    }
    const auto &r = xusbModel().registers;
    EXPECT_EQ(r.at(kPll0) & 0x000f0000u, 0x00050000u);
    EXPECT_EQ(r.at(kPll1) & 0x00f00100u, 0x00400100u);
    EXPECT_EQ(r.at(kPll4) & 0x00400000u, 0u);
    EXPECT_EQ(r.at(kPadctl2), 0x34123456u);
}

TEST_F(SorRegisters, PowersLanesBeforeSelectingThePadClockAndTps1)
{
    ASSERT_TRUE(start()) << why;
    EXPECT_EQ(xusbWritesTo(kLaneSeq), (std::vector<uint32_t>{0x8010f000}));
    const long up = firstMaskedWrite(kLaneSeq, 0xffffffff, 0x8010f000);
    const long pad = firstMaskedWrite(kSorClock, kOutMask, kOutPad);
    ASSERT_GE(up, 0);
    ASSERT_GE(pad, 0);
    EXPECT_LT(up, pad);

    switch_dp_train train{};
    switch_dp_train_out out{};
    out.pattern = 1;
    sor.drive(train, out);
    const long tps1 = firstMaskedWrite(kTpg, 0xffff, 0x4141);
    ASSERT_GE(tps1, 0);
    EXPECT_LT(pad, tps1);
    EXPECT_EQ(xusbModel().registers[kClockOut] & kSorBit, kSorBit);
}

TEST_F(SorRegisters, DrivesTheFourRequestedPostcursorLevels)
{
    ASSERT_TRUE(start(4, SWITCH_DP_RATE_HBR2)) << why;
    switch_dp_train train{};
    (void)switch_dp_train_start(&train, 4, true, 0x12, 0);
    const uint8_t status[11] = {0x11, 0x11, 0, 0, 0, 0, 0, 0, 0, 0, 0xe4};
    const auto next = switch_dp_train_status(&train, status);
    ASSERT_EQ(next.pattern, 3u);
    sor.drive(train, next);
    const auto &regs = xusbModel().registers;
    EXPECT_EQ(regs.at(kSor + 0x4e * 4), 0x11121213u);
    EXPECT_EQ(regs.at(kSor + 0x56 * 4), 0x05040200u);
    EXPECT_EQ(regs.at(kTpg), 0x43434343u);
}

TEST_F(SorRegisters, ConfiguresTransferUnitsBeforeEnablingThePort)
{
    const switch_dp_link link{SWITCH_DP_RATE_HBR, 2};
    ASSERT_TRUE(sor.powerUp(why, sizeof(why))) << why;
    ASSERT_TRUE(sor.pixelClock(74250, why, sizeof(why))) << why;
    ASSERT_TRUE(sor.startLink(link, true, why, sizeof(why))) << why;
    auto &r = xusbModel().registers;
    EXPECT_EQ(r[kLinkCtl] & 1u, 0u);
    EXPECT_EQ(r[kSorClock] & kOutMask, 0u);
    EXPECT_EQ(firstMaskedWrite(kLinkCtl, 1, 1), -1);
    EXPECT_TRUE(xusbWritesTo(kLinkConfig).empty());
    EXPECT_TRUE(xusbWritesTo(kCrossbar).empty());
    EXPECT_TRUE(xusbWritesTo(kTpg).empty());

    switch_dp_tu tu{};
    ASSERT_TRUE(switch_dp_tu_config(74250, 1280, 1650, link.rate, link.lanes, true, &tu));
    sor.enableLink(tu);
    const uint32_t expected = 0x94000000u | (tu.active_polarity ? 1u << 24 : 0) | tu.active_frac << 16 |
                              tu.active_count << 8 | tu.watermark;
    const long units = firstMaskedWrite(kLinkCtl, 0x1fc, tu.tu_size << 2);
    const long config = firstMaskedWrite(kLinkConfig, 0x950f7f3f, expected);
    const long enabled = firstMaskedWrite(kLinkCtl, 1, 1);
    const long crossbar = xusbFirstWrite(kCrossbar);
    const long padClock = firstMaskedWrite(kSorClock, kOutMask, kOutPad);
    for (const long event : {units, config, enabled, crossbar, padClock})
        ASSERT_GE(event, 0);
    EXPECT_LT(units, enabled);
    EXPECT_LT(config, enabled);
    EXPECT_LT(enabled, crossbar);
    EXPECT_LT(crossbar, padClock);
    EXPECT_TRUE(xusbWritesTo(kTpg).empty());
    EXPECT_TRUE(xusbWritesTo(kSor + 0x020).empty());
}

TEST_F(SorRegisters, LeavesABusySequencerUntouched)
{
    ASSERT_TRUE(sor.powerUp(why, sizeof(why))) << why;
    ASSERT_TRUE(sor.pixelClock(74250, why, sizeof(why))) << why;
    xusbModel().registers[kLaneSeq] = 1u << 28;
    const switch_dp_link link{SWITCH_DP_RATE_HBR, 2};
    EXPECT_FALSE(sor.startLink(link, true, why, sizeof(why)));
    EXPECT_GT(xusbModel().reads[kLaneSeq], 0u);
    EXPECT_TRUE(xusbWritesTo(kLaneSeq).empty());
    EXPECT_EQ(xusbModel().registers[kSorClock] & kOutMask, 0u);
    EXPECT_EQ(firstMaskedWrite(kLinkCtl, 1, 1), -1);
}

TEST_F(SorRegisters, PowersLanesDownOnceOnTheSafeClockBeforeStoppingThePll)
{
    ASSERT_TRUE(start()) << why;
    xusbModel().log.clear();
    sor.powerDown();
    sor.powerDown();
    EXPECT_EQ(xusbWritesTo(kLaneSeq), (std::vector<uint32_t>{0x8011f000}));
    const long safe = firstMaskedWrite(kSorClock, kOutMask, 0);
    const long down = firstMaskedWrite(kLaneSeq, 0xffffffff, 0x8011f000);
    const long pllOff = firstMaskedWrite(kPll0, 5, 5);
    for (const long event : {safe, down, pllOff})
        ASSERT_GE(event, 0);
    EXPECT_LT(safe, down);
    EXPECT_LT(down, pllOff);
    EXPECT_EQ(xusbModel().registers[kPadctl0] & 0xfu, 0u);
}

TEST_F(SorRegisters, CalibratesBeforeCyclingThePadsAndStartingThePll)
{
    ASSERT_TRUE(start()) << why;
    const long calibrationBandgap = firstMaskedWrite(kPll2, 1u << 22, 0);
    const long calibrationTerm = firstMaskedWrite(kPll1, 1u << 8, 1u << 8, calibrationBandgap);
    const long calibrationOn = firstMaskedWrite(kPadctl0, 1u << 23, 0, calibrationTerm);
    const long calibrationOff = firstMaskedWrite(kPadctl0, 1u << 23, 1u << 23, calibrationOn);
    const long bandgapOff = firstMaskedWrite(kPll2, 1u << 22, 1u << 22, calibrationOff);
    const long termOff = firstMaskedWrite(kPll1, 1u << 8, 0, calibrationOff);
    const long padOff = firstMaskedWrite(kPadRequest, 0xffffffff, 0x82000000, bandgapOff);
    const long padOn = firstMaskedWrite(kPadRequest, 0xffffffff, 0x42000000, padOff);
    const long initialPll2 = firstMaskedWrite(kPll2, 0x01c60000, 0x01c20000, calibrationOff);
    const long initialPll0 = firstMaskedWrite(kPll0, 5, 5, initialPll2);
    const long bandgapOn = firstMaskedWrite(kPll2, 0x01c60000, 0x01820000, initialPll0);
    const long pllOn = firstMaskedWrite(kPll0, 5, 0, bandgapOn);
    const long overrideOff = firstMaskedWrite(kPll2, 0x01c60000, 0x00820000, bandgapOn);
    const long portOn = firstMaskedWrite(kPll2, 0x01c60000, 0x00020000, overrideOff);
    const long termOn = firstMaskedWrite(kPll1, 1u << 8, 1u << 8, portOn);
    const long lanes = firstMaskedWrite(kLaneSeq, 0xffffffff, 0x8010f000, termOn);
    const long padClock = firstMaskedWrite(kSorClock, kOutMask, kOutPad, lanes);
    const long prechargeOn = firstMaskedWrite(kPadctl0, 0xf0, 0x30, padClock);
    const long prechargeOff = firstMaskedWrite(kPadctl0, 0xf0, 0, prechargeOn);
    for (const long event : {calibrationBandgap, calibrationTerm, calibrationOn, calibrationOff, bandgapOff, termOff,
                             padOff, padOn, initialPll2, initialPll0, bandgapOn, pllOn, overrideOff, portOn, termOn,
                             lanes, padClock, prechargeOn, prechargeOff})
        ASSERT_GE(event, 0);
    EXPECT_LT(termOff, padOff);
    EXPECT_LT(padOn, bandgapOn);
    EXPECT_LT(pllOn, portOn);
    EXPECT_TRUE(hasDelay(5, initialPll0, bandgapOn));
    EXPECT_TRUE(hasDelay(100, bandgapOn, pllOn));
    EXPECT_TRUE(hasDelay(200, pllOn, portOn));
    EXPECT_TRUE(hasDelay(200, overrideOff, portOn));
    EXPECT_TRUE(hasDelay(10, termOn, lanes));
    EXPECT_TRUE(hasDelay(20, prechargeOn, prechargeOff));

    std::vector<uint32_t> adjustments;
    const auto &log = xusbModel().log;
    for (long i = calibrationOn + 1; i < calibrationOff; ++i) {
        const auto &event = log[static_cast<size_t>(i)];
        if (event.kind == XusbEvent::Write && event.address == kPll1)
            adjustments.push_back(event.value & 0x1e00u);
    }
    EXPECT_EQ(adjustments, (std::vector<uint32_t>{0x1000, 0x800, 0x400, 0x200, 0}));
}

TEST_F(SorRegisters, UsesTheSameSequencerCommandForEachLaneCount)
{
    for (const uint8_t lanes : {1, 2, 4}) {
        SCOPED_TRACE(lanes);
        xusbModel().log.clear();
        ASSERT_TRUE(start(lanes)) << why;
        EXPECT_EQ(xusbWritesTo(kLaneSeq), (std::vector<uint32_t>{0x8010f000}));
        EXPECT_EQ(xusbModel().registers[kSor + 0x170] & 0xfu, (1u << lanes) - 1u);
        sor.powerDown();
    }
}

TEST_F(SorRegisters, RetainsBoardSetupOnRepeatedStart)
{
    ASSERT_TRUE(start(2, SWITCH_DP_RATE_HBR)) << why;
    EXPECT_EQ(xusbModel().registers.at(kPll1) & 0x00f00100u, 0x00400100u);
    EXPECT_EQ(xusbModel().registers.at(kPadctl0) & 0x0000ff00u, 0x00003000u);
    EXPECT_EQ(xusbModel().registers.at(kDpSpare0) & 0x7u, 0x5u);
    sor.powerDown();
    EXPECT_EQ(xusbModel().registers.at(kDpSpare0) & (1u << 2), 0u);
    ASSERT_TRUE(start(4, SWITCH_DP_RATE_HBR2)) << why;
    const auto &r = xusbModel().registers;
    EXPECT_EQ(r.at(kPll0) & 0x000f0000u, 0x00050000u);
    EXPECT_EQ(r.at(kPll1) & 0x00f00100u, 0x00400100u);
    EXPECT_EQ(r.at(kPadctl0) & 0x0000ff00u, 0x00002000u);
    EXPECT_EQ(r.at(kPll4) & 0x00400000u, 0u);
    EXPECT_EQ(r.at(kPadctl2), 0x34123456u);
    EXPECT_EQ(r.at(kDpSpare0) & 0x7u, 0x5u);
    sor.powerDown();
    ASSERT_TRUE(start(2, SWITCH_DP_RATE_RBR)) << why;
    EXPECT_EQ(r.at(kPll1) & 0x00f00100u, 0x00400100u);
    EXPECT_EQ(r.at(kPadctl0) & 0x0000ff00u, 0x00003000u);
    sor.powerDown();

    xusbModel().registers[0x70000804] = (0x21u << 8) | (1u << 4);
    ASSERT_TRUE(start(4, SWITCH_DP_RATE_HBR2)) << why;
    EXPECT_EQ(r.at(kPll1) & 0x00f00000u, 0x00600000u);
    sor.powerDown();
    ASSERT_TRUE(start(2, SWITCH_DP_RATE_RBR)) << why;
    EXPECT_EQ(r.at(kPll1) & 0x00f00000u, 0x00300000u);
}

TEST_F(SorRegisters, AttachAndWakeFollowTrmHandshakeOrder)
{
    ASSERT_TRUE(start(2, SWITCH_DP_RATE_HBR)) << why;
    xusbModel().log.clear();
    sor.detachHead();
    EXPECT_TRUE(xusbModel().log.empty());

    switch_dp_mode mode{};
    ASSERT_TRUE(switch_dp_cta_vic(4, &mode));
    switch_dp_tu tu{};
    ASSERT_TRUE(switch_dp_tu_config(mode.pixel_khz, mode.hactive, switch_dp_mode_htotal(&mode),
                                    SWITCH_DP_RATE_HBR, 2, true, &tu));
    xusbModel().log.clear();
    ASSERT_TRUE(sor.attach(mode, tu, why, sizeof(why))) << why;
    EXPECT_EQ(xusbWritesTo(kSor + 0x018), (std::vector<uint32_t>{0u}));
    EXPECT_LT(xusbFirstWrite(kSor + 0x018), xusbFirstWrite(kSor + 0x020));
    const uint32_t expectedHead1 =
        ((switch_dp_mode_vtotal(&mode) - 1u) << 16) | (switch_dp_mode_htotal(&mode) - 1u);
    EXPECT_EQ(xusbModel().registers.at(kSor + 0x020), expectedHead1);
    const long seq0 = firstMaskedWrite(kSor + 0x088, 0xffffffff, 0x0080a001u);
    const long seq8 = firstMaskedWrite(kSor + 0x0a8, 0xffffffff, 0x0080a001u);
    const long pwr = firstMaskedWrite(kSor + 0x054, (1u << 31) | 1u, (1u << 31) | 1u);
    ASSERT_GE(seq0, 0);
    ASSERT_GE(seq8, 0);
    ASSERT_GE(pwr, 0);
    EXPECT_LT(seq0, pwr);
    EXPECT_LT(seq8, pwr);
    EXPECT_EQ(xusbWritesTo(kSuperState1), (std::vector<uint32_t>{0x8u, 0xcu}));
    const long attachWrite = firstMaskedWrite(kSuperState1, 0xffffffff, 0x8u);
    const long normalWrite = firstMaskedWrite(kSuperState1, 0xffffffff, 0xcu, attachWrite);
    ASSERT_GE(attachWrite, 0);
    ASSERT_GE(normalWrite, 0);
    EXPECT_EQ(xusbModel().registers.at(kTest), 1u << 10);

    xusbModel().log.clear();
    ASSERT_TRUE(sor.wake(why, sizeof(why))) << why;
    EXPECT_EQ(xusbWritesTo(kSuperState1), (std::vector<uint32_t>{0xeu}));
    EXPECT_EQ(xusbModel().registers.at(kTest), (1u << 10) | (2u << 8));
}

TEST_F(SorRegisters, DpauxPowerUpBringsUpSorPartitionAndPulsesReset)
{
    auto &r = xusbModel().registers;
    r[kPmc + 0x038] = 0;  // SOR partition 17 initially off
    xusbModel().log.clear();
    TegraDpaux1 aux;
    ASSERT_TRUE(aux.powerUp(why, sizeof(why))) << why;
    EXPECT_NE(r[kPmc + 0x038] & (1u << 17), 0u);
    const long toggle = firstMaskedWrite(kPmc + 0x030, 0xffffffff, (1u << 8) | 17u);
    const long unclamp = firstMaskedWrite(kPmc + 0x034, 1u << 17, 1u << 17, toggle);
    const long pllEnable = firstMaskedWrite(kPllDp, 1u << 30, 1u << 30, unclamp);
    const long resetAssert = firstMaskedWrite(kCar + 0x2a8, 1u << 15, 1u << 15, pllEnable);
    const long clkEnable = firstMaskedWrite(kCar + 0x29c, 1u << 15, 1u << 15, resetAssert);
    const long resetDeassert = firstMaskedWrite(kCar + 0x2ac, 1u << 15, 1u << 15, clkEnable);
    const long padConfig = xusbFirstWrite(kDpauxPadctl);
    for (const long ev : {toggle, unclamp, pllEnable, resetAssert, clkEnable, resetDeassert, padConfig})
        ASSERT_GE(ev, 0);
    EXPECT_LT(toggle, unclamp);
    EXPECT_LT(unclamp, pllEnable);
    EXPECT_LT(pllEnable, resetAssert);
    EXPECT_LT(resetAssert, clkEnable);
    EXPECT_LT(clkEnable, resetDeassert);
    EXPECT_LT(resetDeassert, padConfig);
}

TEST_F(SorRegisters, PacksTheConfiguredCrossbarDriveBytes)
{
    ASSERT_TRUE(start(4)) << why;
    switch_dp_train train{};
    for (uint8_t lane = 0; lane < 4; ++lane)
        train.swing[lane] = lane;
    switch_dp_train_out out{};
    out.pattern = 1;
    sor.drive(train, out);
    const auto &r = xusbModel().registers;
    EXPECT_EQ(r.at(kSor + 0x138), 0x39281e13u);
    EXPECT_EQ(r.at(kSor + 0x148), 0x00010100u);
    EXPECT_EQ(r.at(kSor + 0x170) & 0x0040ff00u, 0x00406000u);
    EXPECT_EQ(r.at(kTpg), 0x41414141u);
    out.pattern = 0;
    sor.drive(train, out);
    EXPECT_EQ(r.at(kTpg), 0x50505050u);
}
}

extern "C" int tegra_i2c_write(unsigned, uint8_t, uint8_t, const void *, size_t)
{
    return 0;
}

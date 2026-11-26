// SPDX-License-Identifier: GPL-2.0-only
#include "../TegraDisplayB.h"
#include "../TegraDpaux1.h"
#include "../TegraSor1.h"
#include "drivers/SwitchUSB/test/xusb_register_model.h"
#include <gtest/gtest.h>
#include <tuple>

namespace {
constexpr uintptr_t dc = 0x54240000, kSor = 0x54580000, kDpaux = 0x54040000;
constexpr uintptr_t kCar = 0x60006000, kPmc = 0x7000e400;
constexpr uintptr_t control = dc + 4 * 0x41, header = dc + 4 * 0x42;
constexpr uintptr_t kStateAccess = dc + 4 * 0x40, kDispWinOptions = dc + 4 * 0x402;
constexpr uintptr_t kDisplayCmd = dc + 4 * 0x32, kDisplayPwr = dc + 4 * 0x36;
constexpr uint32_t kOptions = 0x700, kColorDepth = 0x703, kPosition = 0x704, kSize = 0x705, kPrescaled = 0x706;
constexpr uint32_t kHInit = 0x707, kVInit = 0x708, kDda = 0x709, kLineStride = 0x70a, kBlend = 0x716;
constexpr uint32_t kBlendMatch = 0x717, kBlendNomatch = 0x718, kBufferControl = 0x702, kStartAddrHiU = 0x80f;
constexpr uint32_t kStart = 0x800, kStartU = 0x802, kHOffset = 0x806, kVOffset = 0x808, kKind = 0x80b;
constexpr uint32_t kHFilter0 = 0x601, kCscYof = 0x611, kVFilter0 = 0x619;
constexpr uint32_t kMcHotResetCtrl = 0x200, kMcHotResetStatus = 0x204, kMcHotResetDcb = 1u << 3;
constexpr uint32_t kEnable = 1u << 30, kActA = 1u << 1, kActB = 1u << 2, kActC = 1u << 3;
std::vector<switch_display_memory_head> plans;
bool admit = true;
uint32_t mcCtrl, mcStatusReads;
std::vector<bool> mcHotResetEvents;

// DC B's window registers as banks behind DISPLAY_WINDOW_HEADER: a write goes to the selected window's assembly copy,
// a read of START_ADDR or WIN_OPTIONS returns its active copy, and a frame boundary latches what the request names.
struct Banks {
    unsigned selected = 0;
    std::vector<std::tuple<unsigned, uint32_t, uint32_t>> writes;
    std::map<uint32_t, uint32_t> assembly[3];
    uint32_t active[3][2] = {};

    void attach()
    {
        xusbModel().afterWrite = [this](uintptr_t address, uint32_t value) {
            wrote(address, value);
        };
    }

    ~Banks()
    {
        xusbModel().afterWrite = nullptr;
    }

    void wrote(uintptr_t address, uint32_t value)
    {
        if (address == header) {
            selected = (value >> 4) & 1 ? 0 : (value >> 5) & 1 ? 1 : 2;
        } else if (address >= dc + 4 * 0x500 && address < dc + 4 * 0x900) {
            const uint32_t word = uint32_t((address - dc) / 4);
            assembly[selected][word] = value;
            writes.emplace_back(selected, word, value);
        }
        expose();
    }

    void expose()
    {
        xusbModel().registers[dc + 4 * kStart] = active[selected][0];
        xusbModel().registers[dc + 4 * kOptions] = active[selected][1];
    }

    void boundary()
    {
        const uint32_t requests = xusbModel().registers[control] & 0xeu;
        for (unsigned w = 0; w < 3; ++w)
            if (requests & (1u << (1 + w))) {
                active[w][0] = assembly[w][kStart];
                active[w][1] = assembly[w][kOptions];
            }
        xusbModel().registers[control] = 0;
        expose();
    }

    std::vector<uint32_t> to(unsigned window, uint32_t word) const
    {
        std::vector<uint32_t> values;
        for (const auto &[w, at, value] : writes)
            if (w == window && at == word)
                values.push_back(value);
        return values;
    }

    long first(unsigned window, uint32_t word) const
    {
        for (size_t i = 0; i < writes.size(); ++i)
            if (std::get<0>(writes[i]) == window && std::get<1>(writes[i]) == word)
                return long(i);
        return -1;
    }
};

switch_external::WindowRegisters rgb(uint32_t start, uint32_t position, uint32_t width, uint32_t height, unsigned depth)
{
    switch_external::WindowRegisters r;
    r.on = true;
    r.options = kEnable;
    r.colorDepth = 12;
    r.position = position;
    r.size = (height << 16) | width;
    r.prescaled = (height << 16) | (width * 4);
    r.dda = 0x10001000;
    r.lineStride = width * 4;
    r.blend = 0x00ffff00u | depth;
    r.blendSelect = 0x262;
    r.start = start;
    return r;
}

switch_external::WindowRegisters yuv(uint32_t start, uint32_t position, uint32_t width, uint32_t height, unsigned depth,
                                     bool csc, int matrix)
{
    switch_external::WindowRegisters r;
    r.on = true;
    r.csc = csc;
    r.matrix = matrix;
    r.fullRange = 0;
    r.options = kEnable | (1u << 18);
    r.colorDepth = 43;
    r.position = position;
    r.size = (height << 16) | width;
    r.prescaled = (height << 16) | width;
    r.dda = 0x10001000;
    r.lineStride = (width << 16) | width;
    r.blend = (1u << 24) | depth;
    r.start = start;
    r.startU = start + width * height;
    for (uint32_t i = 0; i < 8; ++i)
        r.cscWords[i] = 0x100u + uint32_t(matrix) * 16u + i;
    return r;
}

switch_external::Frame clone(uint32_t ui, uint32_t video, bool csc, int matrix = 1)
{
    switch_external::Frame frame;
    frame.window[0] = rgb(ui, 440, 2560, 1440, 0);
    frame.window[1] = yuv(video, (480u << 16) | 1336u, 768, 480, 1, csc, matrix);
    frame.memory[0] = {319750000ULL * 4, 0, 0, 1};
    frame.memory[1] = {319750000ULL * 2, 0, 0, 1};
    frame.peakBytes = 319750000ULL * 6;
    return frame;
}

const switch_dp_mode kUltrawide = {319750, 3440, 48, 32, 80, 1440, 3, 10, 28, true, true, true};

struct Display {
    TegraDisplayB dcb;
    Banks banks;

    Display()
    {
        xusbModelReset();
        plans.clear();
        admit = true;
        banks.attach();
        char why[96] = {};
        EXPECT_TRUE(dcb.powerUp(why, sizeof(why))) << why;
        dcb.setMode(kUltrawide);
        banks.boundary();
        clear();
    }

    void clear()
    {
        xusbModel().log.clear();
        banks.writes.clear();
    }
};
}  // namespace

extern "C" int tegra_i2c_write(unsigned, uint8_t, uint8_t, const void *, size_t)
{
    return 0;
}

extern "C" void tegra_mc_hot_reset(uint32_t ctrl, uint32_t bit, bool on)
{
    if (ctrl == kMcHotResetCtrl && bit == kMcHotResetDcb) {
        if (on)
            mcCtrl |= bit;
        else
            mcCtrl &= ~bit;
        mcHotResetEvents.push_back(on);
    }
}

extern "C" uint32_t tegra_smmu_read(uint32_t offset)
{
    if (offset == kMcHotResetCtrl)
        return mcCtrl;
    if (offset == kMcHotResetStatus) {
        ++mcStatusReads;
        return mcCtrl;
    }
    return 0;
}

extern "C" int switch_emc_display_memory_reserve(const switch_display_memory_head *head)
{
    plans.push_back(*head);
    return admit ? 0 : -1;
}

TEST(DisplayB, SetModeTurnsEveryWindowOffAndWritesItsFilters)
{
    xusbModelReset();
    Banks banks;
    banks.attach();
    TegraDisplayB display;
    char why[96] = {};
    ASSERT_TRUE(display.powerUp(why, sizeof(why))) << why;
    xusbModel().log.clear();
    banks.writes.clear();
    display.setMode(kUltrawide);
    EXPECT_EQ(xusbWritesTo(control), (std::vector<uint32_t>{0xf00u, 0xfu}));
    EXPECT_EQ(xusbModel().registers.at(dc + 4 * 0x405), 1u);
    EXPECT_EQ(xusbModel().registers.at(dc + 4 * 0x40), 1u);
    for (unsigned w = 0; w < 3; ++w) {
        EXPECT_EQ(banks.to(w, kOptions), std::vector<uint32_t>{0u}) << w;
        EXPECT_LT(banks.first(w, kOptions), banks.first(w, kHFilter0)) << w;
        EXPECT_EQ(banks.assembly[w][kHFilter0], 0x00008000u);
        EXPECT_EQ(banks.assembly[w][kHFilter0 + 15], 0x3c7c08f1u);
        EXPECT_EQ(banks.assembly[w][kVFilter0], 128u);
        EXPECT_EQ(banks.assembly[w][kVFilter0 + 15], 8u);
        EXPECT_EQ(banks.assembly[w][kBufferControl], 0u);
        EXPECT_EQ(banks.assembly[w][kStartAddrHiU], 0u);
        EXPECT_FALSE(display.windowState(w).live);
        EXPECT_EQ(display.windowState(w).matrix, -1);
    }
}

// The UI and a video in one request: each window's registers through its header, the video's CSC while its window is
// off, one UPDATE write and one ACT_REQ write naming both windows.
TEST(DisplayB, PresentWritesEveryWindowInOneRequest)
{
    Display d;
    d.dcb.present(clone(0x10000000, 0x14400000, true));
    EXPECT_EQ(xusbWritesTo(control), (std::vector<uint32_t>{(kActA | kActB) << 8, kActA | kActB}));
    EXPECT_EQ(d.banks.to(0, kStart), std::vector<uint32_t>{0x10000000u});
    EXPECT_EQ(d.banks.to(0, kPosition), std::vector<uint32_t>{440u});
    EXPECT_EQ(d.banks.to(0, kBlend), std::vector<uint32_t>{0x00ffff00u});
    EXPECT_EQ(d.banks.to(0, kBlendMatch), std::vector<uint32_t>{0x262u});
    EXPECT_EQ(d.banks.to(0, kBlendNomatch), std::vector<uint32_t>{0x262u});
    EXPECT_TRUE(d.banks.to(0, kCscYof).empty());
    EXPECT_EQ(d.banks.to(1, kColorDepth), std::vector<uint32_t>{43u});
    EXPECT_EQ(d.banks.to(1, kStart), std::vector<uint32_t>{0x14400000u});
    EXPECT_EQ(d.banks.to(1, kStartU), std::vector<uint32_t>{0x14400000u + 768u * 480u});
    EXPECT_EQ(d.banks.to(1, kLineStride), std::vector<uint32_t>{(768u << 16) | 768u});
    EXPECT_EQ(d.banks.to(1, kBlend), std::vector<uint32_t>{(1u << 24) | 1u});
    for (uint32_t i = 0; i < 8; ++i)
        EXPECT_EQ(d.banks.to(1, kCscYof + i), std::vector<uint32_t>{0x110u + i});
    EXPECT_LT(d.banks.first(1, kCscYof), d.banks.first(1, kOptions));
    EXPECT_EQ(d.banks.to(1, kOptions), std::vector<uint32_t>{kEnable | (1u << 18)});
    EXPECT_TRUE(d.banks.to(2, kOptions).empty());
    for (unsigned w = 0; w < 2; ++w)
        EXPECT_GT(d.banks.first(w, kOptions), d.banks.first(w, kStart)) << w;
    const long update = xusbFirstWrite(control, (kActA | kActB) << 8);
    ASSERT_GE(update, 0);
    for (const auto &event : xusbModel().log)
        if (event.kind == XusbEvent::Write && event.address >= dc + 4 * 0x500 && event.address < dc + 4 * 0x900)
            EXPECT_LT(&event - &xusbModel().log[0], update);
    ASSERT_EQ(plans.size(), 1u);
    EXPECT_EQ(plans.back().head, 1u);
    EXPECT_EQ(plans.back().peak_bytes_per_second, 319750000ULL * 6);
    EXPECT_EQ(plans.back().windows[0].peak_bytes_per_second, 319750000ULL * 4);
    EXPECT_EQ(plans.back().windows[1].peak_bytes_per_second, 319750000ULL * 2);
    EXPECT_FALSE(plans.back().windows[2].active);
    EXPECT_TRUE(d.dcb.windowState(1).live);
    EXPECT_EQ(d.dcb.windowState(1).matrix, 1);
}

TEST(DisplayB, LatchWaitsForEveryWindowThenChecksEach)
{
    Display d;
    d.dcb.present(clone(0x10000000, 0x14400000, true));
    EXPECT_FALSE(d.dcb.latch());
    xusbModel().registers[control] = kActB;
    EXPECT_FALSE(d.dcb.latch());
    xusbModel().registers[control] = kActA | kActB;
    d.banks.boundary();
    d.clear();
    EXPECT_TRUE(d.dcb.latch());
    EXPECT_EQ(xusbWritesTo(header), (std::vector<uint32_t>{1u << 4, 1u << 5}));

    d.dcb.present(clone(0x10000000, 0x18800000, false));
    EXPECT_TRUE(d.banks.to(1, kCscYof).empty());
    xusbModel().registers[control] = kActA | kActB;
    d.banks.boundary();
    d.banks.active[1][0] = 0x14400000;
    EXPECT_ANY_THROW(d.dcb.latch());
}

// A window the next frame leaves goes off in the same request, and its latch is that it is off.
TEST(DisplayB, UnusedWindowsTurnOffInTheSameRequest)
{
    Display d;
    d.dcb.present(clone(0x10000000, 0x14400000, true));
    d.banks.boundary();
    ASSERT_TRUE(d.dcb.latch());
    d.clear();
    switch_external::Frame ui;
    ui.window[0] = rgb(0x10000000, 440, 2560, 1440, 0);
    ui.memory[0] = {319750000ULL * 4, 0, 0, 1};
    ui.peakBytes = 319750000ULL * 4;
    d.dcb.present(ui);
    EXPECT_EQ(xusbWritesTo(control), (std::vector<uint32_t>{(kActA | kActB) << 8, kActA | kActB}));
    EXPECT_EQ(d.banks.to(1, kOptions), std::vector<uint32_t>{0u});
    EXPECT_EQ(d.banks.to(0, kStart), std::vector<uint32_t>{0x10000000u});
    EXPECT_TRUE(d.banks.to(0, kPosition).empty());
    EXPECT_EQ(plans.back().peak_bytes_per_second, 319750000ULL * 6);
    xusbModel().registers[control] = kActA | kActB;
    d.banks.active[1][1] = kEnable;
    d.banks.expose();
    xusbModel().registers[control] = 0;
    EXPECT_ANY_THROW(d.dcb.latch());

    d.dcb.present(ui);
    d.banks.boundary();
    EXPECT_TRUE(d.dcb.latch());
    EXPECT_FALSE(d.dcb.windowState(1).live);
    EXPECT_EQ(plans.back().peak_bytes_per_second, 319750000ULL * 4);
    EXPECT_FALSE(plans.back().windows[1].active);
}

// The CSC is not shadowed: a frame that writes a scanning window's CSC panics before writing anything.
TEST(DisplayB, CscOfAScanningWindowIsNeverWritten)
{
    Display d;
    d.dcb.present(clone(0x10000000, 0x14400000, true));
    d.banks.boundary();
    ASSERT_TRUE(d.dcb.latch());
    d.clear();
    EXPECT_ANY_THROW(d.dcb.present(clone(0x10000000, 0x14400000, true, 2)));
    EXPECT_TRUE(d.banks.writes.empty());
    EXPECT_TRUE(xusbWritesTo(control).empty());

    switch_external::Frame moved = clone(0x10000000, 0x14400000, false);
    moved.window[2] = yuv(0x18800000, (480u << 16) | 1336u, 768, 480, 1, true, 2);
    moved.window[1] = {};
    d.dcb.present(moved);
    EXPECT_EQ(d.banks.to(2, kCscYof), std::vector<uint32_t>{0x120u});
    EXPECT_EQ(d.banks.to(1, kOptions), std::vector<uint32_t>{0u});
    EXPECT_EQ(xusbWritesTo(control), (std::vector<uint32_t>{(kActA | kActB | kActC) << 8, kActA | kActB | kActC}));
    d.banks.boundary();
    ASSERT_TRUE(d.dcb.latch());
    EXPECT_FALSE(d.dcb.windowState(1).live);
    EXPECT_EQ(d.dcb.windowState(1).matrix, 1);
    EXPECT_EQ(d.dcb.windowState(2).matrix, 2);
}

// Scaling and a source offset land in the window's registers; only what changed is written again.
TEST(DisplayB, ProgramsScalingAndWritesOnlyWhatChanged)
{
    Display d;
    switch_external::Frame frame;
    frame.window[0] = rgb(0x10000000, 440, 2560, 1440, 0);
    frame.window[0].prescaled = (720u << 16) | (1280u * 4u);
    frame.window[0].dda = (0x0800u << 16) | 0x0800u;
    frame.window[0].hInitial = 0x400;
    frame.window[0].vInitial = 0x400;
    frame.window[0].hOffset = 64;
    frame.window[0].vOffset = 22;
    frame.window[0].kind = 2u | (4u << 4);
    frame.window[0].options = kEnable | (1u << 8) | (1u << 10);
    d.dcb.present(frame);
    EXPECT_EQ(d.banks.to(0, kDda), std::vector<uint32_t>{(0x0800u << 16) | 0x0800u});
    EXPECT_EQ(d.banks.to(0, kHInit), std::vector<uint32_t>{0x400u});
    EXPECT_EQ(d.banks.to(0, kVInit), std::vector<uint32_t>{0x400u});
    EXPECT_EQ(d.banks.to(0, kPrescaled), std::vector<uint32_t>{(720u << 16) | (1280u * 4u)});
    EXPECT_EQ(d.banks.to(0, kHOffset), std::vector<uint32_t>{64u});
    EXPECT_EQ(d.banks.to(0, kVOffset), std::vector<uint32_t>{22u});
    EXPECT_EQ(d.banks.to(0, kKind), std::vector<uint32_t>{2u | (4u << 4)});
    EXPECT_EQ(d.banks.to(0, kOptions), std::vector<uint32_t>{kEnable | (1u << 8) | (1u << 10)});
    EXPECT_EQ(d.banks.to(0, kSize), std::vector<uint32_t>{(1440u << 16) | 2560u});
    d.banks.boundary();
    ASSERT_TRUE(d.dcb.latch());

    d.clear();
    frame.window[0].start = 0x14400000;
    d.dcb.present(frame);
    ASSERT_EQ(d.banks.writes.size(), 1u);
    EXPECT_EQ(d.banks.to(0, kStart), std::vector<uint32_t>{0x14400000u});
}

// The EMC is asked for what the windows scan during a change (the frame leaving and the one coming, each window's worst)
// and then for what is latched; a refusal leaves the frame on the screen and is not asked again.
TEST(DisplayB, MemoryFollowsTheLatchedWindows)
{
    Display d;
    admit = false;
    d.dcb.present(clone(0x10000000, 0x14400000, true));
    ASSERT_EQ(plans.size(), 1u);
    d.banks.boundary();
    ASSERT_TRUE(d.dcb.latch());
    EXPECT_EQ(plans.size(), 1u);
    admit = true;

    switch_external::Frame off;
    d.dcb.present(off);
    EXPECT_EQ(plans.size(), 1u);
    d.banks.boundary();
    ASSERT_TRUE(d.dcb.latch());
    ASSERT_EQ(plans.size(), 2u);
    EXPECT_EQ(plans.back().peak_bytes_per_second, 0u);
    EXPECT_FALSE(plans.back().windows[0].active);
    EXPECT_FALSE(plans.back().windows[1].active);
    EXPECT_FALSE(d.dcb.windowState(0).live);
}

TEST(DisplayB, TeardownUsesActiveWriteMuxAndFlushesMcHotResetBeforeReset)
{
    xusbModelReset();
    plans.clear();
    mcCtrl = kMcHotResetDcb;
    mcStatusReads = 0;
    mcHotResetEvents.clear();
    auto &model = xusbModel();
    model.afterWrite = [](uintptr_t addr, uint32_t value) {
        auto &r = xusbModel().registers;
        if (addr == kCar + 0x320)
            r[kCar + 0x010] |= value;
        else if (addr == kCar + 0x324)
            r[kCar + 0x010] &= ~value;
        else if (addr == kCar + 0x300)
            r[kCar + 0x004] |= value;
        else if (addr == kCar + 0x304)
            r[kCar + 0x004] &= ~value;
    };
    TegraDisplayB display;
    char why[96] = {};
    ASSERT_TRUE(display.powerUp(why, sizeof(why))) << why;
    EXPECT_EQ(mcCtrl, 0u);
    switch_dp_mode mode = {};
    ASSERT_TRUE(switch_dp_cta_vic(16, &mode));
    display.setMode(mode);

    model.log.clear();
    display.enableSor(true);
    EXPECT_EQ(xusbWritesTo(kStateAccess), (std::vector<uint32_t>{1u, 5u, 1u}));
    EXPECT_EQ(xusbWritesTo(kDispWinOptions), (std::vector<uint32_t>{1u << 26, 1u << 26}));
    EXPECT_LT(xusbFirstWrite(kStateAccess), xusbFirstWrite(kDispWinOptions));
    EXPECT_EQ(model.registers[kDispWinOptions], 1u << 26);

    model.log.clear();
    display.enableSor(false);
    EXPECT_EQ(xusbWritesTo(kStateAccess), (std::vector<uint32_t>{1u, 5u, 1u}));
    EXPECT_EQ(xusbWritesTo(kDispWinOptions), (std::vector<uint32_t>{0u, 0u}));
    const long activeMux = xusbFirstWrite(kStateAccess, 5u);
    const long clearSor = xusbFirstWrite(kDispWinOptions);
    EXPECT_GT(activeMux, clearSor);

    model.log.clear();
    mcStatusReads = 0;
    mcHotResetEvents.clear();
    display.powerDown();
    EXPECT_EQ(xusbWritesTo(kStateAccess), (std::vector<uint32_t>{5u, 1u}));
    EXPECT_EQ(xusbWritesTo(kDispWinOptions), (std::vector<uint32_t>{0u}));
    EXPECT_EQ(xusbWritesTo(kDisplayCmd), (std::vector<uint32_t>{0u}));
    EXPECT_EQ(xusbWritesTo(kDisplayPwr), (std::vector<uint32_t>{0u}));
    EXPECT_EQ(mcHotResetEvents, (std::vector<bool>{true}));
    EXPECT_EQ(mcCtrl, kMcHotResetDcb);
    EXPECT_GE(mcStatusReads, 5u);
    const long resetAssert = xusbFirstWrite(kCar + 0x300, 1u << 26);
    const long clockDisable = xusbFirstWrite(kCar + 0x324, 1u << 26);
    const long pmcOff = xusbFirstWrite(kPmc + 0x030);
    ASSERT_GE(resetAssert, 0);
    ASSERT_GE(clockDisable, 0);
    ASSERT_GE(pmcOff, 0);
    EXPECT_LT(resetAssert, clockDisable);
    EXPECT_LT(clockDisable, pmcOff);
    EXPECT_EQ(model.registers[kPmc + 0x038] & (1u << 19), 0u);
}

TEST(DisplayB, CombinedTeardownKeepsPllD2RunningThroughDcbPowerDown)
{
    xusbModelReset();
    plans.clear();
    admit = true;
    mcCtrl = 0;
    mcStatusReads = 0;
    mcHotResetEvents.clear();

    constexpr uintptr_t kPllD2 = kCar + 0x4b8, kPllDp = kCar + 0x590;
    constexpr uintptr_t kSuperState0 = kSor + 0x004, kSuperState1 = kSor + 0x008;
    constexpr uintptr_t kHeadState0 = kSor + 0x018, kHeadState1 = kSor + 0x020;
    constexpr uintptr_t kSorPwr = kSor + 0x054, kSorTest = kSor + 0x058, kLaneSeq = kSor + 0x084;

    auto &model = xusbModel();
    auto &r = model.registers;
    r[0x70000804] = (0x21u << 8) | (2u << 4);
    r[kPmc + 0x038] = 1u << 17;

    unsigned testReadsBeforeDetach = 0;
    bool sawDetachClear = false;
    bool dcbWriteAfterPllD2Off = false;

    model.afterWrite = [&](uintptr_t addr, uint32_t value) {
        auto &reg = xusbModel().registers;
        if (addr == kPllD2 || addr == kPllDp) {
            if (value & (1u << 30))
                reg[addr] |= 1u << 27;
            else
                reg[addr] &= ~(1u << 27);
        }
        if (addr >= dc && addr < dc + 0x40000 && (reg[kPllD2] & (1u << 30)) == 0u)
            dcbWriteAfterPllD2Off = true;
        if (addr == kSuperState1 && !sawDetachClear && (value & (1u << 3)) == 0u &&
            (reg[kSorTest] & (1u << 10)) != 0u) {
            sawDetachClear = true;
            testReadsBeforeDetach = xusbModel().reads[kSorTest];
        }
        if (addr == kSuperState0 && value == 1u) {
            const uint32_t ss1 = reg[kSuperState1];
            uint32_t test = 0;
            if (ss1 & (1u << 3))
                test |= 1u << 10;
            test |= (ss1 & 3u) << 8;
            reg[kSorTest] = test;
            if ((ss1 & (1u << 2)) == 0u)
                reg[kSorPwr] |= 1u << 28;
            else
                reg[kSorPwr] &= ~(1u << 28);
        }
        if (addr == kLaneSeq || addr == kSorPwr)
            reg[addr] &= ~(1u << 31);
        if (addr == kCar + 0x320)
            reg[kCar + 0x010] |= value;
        else if (addr == kCar + 0x324)
            reg[kCar + 0x010] &= ~value;
        else if (addr == kCar + 0x300)
            reg[kCar + 0x004] |= value;
        else if (addr == kCar + 0x304)
            reg[kCar + 0x004] &= ~value;
        else if (addr == kCar + 0x284)
            reg[kCar + 0x280] |= value;
        else if (addr == kCar + 0x288)
            reg[kCar + 0x280] &= ~value;
        else if (addr == kCar + 0x29c)
            reg[kCar + 0x298] |= value;
        else if (addr == kCar + 0x2a0)
            reg[kCar + 0x298] &= ~value;
        else if (addr == kCar + 0x2a8)
            reg[kCar + 0x2a4] |= value;
        else if (addr == kCar + 0x2ac)
            reg[kCar + 0x2a4] &= ~value;
        else if (addr == kPmc + 0x1c0) {
            if ((value >> 30) == 1)
                reg[kPmc + 0x1c4] &= ~(value & 0x3fffffffu);
            else if ((value >> 30) == 2)
                reg[kPmc + 0x1c4] |= value & 0x3fffffffu;
        }
    };

    TegraDpaux1 aux;
    TegraSor1 sor;
    TegraDisplayB dcb;
    char why[160] = {};
    ASSERT_TRUE(aux.powerUp(why, sizeof(why))) << why;
    switch_dp_mode mode = {};
    ASSERT_TRUE(switch_dp_cta_vic(16, &mode));
    const switch_dp_link link = {SWITCH_DP_RATE_HBR, 2};
    switch_dp_tu tu = {};
    ASSERT_TRUE(switch_dp_tu_config(mode.pixel_khz, mode.hactive, switch_dp_mode_htotal(&mode), link.rate, link.lanes,
                                    true, &tu));
    ASSERT_TRUE(sor.powerUp(why, sizeof(why))) << why;
    ASSERT_TRUE(sor.pixelClock(mode.pixel_khz, why, sizeof(why))) << why;
    ASSERT_TRUE(dcb.powerUp(why, sizeof(why))) << why;
    dcb.setMode(mode);
    ASSERT_TRUE(sor.startLink(link, true, why, sizeof(why))) << why;
    sor.enableLink(tu);

    model.log.clear();
    ASSERT_TRUE(sor.attach(mode, tu, why, sizeof(why))) << why;
    const long headState0Write = xusbFirstWrite(kHeadState0);
    const long headState1Write = xusbFirstWrite(kHeadState1);
    ASSERT_GE(headState0Write, 0);
    ASSERT_GE(headState1Write, 0);
    EXPECT_LT(headState0Write, headState1Write);
    EXPECT_EQ(r[kHeadState0], 0u);

    dcb.enableSor(true);
    ASSERT_TRUE(sor.wake(why, sizeof(why))) << why;

    model.log.clear();
    const unsigned testReadsAtStart = model.reads[kSorTest];
    dcbWriteAfterPllD2Off = false;

    dcb.windowsOff();
    sor.detachHead();
    dcb.enableSor(false);
    dcb.powerDown();
    EXPECT_NE(r[kPllD2] & (1u << 30), 0u);
    sor.powerDown();

    EXPECT_FALSE(dcbWriteAfterPllD2Off);
    EXPECT_TRUE(sawDetachClear);
    EXPECT_GT(testReadsBeforeDetach, testReadsAtStart);
    EXPECT_EQ(r[kPllD2] & (1u << 30), 0u);

    const long disp2Reset = xusbFirstWrite(kCar + 0x300, 1u << 26);
    const long disp2ClkOff = xusbFirstWrite(kCar + 0x324, 1u << 26);
    long pllD2Off = -1;
    for (size_t i = 0; i < model.log.size(); ++i) {
        if (model.log[i].kind == XusbEvent::Write && model.log[i].address == kPllD2 &&
            (model.log[i].value & (1u << 30)) == 0u) {
            pllD2Off = static_cast<long>(i);
            break;
        }
    }
    ASSERT_GE(disp2Reset, 0);
    ASSERT_GE(disp2ClkOff, 0);
    ASSERT_GE(pllD2Off, 0);
    EXPECT_LT(disp2Reset, pllD2Off);
    EXPECT_LT(disp2ClkOff, pllD2Off);

    aux.powerDown();
    model.log.clear();
    aux.powerDown();
    EXPECT_TRUE(model.log.empty());
    EXPECT_EQ(xusbWritesTo(kDpaux + 0x138).size(), 0u);
}

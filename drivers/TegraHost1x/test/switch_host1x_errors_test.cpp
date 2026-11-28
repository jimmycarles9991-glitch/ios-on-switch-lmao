// SPDX-License-Identifier: BSD-2-Clause-Patent
// host1x's general interrupt (SwitchHost1xErrors.cpp) on a sync-unit register model whose HINTSTATUS_EXT clears where
// a one is written (TRM 14.9.7): the attach and its masks, a stopped channel taken once and masked until its restart,
// its jobs failed by the action, and a debug kernel's expected error (the illegal-opcode probe) delivered or lost.
#include "switch_host1x_fence_stubs.h"
#include <gtest/gtest.h>
#include <map>
#include <string>
#include <vector>
#include "../SwitchHost1xErrors.cpp"

namespace {
constexpr uintptr_t kSync = 0x50002100;
std::map<uint32_t, uint32_t> gRegs;
bool gIntmaskStuck, gIntc0Stuck, gHintmaskExtStuck, gNoNub;
// A source on the line that no register here describes.
bool gUnknownSource;
// TRM 14.9.7 does not say whether HINTMASK_EXT gates HINTSTATUS bit 31 (review-dma3 O7): both are modelled.
bool gExtGatesSummary;
std::vector<uint32_t> gFailedChannels;

uint32_t &reg(uint32_t offset)
{
    return gRegs[offset];
}

bool anyLine(const std::string &text)
{
    for (const auto &line : switch_test_iolog)
        if (line.find(text) != std::string::npos)
            return true;
    return false;
}

IOFilterInterruptEventSource *source()
{
    return IOFilterInterruptEventSource::made().empty() ? nullptr : IOFilterInterruptEventSource::made().back();
}

// CPU0's line (TRM 14.9.2-14.9.8): INTMASK's CPU0 master, and either host1x's own interrupt (an unmasked
// HINTSTATUS_EXT bit, HINTMASK's EXT enable, INTC0MASK's HOST_INT) or a source this model does not know.
bool lineAsserted()
{
    const uint32_t ext = reg(kHintStatusExt) & (gExtGatesSummary ? reg(kHintMaskExt) : kIllegalAll);
    const bool host = ext && (reg(kHintMask) & kExtEnable) && (reg(kIntC0Mask) & kHostInt);
    return (reg(kIntMask) & kCpu0All) && (host || gUnknownSource);
}

// True when the line was asserted and its filter took it, then the action ran.
bool deliver()
{
    if (!lineAsserted() || !source()->filter(nullptr, source()))
        return false;
    source()->action(nullptr, source(), 1);
    return true;
}

// The channel's illegal opcode latches and the line delivers it if the masks let it.
bool fire(uint32_t channel)
{
    reg(kHintStatusExt) |= 1u << channel;
    return deliver();
}

struct Host1xErrors : testing::Test {
    void SetUp() override
    {
        gRegs = {{kIntMask, 0x10}};
        gIntmaskStuck = gIntc0Stuck = gHintmaskExtStuck = gNoNub = gUnknownSource = false;
        gExtGatesSummary = true;
        switch_test_preemption_off = 0;
        gFailedChannels.clear();
        switch_test_iolog.clear();
        switch_test_sleep = nullptr;
        gErrorsSource = nullptr;
    }

    static bool start()
    {
        switch_host1x_errors_start(IOWorkLoop::workLoop(), reinterpret_cast<volatile uint8_t *>(kSync));
        return source() && source()->enabled;
    }

    static std::string panicOf(bool (*run)())
    {
        try {
            run();
        } catch (const std::runtime_error &e) {
            return e.what();
        }
        return "no panic";
    }
};
}  // namespace

extern "C" {
uint32_t switch_mmio_model_read32(uintptr_t address)
{
    return gRegs[static_cast<uint32_t>(address - kSync)];
}

void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
    const auto offset = static_cast<uint32_t>(address - kSync);
    if (offset == kHintStatusExt)
        gRegs[offset] &= ~value;
    else if (offset == kIntC0Mask)
        gRegs[offset] = gIntc0Stuck ? 0 : value;
    else if (offset == kHintMaskExt)
        gRegs[offset] = gHintmaskExtStuck ? value & 0xff : value;
    else if (offset != kIntMask || !gIntmaskStuck)
        gRegs[offset] = value;
}

void tegra_host1x_describe_channel(uint32_t channel, char *buffer, size_t size)
{
    snprintf(buffer, size, "ch%u DMAGET 0x40", channel);
}
}

IOService *switch_interrupt_nub(uint32_t, bool)
{
    static IOService nub;
    return gNoNub ? nullptr : &nub;
}

void switch_host1x_channels_failed(uint32_t channel, char *line, size_t size)
{
    gFailedChannels.push_back(channel);
    snprintf(line, size, "syncpoint %u: 1 failed", 160 + channel);
}

TEST_F(Host1xErrors, TheAttachUnmasksEveryChannelSetsIntmaskToCpu0AloneAndClearsWhatLatched)
{
    reg(kHintStatusExt) = 1u << 2;
    ASSERT_TRUE(start());
    EXPECT_TRUE(source()->enabled);
    EXPECT_EQ(reg(kIntMask), 1u) << "CPU0's master enable alone: bit 4 (TrustZone violations, reset 1) off, as L4T";
    EXPECT_EQ(reg(kIntC0Mask), 1u) << "HOST_INT reaches CPU0";
    EXPECT_EQ(reg(kHintMask), 1u << 31);
    EXPECT_EQ(reg(kHintMaskExt), 0x3fffu);
    EXPECT_EQ(reg(kHintStatusExt), 0u);
    EXPECT_TRUE(anyLine("latched before the general interrupt was on: channels 0x4")) << switch_test_iolog.size();
    EXPECT_TRUE(gFailedChannels.empty()) << "what latched before the channels were programmed fails nothing";
}

TEST_F(Host1xErrors, AnIntmaskThatDoesNotTakePanics)
{
    gIntmaskStuck = true;
    EXPECT_EQ(panicOf(&start), "TegraHost1x: general interrupt (INTID 99) not enabled: INTMASK reads 0x10 after CPU0's "
                               "enable was written");
    EXPECT_FALSE(source()->enabled);
}

TEST_F(Host1xErrors, AnIntc0maskThatDoesNotTakeHostIntPanics)
{
    gIntc0Stuck = true;
    EXPECT_EQ(panicOf(&start),
              "TegraHost1x: general interrupt (INTID 99) not enabled: INTC0MASK reads 0 after HOST_INT "
              "was written");
}

// A channel whose bit does not stick would stop without a word: as loud as the other masks.
TEST_F(Host1xErrors, AnIllegalOpcodeMaskThatDoesNotStickPanics)
{
    gHintmaskExtStuck = true;
    EXPECT_EQ(panicOf(&start), "TegraHost1x: general interrupt (INTID 99) not enabled: HINTMASK_EXT reads 0xff after "
                               "0x3fff was written");
}

TEST_F(Host1xErrors, NoInterruptNubPanics)
{
    gNoNub = true;
    EXPECT_EQ(panicOf(&start), "TegraHost1x: general interrupt (INTID 99) could not be attached: no interrupt nub");
}

TEST_F(Host1xErrors, AnIntmaskThatDoesNotTakeAfterSleepPanics)
{
    ASSERT_TRUE(start());
    switch_host1x_errors_stop();
    gRegs = {{kIntMask, 0x10}};
    gIntmaskStuck = true;
    EXPECT_EQ(panicOf([] {
                  switch_host1x_errors_resume();
                  return true;
              }),
              "TegraHost1x: general interrupt (INTID 99) not enabled: INTMASK reads 0x10 after CPU0's enable was "
              "written");
    EXPECT_FALSE(source()->enabled);
}

// The channel stays stopped, so its bit may latch again: taken once, masked until the channel's restart rearms it.
TEST_F(Host1xErrors, AStoppedChannelIsTakenOnceItsJobsFailAndItsRestartUnmasksIt)
{
    ASSERT_TRUE(start());
    reg(kCmdprocStat) = 1u << 1;
    ASSERT_TRUE(fire(1));
    EXPECT_EQ(gFailedChannels, std::vector<uint32_t>{1});
    EXPECT_EQ(reg(kHintMaskExt), 0x3fffu & ~(1u << 1));
    EXPECT_EQ(reg(kHintStatusExt), 0u);
    EXPECT_TRUE(anyLine("channel 1 stopped on an illegal opcode (CMDPROC_STAT 0x2): ch1 DMAGET 0x40; its jobs fail now "
                        "(syncpoint 161: 1 failed)"));
    reg(kHintStatusExt) |= 1u << 1;  // latched again while stopped: masked, so it does not fire the line by itself
    EXPECT_TRUE(fire(2)) << "another channel's stop";
    EXPECT_EQ(gFailedChannels, (std::vector<uint32_t>{1, 2})) << "channel 1 is not taken twice";
    switch_host1x_errors_rearm(1);
    switch_host1x_errors_rearm(2);
    EXPECT_EQ(reg(kHintMaskExt), 0x3fffu);
    EXPECT_EQ(reg(kHintStatusExt), 0u) << "what latched meanwhile is cleared";
    EXPECT_TRUE(fire(1)) << "a later stop is taken again";
    EXPECT_EQ(gFailedChannels.size(), 3u);
}

// Whatever the boot chain left in INTC0MASK and HINTMASK, only the illegal opcodes reach the line: the engines' bits go
// (each engine has its own GIC line), host1x's own HOST_INT stays.
TEST_F(Host1xErrors, TheAttachLeavesOnlyTheIllegalOpcodesOnTheLine)
{
    reg(kHintMask) = 1u << 16 | 1u << 29;
    reg(kIntC0Mask) = 0x2004;
    ASSERT_TRUE(start());
    EXPECT_EQ(reg(kHintMask), 1u << 31);
    EXPECT_EQ(reg(kIntC0Mask), 1u);
    EXPECT_TRUE(anyLine("INTC0MASK read 0x2004: the engines' bits cleared (each has its own line), HOST_INT kept"));
}

TEST_F(Host1xErrors, AnIntc0maskWithOnlyHostIntSaysNothing)
{
    reg(kIntC0Mask) = 1;
    ASSERT_TRUE(start());
    EXPECT_FALSE(anyLine("INTC0MASK"));
}

// A fire with none of ours pending is a source this driver set up wrong. Turning the line off would lose every later
// stop, so it panics, naming every mask, and the master enable stays on.
TEST_F(Host1xErrors, ALineThatFiresWithNothingOfOursPanics)
{
    ASSERT_TRUE(start());
    reg(kHintStatus) = 1u << 16;
    EXPECT_FALSE(deliver()) << "HINTMASK keeps host1x's other sources off the line";
    gUnknownSource = true;
    EXPECT_EQ(panicOf(&deliver),
              "TegraHost1x: general interrupt (INTID 99) fired with no illegal opcode of ours pending "
              "(INTSTATUS 0 HINTSTATUS 0x10000 HINTSTATUS_EXT 0 HINTMASK_EXT 0x3fff INTMASK 0x1)");
    EXPECT_EQ(reg(kIntMask) & 1u, 1u) << "the master enable is never turned off";
    gErrorsLock->held = false;
}

// O7's other answer: if HINTMASK_EXT does not gate the summary, a stopped channel that latches again fires the line
// with only its masked bit pending, and the panic says which.
TEST_F(Host1xErrors, AStoppedChannelThatFiresAnUngatedLineAgainPanicsNamingIt)
{
    gExtGatesSummary = false;
    ASSERT_TRUE(start());
    ASSERT_TRUE(fire(1));
    EXPECT_EQ(gFailedChannels, std::vector<uint32_t>{1});
    reg(kHintStatusExt) |= 1u << 1;
    EXPECT_EQ(panicOf(&deliver),
              "TegraHost1x: general interrupt (INTID 99) fired for channels 0x2, masked since their "
              "stop (HINTSTATUS_EXT 0x2 HINTMASK_EXT 0x3ffd HINTSTATUS 0): HINTMASK_EXT does not keep "
              "a stopped channel off the line");
    gErrorsLock->held = false;
}

TEST_F(Host1xErrors, ARestartOfAChannelThatDidNotStopChangesNothing)
{
    ASSERT_TRUE(start());
    reg(kHintStatusExt) = 0;
    switch_host1x_errors_rearm(3);
    EXPECT_EQ(reg(kHintMaskExt), 0x3fffu);
}

TEST_F(Host1xErrors, SleepTurnsItOffAndTheResumeConfiguresItAgain)
{
    ASSERT_TRUE(start());
    switch_host1x_errors_stop();
    EXPECT_FALSE(source()->enabled);
    gRegs = {{kIntMask, 0x10}};  // SC7 reset the sync unit
    switch_host1x_errors_resume();
    EXPECT_TRUE(source()->enabled);
    EXPECT_EQ(reg(kHintMaskExt), 0x3fffu);
    EXPECT_EQ(reg(kIntC0Mask), 1u);
    EXPECT_EQ(reg(kIntMask), 1u);
    EXPECT_TRUE(fire(5)) << "delivered after the resume";
}

// The illegal-opcode probe's error: taken by the filter, delivered to the probe, no job failed, no line.
TEST_F(Host1xErrors, AnExpectedErrorWakesTheProbeAndFailsNothing)
{
    ASSERT_TRUE(start());
    switch_host1x_errors_expect(13);
    static int sleeps;
    sleeps = 0;
    switch_test_sleep = [](uint64_t) {
        if (sleeps++)
            return THREAD_TIMED_OUT;
        EXPECT_TRUE(fire(13));
        return THREAD_AWAKENED;
    };
    EXPECT_EQ(switch_host1x_errors_expect_end(100), 1u);
    EXPECT_TRUE(gFailedChannels.empty());
    EXPECT_FALSE(anyLine("stopped on an illegal opcode"));
    switch_test_sleep = nullptr;
    reg(kCmdprocStat) = 0;
    switch_host1x_errors_rearm(13);
    EXPECT_TRUE(fire(13));
    EXPECT_EQ(gFailedChannels, std::vector<uint32_t>{13}) << "the expectation ended with the probe";
}

TEST_F(Host1xErrors, AnErrorThatLatchedAndWasNeverDeliveredPanicsAsLost)
{
    ASSERT_TRUE(start());
    switch_host1x_errors_expect(13);
    reg(kCmdprocStat) = 1u << 13;
    switch_test_sleep = [](uint64_t) {
        return THREAD_TIMED_OUT;
    };
    try {
        switch_host1x_errors_expect_end(100);
        ADD_FAILURE() << "no panic";
    } catch (const std::runtime_error &e) {
        EXPECT_EQ(std::string(e.what()), "TegraHost1x: general interrupt (INTID 99) lost: channel 13's illegal opcode "
                                         "latched (HINTSTATUS_EXT 0 CMDPROC_STAT 0x2000) and not delivered in 100 ms");
    }
    gExpectLock->held = false;
}

TEST_F(Host1xErrors, AnOpcodeThatIsNotIllegalLatchesNothingAndIsNoPanic)
{
    ASSERT_TRUE(start());
    switch_host1x_errors_expect(13);
    switch_test_sleep = [](uint64_t) {
        return THREAD_TIMED_OUT;
    };
    EXPECT_EQ(switch_host1x_errors_expect_end(100), 0u);
}

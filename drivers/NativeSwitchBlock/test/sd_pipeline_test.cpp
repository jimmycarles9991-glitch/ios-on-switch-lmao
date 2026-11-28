// Host test of drivers/NativeSwitchBlock/SwitchSdPipeline.h and SwitchAdma.h PA-range disjointness.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

typedef uint64_t IOByteCount;
constexpr uint32_t kIOMemoryMapperNone = 0x800;

#include "../SwitchAdma.h"
#include "../SwitchSdPipeline.h"

namespace {
constexpr uint32_t kIntErrors = 0xffff8000u;
constexpr uint32_t kIntAdmaError = 1u << 25;
constexpr uint32_t kR1Errors = kSdR1Errors;
constexpr uint32_t kR1StopErrors = 0x7df90008u;

struct TestSlot : SwitchSdPipeSlot {
    int id = 0;
};

SwitchSdIrqSample cleanSample(uint64_t nowNs = 1000)
{
    SwitchSdIrqSample s = {};
    s.status = kSdCommandComplete | kSdTransferComplete;
    s.present = kSdPresentDat0;
    s.nowNs = nowNs;
    return s;
}

TEST(SdPipeline, FastIssuesPreparedSlotAndRotatesToDone)
{
    SwitchSdPipeline<TestSlot> pipe;
    SwitchSdBus bus;
    TestSlot s0, s1;
    s0.id = 0;
    s0.lba = 100;
    s0.sectors = 8;
    s0.direct = true;
    s0.cmd23 = true;
    s1.id = 1;
    s1.lba = 200;
    s1.sectors = 16;
    s1.write = true;
    s1.direct = true;
    s1.cmd23 = true;

    ASSERT_EQ(bus.claim(kSdPresentDat0, SwitchSdUse::kRead), SwitchSdBusState::kFree);
    pipe.active = &s0;
    pipe.prepared = &s1;

    int emittedId = -1;
    const SwitchSdIrqAction action = pipe.onIrq(bus, cleanSample(5000), kIntErrors, kR1Errors, kR1StopErrors,
                                                kIntAdmaError, [&](const TestSlot &slot) {
                                                    emittedId = slot.id;
                                                });
    EXPECT_EQ(action, SwitchSdIrqAction::kFastIssued);
    EXPECT_EQ(emittedId, 1);
    EXPECT_EQ(pipe.done, &s0);
    EXPECT_EQ(pipe.active, &s1);
    EXPECT_EQ(pipe.prepared, nullptr);
    EXPECT_TRUE(s1.fastIssued);
    EXPECT_EQ(s1.issuedAt, 5000u);
    EXPECT_TRUE(bus.card);  // s1 is a write

    EXPECT_EQ(pipe.takeDone(), &s0);
    EXPECT_EQ(pipe.done, nullptr);
    EXPECT_TRUE(pipe.shouldSleep());
}

TEST(SdPipeline, CleanIdleWhenNoPrepared)
{
    SwitchSdPipeline<TestSlot> pipe;
    SwitchSdBus bus;
    TestSlot s0;
    s0.lba = 100;
    s0.sectors = 8;
    s0.direct = true;
    ASSERT_EQ(bus.claim(kSdPresentDat0, SwitchSdUse::kRead), SwitchSdBusState::kFree);
    pipe.active = &s0;

    bool emitted = false;
    const SwitchSdIrqAction action =
        pipe.onIrq(bus, cleanSample(2000), kIntErrors, kR1Errors, kR1StopErrors, kIntAdmaError, [&](const TestSlot &) {
            emitted = true;
        });
    EXPECT_EQ(action, SwitchSdIrqAction::kDoneIdle);
    EXPECT_FALSE(emitted);
    EXPECT_EQ(pipe.done, &s0);
    EXPECT_EQ(pipe.active, nullptr);
    EXPECT_TRUE(bus.idle());
    EXPECT_FALSE(pipe.shouldSleep());
}

TEST(SdPipeline, DefersWhenDoneStillPendingOrDat0BusyOrError)
{
    // Case 1: done != nullptr -> slow wake without mutating bus or issuing prepared.
    {
        SwitchSdPipeline<TestSlot> pipe;
        SwitchSdBus bus;
        TestSlot s0, s1;
        s1.lba = 200;
        s1.sectors = 8;
        s1.direct = true;
        ASSERT_EQ(bus.claim(kSdPresentDat0, SwitchSdUse::kRead), SwitchSdBusState::kFree);
        pipe.done = &s0;
        pipe.active = &s1;
        const SwitchSdIrqAction action = pipe.onIrq(bus, cleanSample(3000), kIntErrors, kR1Errors, kR1StopErrors,
                                                    kIntAdmaError, [](const TestSlot &) {});
        EXPECT_EQ(action, SwitchSdIrqAction::kSlowWake);
        EXPECT_TRUE(s1.irqDone);
        EXPECT_FALSE(bus.idle());  // untouched
        EXPECT_EQ(pipe.done, &s0);
    }

    // Case 2: write completed but DAT0 still low -> slow wake, prepared NOT issued.
    {
        SwitchSdPipeline<TestSlot> pipe;
        SwitchSdBus bus;
        TestSlot s0, s1;
        s0.write = true;
        s0.direct = true;
        s0.sectors = 8;
        s1.direct = true;
        s1.sectors = 8;
        ASSERT_EQ(bus.claim(kSdPresentDat0, SwitchSdUse::kWrite), SwitchSdBusState::kFree);
        pipe.active = &s0;
        pipe.prepared = &s1;
        SwitchSdIrqSample busy = cleanSample(4000);
        busy.present = 0;  // DAT0 low
        bool emitted = false;
        const SwitchSdIrqAction action =
            pipe.onIrq(bus, busy, kIntErrors, kR1Errors, kR1StopErrors, kIntAdmaError, [&](const TestSlot &) {
                emitted = true;
            });
        EXPECT_EQ(action, SwitchSdIrqAction::kSlowWake);
        EXPECT_FALSE(emitted);
        EXPECT_TRUE(s0.irqDone);
        EXPECT_EQ(pipe.prepared, &s1);
        EXPECT_EQ(pipe.retractPrepared(), &s1);
        EXPECT_EQ(pipe.prepared, nullptr);
    }

    // Case 3: ADMA error saves ADMA state/address and never issues prepared.
    {
        SwitchSdPipeline<TestSlot> pipe;
        SwitchSdBus bus;
        TestSlot s0, s1;
        s0.direct = true;
        s0.sectors = 8;
        s1.direct = true;
        s1.sectors = 8;
        ASSERT_EQ(bus.claim(kSdPresentDat0, SwitchSdUse::kRead), SwitchSdBusState::kFree);
        pipe.active = &s0;
        pipe.prepared = &s1;
        SwitchSdIrqSample err = cleanSample(6000);
        err.status = (1u << 15) | kIntAdmaError;
        err.admaError = 0x05;
        err.admaAddress = 0x80001230ULL;
        const SwitchSdIrqAction action =
            pipe.onIrq(bus, err, kIntErrors, kR1Errors, kR1StopErrors, kIntAdmaError, [](const TestSlot &) {});
        EXPECT_EQ(action, SwitchSdIrqAction::kSlowWake);
        EXPECT_TRUE(s0.irqDone);
        EXPECT_EQ(s0.irqAdmaState, 0x05u);
        EXPECT_EQ(s0.irqAdmaAddress, 0x80001230ULL);
        EXPECT_EQ(pipe.prepared, &s1);
    }
}

TEST(SdPipeline, TimeoutVsIrqRaceIsSafe)
{
    SwitchSdPipeline<TestSlot> pipe;
    TestSlot s0, s1;
    s0.issuedAt = 1000;
    pipe.active = &s0;
    // Before limit: false.
    EXPECT_FALSE(pipe.noteTimeout(&s0, 0, 1500, 2000));
    EXPECT_FALSE(s0.irqDone);
    // If ISR rotated active to s1 before timeout check ran: stale expected -> false.
    pipe.active = &s1;
    EXPECT_FALSE(pipe.noteTimeout(&s0, 0, 5000, 2000));
    EXPECT_FALSE(s1.irqDone);
    // Real timeout on active s1.
    s1.issuedAt = 2000;
    EXPECT_TRUE(pipe.noteTimeout(&s1, 0x01, 4500, 2000));
    EXPECT_TRUE(s1.irqDone);
    EXPECT_TRUE(s1.timedOut);
    EXPECT_EQ(s1.irqStatus, 0x01u);
}

TEST(SdPipeline, OverlapAndAdmaPaDisjointness)
{
    TestSlot a, b;
    a.lba = 100;
    a.sectors = 8;
    b.lba = 108;
    b.sectors = 8;
    EXPECT_TRUE(SwitchSdPipeline<TestSlot>::canOverlap(a, b, false));
    // PA overlap rejects even when LBAs are disjoint.
    EXPECT_FALSE(SwitchSdPipeline<TestSlot>::canOverlap(a, b, true));
    // Staged bounce in both rejects.
    a.staged = true;
    b.staged = true;
    EXPECT_FALSE(SwitchSdPipeline<TestSlot>::canOverlap(a, b, false));
    b.staged = false;
    EXPECT_TRUE(SwitchSdPipeline<TestSlot>::canOverlap(a, b, false));
    // LBA overlap rejects.
    b.lba = 104;
    EXPECT_FALSE(SwitchSdPipeline<TestSlot>::canOverlap(a, b, false));

    // SwitchAdmaTable PA overlap check excludes the gap sink.
    std::vector<SwitchAdmaDescriptor> ea(8), eb(8);
    SwitchAdmaTable ta = {ea.data(), 8, kAdmaDmaLimit, 0, 0, 0, 0};
    SwitchAdmaTable tb = {eb.data(), 8, kAdmaDmaLimit, 0, 0, 0, 0};
    constexpr uint64_t kSink = 0x90000000ULL;
    constexpr uint64_t kSinkBytes = 32 * 1024;
    auto ok = [](uint64_t) {
        return true;
    };
    ASSERT_EQ(ta.add(0x80001000ULL, 4096, ok), kAdmaOk);
    ASSERT_EQ(ta.addSealed(kSink, 4096), kAdmaOk);
    ASSERT_EQ(tb.add(0x80002000ULL, 4096, ok), kAdmaOk);
    ASSERT_EQ(tb.addSealed(kSink, 4096), kAdmaOk);
    // Disjoint client buffers sharing the sink -> disjoint when sink skipped, overlapping if not skipped.
    EXPECT_FALSE(switchAdmaTablesOverlap(ta, tb, kSink, kSinkBytes));
    EXPECT_TRUE(switchAdmaTablesOverlap(ta, tb, 0, 0));
    // Same physical page in both tables -> overlapping.
    ASSERT_EQ(tb.add(0x80001800ULL, 512, ok), kAdmaOk);
    EXPECT_TRUE(switchAdmaTablesOverlap(ta, tb, kSink, kSinkBytes));
}
}  // namespace

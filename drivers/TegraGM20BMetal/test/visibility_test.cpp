// SPDX-License-Identifier: MIT
// visibility.h: which visibility results the driver writes, and the methods an encoder's mode sets and
// end emit (the sample counter per mode set, its report into a slot, the copy engine's copy to the offset).
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "drivers/TegraGM20BMetal/visibility.h"
#include "kits/SwitchGM20B/SwitchGM20B.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"
#include "visibility_model.h"

namespace {

struct Method {
    uint32_t subchannel, method, value;
    bool operator==(const Method &) const = default;
};

std::vector<Method> methods(const std::vector<uint32_t> &words)
{
    std::vector<switch_gm20b_method> decoded(words.size() * 2 + 1);
    const long n = switch_gm20b_push_decode(words.data(), words.size(), decoded.data(), decoded.size());
    std::vector<Method> out;
    for (long i = 0; i < n; i++)
        out.push_back({decoded[size_t(i)].subchannel, decoded[size_t(i)].method, decoded[size_t(i)].value});
    return out;
}

struct Recorder {
    std::vector<uint32_t> words = std::vector<uint32_t>(4096);
    gm20b::Push p{words.data(), words.data() + words.size()};

    std::vector<uint32_t> take()
    {
        std::vector<uint32_t> out(words.data(), p.at);
        p.at = words.data();
        return out;
    }
};

std::vector<uint32_t> methodNumbers(const std::vector<Method> &m)
{
    std::vector<uint32_t> out;
    for (const Method &x : m)
        out.push_back(x.method);
    return out;
}

constexpr uint64_t kResults = 0x0100002000, kSlot0 = 0x0200000000, kSlot1 = 0x0200000010;

TEST(GM20BVisibility, CountingOnARawBufferIsTaken)
{
    gm20b::VisibilityAsk a;
    a.mode = gm20b::kVisibilityCounting;
    a.offset = 8;
    a.bufferLength = 16;
    a.gm20bBuffer = true;
    EXPECT_EQ(gm20b::visibilityRefusal(a), nullptr);
}

TEST(GM20BVisibility, RefusalsNameTheirReason)
{
    gm20b::VisibilityAsk a;
    a.mode = gm20b::kVisibilityCounting;
    a.offset = 16;
    a.bufferLength = 16;
    a.gm20bBuffer = true;
    EXPECT_NE(gm20b::visibilityRefusal(a), nullptr) << "past the buffer";
    a.offset = 12;
    a.bufferLength = 16;
    EXPECT_EQ(gm20b::visibilityRefusal(a), nullptr) << "M4 writes at the offset rounded down to 8";
    a.gm20bBuffer = false;
    EXPECT_NE(gm20b::visibilityRefusal(a), nullptr);
    a.gm20bBuffer = true;
    a.mode = gm20b::kVisibilityBoolean;
    EXPECT_EQ(gm20b::visibilityRefusal(a), nullptr) << "Boolean is taken as Counting is";
    a.mode = gm20b::kVisibilityDisabled;
    a.gm20bBuffer = false;
    EXPECT_EQ(gm20b::visibilityRefusal(a), nullptr) << "Disabled needs no buffer";
}

TEST(GM20BVisibility, AModeSetClearsTheCounterAndCounts)
{
    Recorder r;
    gm20b::VisibilityQueries q;
    ASSERT_EQ(q.set(r.p, gm20b::kVisibilityCounting, kResults, kSlot0), gm20b::VisibilityQueries::Set::Done);
    EXPECT_EQ(methods(r.take()),
              (std::vector<Method>{{0, NVB197_CLEAR_REPORT_VALUE, NVB197_CLEAR_REPORT_VALUE_TYPE_ZPASS_PIXEL_CNT},
                                   {0, NVB197_SET_ZPASS_PIXEL_COUNT, 1}}));
}

TEST(GM20BVisibility, TheNextModeReportsTheOpenCountIntoItsSlot)
{
    Recorder r;
    gm20b::VisibilityQueries q;
    q.set(r.p, gm20b::kVisibilityCounting, kResults, kSlot0);
    r.take();
    ASSERT_EQ(q.set(r.p, gm20b::kVisibilityCounting, kResults + 8, kSlot1), gm20b::VisibilityQueries::Set::Done);
    const auto m = methods(r.take());
    ASSERT_EQ(m.size(), 6u);
    EXPECT_EQ(m[0], (Method{0, NVB197_SET_REPORT_SEMAPHORE_A, uint32_t(kSlot0 >> 32)}));
    EXPECT_EQ(m[1], (Method{0, NVB197_SET_REPORT_SEMAPHORE_B, uint32_t(kSlot0)}));
    EXPECT_EQ(m[3].method, NVB197_SET_REPORT_SEMAPHORE_D);
    EXPECT_EQ(m[4].method, NVB197_CLEAR_REPORT_VALUE);
    EXPECT_EQ(m[5].method, NVB197_SET_ZPASS_PIXEL_COUNT);
}

TEST(GM20BVisibility, DisabledReportsAndStopsCounting)
{
    Recorder r;
    gm20b::VisibilityQueries q;
    q.set(r.p, gm20b::kVisibilityCounting, kResults, kSlot0);
    r.take();
    q.set(r.p, gm20b::kVisibilityDisabled, 0, 0);
    EXPECT_EQ(methodNumbers(methods(r.take())),
              (std::vector<uint32_t>{NVB197_SET_REPORT_SEMAPHORE_A, NVB197_SET_REPORT_SEMAPHORE_B,
                                     NVB197_SET_REPORT_SEMAPHORE_C, NVB197_SET_REPORT_SEMAPHORE_D,
                                     NVB197_SET_ZPASS_PIXEL_COUNT}));
    q.set(r.p, gm20b::kVisibilityDisabled, 0, 0);
    EXPECT_TRUE(r.take().empty()) << "Disabled twice emits nothing";
}

TEST(GM20BVisibility, AnOffsetTakenTwiceGoesToMtwoV)
{
    Recorder r;
    gm20b::VisibilityQueries q;
    q.set(r.p, gm20b::kVisibilityCounting, kResults, kSlot0);
    q.set(r.p, gm20b::kVisibilityDisabled, 0, 0);
    r.take();
    EXPECT_EQ(q.set(r.p, gm20b::kVisibilityCounting, kResults, kSlot1), gm20b::VisibilityQueries::Set::Retaken);
    EXPECT_TRUE(r.take().empty());
}

TEST(GM20BVisibility, TheEndWaitsForIdleThenCopiesEachCountToItsOffset)
{
    Recorder r;
    gm20b::VisibilityQueries q;
    q.set(r.p, gm20b::kVisibilityCounting, kResults, kSlot0);
    q.set(r.p, gm20b::kVisibilityCounting, kResults + 8, kSlot1);
    r.take();
    ASSERT_TRUE(q.any());
    q.finish(r.p);
    EXPECT_EQ(methodNumbers(methods(r.take())),
              (std::vector<uint32_t>{NVB197_SET_REPORT_SEMAPHORE_A, NVB197_SET_REPORT_SEMAPHORE_B,
                                     NVB197_SET_REPORT_SEMAPHORE_C, NVB197_SET_REPORT_SEMAPHORE_D,
                                     NVB197_SET_ZPASS_PIXEL_COUNT, NVB197_WAIT_FOR_IDLE}));
    ASSERT_EQ(q.results(), 2u);
    for (size_t i = 0; i < 2; i++) {
        q.finishResult(r.p, i);
        const auto words = r.take();
        EXPECT_LE(words.size(), gm20b::VisibilityQueries::kResultWords);
        const auto m = methods(words);
        ASSERT_EQ(m.size(), 9u);
        const uint64_t slot = i ? kSlot1 : kSlot0, result = kResults + 8 * i;
        EXPECT_EQ(m[0], (Method{4, NVB0B5_OFFSET_IN_UPPER, uint32_t(slot >> 32)}));
        EXPECT_EQ(m[1], (Method{4, NVB0B5_OFFSET_IN_LOWER, uint32_t(slot)}));
        EXPECT_EQ(m[3], (Method{4, NVB0B5_OFFSET_OUT_LOWER, uint32_t(result)}));
        EXPECT_EQ(m[6], (Method{4, NVB0B5_LINE_LENGTH_IN, 8}));
        EXPECT_EQ(m[8].method, NVB0B5_LAUNCH_DMA);
    }
    q.clear();
    EXPECT_FALSE(q.any());
}

// Each half of the count copied to its half of the offset and made at most 1 there by the copy's semaphore release
TEST(GM20BVisibility, ABooleanResultIsEachHalfOfTheCountAtMostOne)
{
    Recorder r;
    gm20b::VisibilityQueries q;
    q.set(r.p, gm20b::kVisibilityBoolean, kResults, kSlot0);
    q.finish(r.p);
    r.take();
    q.finishResult(r.p, 0);
    const auto words = r.take();
    EXPECT_LE(words.size(), gm20b::VisibilityQueries::kResultWords);
    const auto m = methods(words);
    ASSERT_EQ(m.size(), 24u);
    for (uint32_t half = 0; half < 2; half++) {
        const Method *copy = &m[12 * half];
        EXPECT_EQ(copy[1], (Method{4, NVB0B5_OFFSET_IN_LOWER, uint32_t(kSlot0) + 4 * half}));
        EXPECT_EQ(copy[3], (Method{4, NVB0B5_OFFSET_OUT_LOWER, uint32_t(kResults) + 4 * half}));
        EXPECT_EQ(copy[6], (Method{4, NVB0B5_LINE_LENGTH_IN, 4}));
        EXPECT_EQ(copy[9], (Method{4, NVB0B5_SET_SEMAPHORE_B, uint32_t(kResults) + 4 * half}));
        EXPECT_EQ(copy[10], (Method{4, NVB0B5_SET_SEMAPHORE_PAYLOAD, 1}));
        ASSERT_EQ(copy[11].method, NVB0B5_LAUNCH_DMA);
        const uint32_t launch = copy[11].value;
        EXPECT_EQ(launch >> 14 & 0xf, uint32_t(NVB0B5_LAUNCH_DMA_SEMAPHORE_REDUCTION_IMIN)) << std::hex << launch;
        EXPECT_EQ(launch >> 18 & 1, uint32_t(NVB0B5_LAUNCH_DMA_SEMAPHORE_REDUCTION_SIGN_UNSIGNED));
        EXPECT_EQ(launch >> 19 & 1, 1u) << "the reduction is enabled";
    }
}

// Every offset maxVisibilityQueryOffset (65,528) leaves is taken once; one 65,536 bytes on shares a key and is refused
TEST(GM20BVisibility, AnEncoderTakesEveryOffsetTheDeviceAnswersOnce)
{
    Recorder r;
    gm20b::VisibilityQueries q;
    for (uint32_t i = 0; i < gm20b::kVisibilityQueriesPerEncoder; i++) {
        ASSERT_EQ(q.set(r.p, gm20b::kVisibilityCounting, kResults + 8 * i, kSlot0 + 32 * i),
                  gm20b::VisibilityQueries::Set::Done)
            << i;
        r.take();
    }
    EXPECT_EQ(gm20b::kVisibilityQueriesPerEncoder, 8192u);
    EXPECT_EQ(q.set(r.p, gm20b::kVisibilityCounting, kResults + 65536, kSlot0), gm20b::VisibilityQueries::Set::Retaken);
    q.clear();
    EXPECT_EQ(q.set(r.p, gm20b::kVisibilityCounting, kResults, kSlot0), gm20b::VisibilityQueries::Set::Done)
        << "a new encoder takes its offsets again";
}

// One encoder's mode sets and draws run on the model, then its end, each result written where the end puts it
struct Step {
    uint32_t mode;  // or a draw of `samples` where `draw`
    uint32_t offset;
    uint64_t samples = 0;
    bool draw = false;
};

Step at(uint32_t mode, uint32_t offset)
{
    return {mode, offset};
}

Step drawn(uint64_t samples)
{
    return {0, 0, samples, true};
}

std::vector<uint64_t> ran(const std::vector<uint64_t> &preset, const std::vector<Step> &steps, bool accumulate)
{
    visibility_model::Gpu gpu;
    for (size_t i = 0; i < preset.size(); i++)
        gpu.putDword(kResults + 8 * i, preset[i]);
    Recorder r;
    gm20b::VisibilityQueries q;
    uint32_t slot = 0;
    for (const Step &s : steps) {
        if (s.draw) {
            gpu.draw(s.samples);
            continue;
        }
        const uint64_t slotVa = s.mode ? kSlot0 + gm20b::kVisibilitySlotBytes * slot++ : 0;
        EXPECT_EQ(q.set(r.p, s.mode, s.mode ? kResults + s.offset : 0, slotVa, accumulate),
                  gm20b::VisibilityQueries::Set::Done);
        const auto words = r.take();
        EXPECT_LE(words.size(), gm20b::VisibilityQueries::kSetWords);
        gpu.run(words);
    }
    q.finish(r.p);
    auto words = r.take();
    EXPECT_LE(words.size(), gm20b::VisibilityQueries::kFinishWords);
    gpu.run(words);
    for (size_t i = 0; i < q.results(); i++) {
        q.finishResult(r.p, i);
        words = r.take();
        EXPECT_LE(words.size(), gm20b::VisibilityQueries::kResultWords);
        gpu.run(words);
    }
    EXPECT_TRUE(gpu.enabled) << "the copy engine's render enable is put back to always";
    std::vector<uint64_t> out;
    for (size_t i = 0; i < preset.size(); i++)
        out.push_back(gpu.dword(kResults + 8 * i));
    return out;
}

constexpr uint32_t kCount = gm20b::kVisibilityCounting, kBool = gm20b::kVisibilityBoolean, kOff = 0;

// An M4 Max's answers (macOS 27.2.1): a pass that resets writes the
// count or 1 or 0 at each offset it set, 0 with no draw
TEST(GM20BVisibility, AResettingPassWritesEachOffsetsCountOrWhetherAny)
{
    const auto got = ran({1000, 7, 0xabab, 0xdead00000000, 5, 9},
                         {at(kCount, 0), drawn(10000), at(kCount, 8), at(kCount, 24), drawn(100), at(kBool, 32),
                          drawn(100), at(kBool, 40), at(kOff, 0), drawn(50)},
                         false);
    EXPECT_EQ(got, (std::vector<uint64_t>{10000, 0, 0xabab, 100, 1, 0}));
}

// Accumulate: Counting adds to the low word modulo 2^32 and keeps the high word; no draw keeps the bytes
TEST(GM20BVisibility, AnAccumulatingPassAddsTheCountToTheLowWord)
{
    const auto got = ran({1000, 7, 0xabab, 0xfffffffffffffff0, 0xffffffff, 0x100000000},
                         {at(kCount, 0), drawn(10000), at(kCount, 8), at(kCount, 24), drawn(100), at(kCount, 32),
                          drawn(1), at(kCount, 40), drawn(16), at(kOff, 0), drawn(50)},
                         true);
    EXPECT_EQ(got, (std::vector<uint64_t>{11000, 7, 0xabab, 0xffffffff00000054, 0, 0x100000010}));
}

// Accumulate: Boolean ORs 1 into the low word where a sample passed, whatever its slot held before
TEST(GM20BVisibility, AnAccumulatingPassOrsOneWhereAnySamplePassed)
{
    const auto got = ran({5, 2, 0x100000000, 0, 3, 0},
                         {at(kBool, 0), drawn(100), at(kBool, 8), drawn(100), at(kBool, 16), drawn(1), at(kBool, 24),
                          drawn(4096), at(kBool, 32), at(kBool, 40), at(kOff, 0), drawn(50)},
                         true);
    EXPECT_EQ(got, (std::vector<uint64_t>{5, 3, 0x100000001, 1, 3, 0}));
}

// A count of 2^32 samples: an M4 Max's counter is 32 bits, so the low word is what any pass sees
TEST(GM20BVisibility, AnAccumulatingPassSeesTheCountersLowWord)
{
    const auto got = ran({0, 0x10}, {at(kCount, 0), drawn(0x110000000), at(kBool, 8), drawn(0x100000000)}, true);
    EXPECT_EQ(got, (std::vector<uint64_t>{0x10000000, 0x10}));
}

}  // namespace

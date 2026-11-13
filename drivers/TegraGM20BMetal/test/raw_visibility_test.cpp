// SPDX-License-Identifier: MIT
// raw/runtime/raw_visibility.h: which visibility results the raw path writes, and the methods an encoder's mode sets and
// end emit (the sample counter per mode set, its report into a slot, the copy engine's copy to the offset).
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "drivers/TegraGM20BMetal/raw/runtime/raw_visibility.h"
#include "kits/SwitchGM20B/SwitchGM20B.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"

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
    raw::Push p{words.data(), words.data() + words.size()};

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

TEST(RawVisibility, CountingOnARawBufferIsTaken)
{
    raw::VisibilityAsk a;
    a.mode = raw::kVisibilityCounting;
    a.offset = 8;
    a.bufferLength = 16;
    a.rawBuffer = true;
    EXPECT_EQ(raw::visibilityRefusal(a), nullptr);
}

TEST(RawVisibility, RefusalsNameTheirReason)
{
    raw::VisibilityAsk a;
    a.mode = raw::kVisibilityCounting;
    a.offset = 16;
    a.bufferLength = 16;
    a.rawBuffer = true;
    EXPECT_NE(raw::visibilityRefusal(a), nullptr) << "past the buffer";
    a.offset = 12;
    a.bufferLength = 16;
    EXPECT_EQ(raw::visibilityRefusal(a), nullptr) << "M4 writes at the offset rounded down to 8";
    a.rawBuffer = false;
    EXPECT_NE(raw::visibilityRefusal(a), nullptr);
    a.rawBuffer = true;
    a.accumulate = true;
    EXPECT_NE(raw::visibilityRefusal(a), nullptr);
    a.accumulate = false;
    a.mode = raw::kVisibilityBoolean;
    EXPECT_EQ(raw::visibilityRefusal(a), nullptr) << "Boolean is taken as Counting is";
    a.mode = raw::kVisibilityDisabled;
    a.rawBuffer = false;
    EXPECT_EQ(raw::visibilityRefusal(a), nullptr) << "Disabled needs no buffer";
}

TEST(RawVisibility, AModeSetClearsTheCounterAndCounts)
{
    Recorder r;
    raw::VisibilityQueries q;
    ASSERT_EQ(q.set(r.p, raw::kVisibilityCounting, kResults, kSlot0), raw::VisibilityQueries::Set::Done);
    EXPECT_EQ(methods(r.take()),
              (std::vector<Method>{{0, NVB197_CLEAR_REPORT_VALUE, NVB197_CLEAR_REPORT_VALUE_TYPE_ZPASS_PIXEL_CNT},
                                   {0, NVB197_SET_ZPASS_PIXEL_COUNT, 1}}));
}

TEST(RawVisibility, TheNextModeReportsTheOpenCountIntoItsSlot)
{
    Recorder r;
    raw::VisibilityQueries q;
    q.set(r.p, raw::kVisibilityCounting, kResults, kSlot0);
    r.take();
    ASSERT_EQ(q.set(r.p, raw::kVisibilityCounting, kResults + 8, kSlot1), raw::VisibilityQueries::Set::Done);
    const auto m = methods(r.take());
    ASSERT_EQ(m.size(), 6u);
    EXPECT_EQ(m[0], (Method{0, NVB197_SET_REPORT_SEMAPHORE_A, uint32_t(kSlot0 >> 32)}));
    EXPECT_EQ(m[1], (Method{0, NVB197_SET_REPORT_SEMAPHORE_B, uint32_t(kSlot0)}));
    EXPECT_EQ(m[3].method, NVB197_SET_REPORT_SEMAPHORE_D);
    EXPECT_EQ(m[4].method, NVB197_CLEAR_REPORT_VALUE);
    EXPECT_EQ(m[5].method, NVB197_SET_ZPASS_PIXEL_COUNT);
}

TEST(RawVisibility, DisabledReportsAndStopsCounting)
{
    Recorder r;
    raw::VisibilityQueries q;
    q.set(r.p, raw::kVisibilityCounting, kResults, kSlot0);
    r.take();
    q.set(r.p, raw::kVisibilityDisabled, 0, 0);
    EXPECT_EQ(methodNumbers(methods(r.take())),
              (std::vector<uint32_t>{NVB197_SET_REPORT_SEMAPHORE_A, NVB197_SET_REPORT_SEMAPHORE_B,
                                     NVB197_SET_REPORT_SEMAPHORE_C, NVB197_SET_REPORT_SEMAPHORE_D,
                                     NVB197_SET_ZPASS_PIXEL_COUNT}));
    q.set(r.p, raw::kVisibilityDisabled, 0, 0);
    EXPECT_TRUE(r.take().empty()) << "Disabled twice emits nothing";
}

TEST(RawVisibility, AnOffsetTakenTwiceGoesToMtwoV)
{
    Recorder r;
    raw::VisibilityQueries q;
    q.set(r.p, raw::kVisibilityCounting, kResults, kSlot0);
    q.set(r.p, raw::kVisibilityDisabled, 0, 0);
    r.take();
    EXPECT_EQ(q.set(r.p, raw::kVisibilityCounting, kResults, kSlot1), raw::VisibilityQueries::Set::Retaken);
    EXPECT_TRUE(r.take().empty());
}

TEST(RawVisibility, TheEndWaitsForIdleThenCopiesEachCountToItsOffset)
{
    Recorder r;
    raw::VisibilityQueries q;
    q.set(r.p, raw::kVisibilityCounting, kResults, kSlot0);
    q.set(r.p, raw::kVisibilityCounting, kResults + 8, kSlot1);
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
        EXPECT_LE(words.size(), raw::VisibilityQueries::kResultWords);
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
TEST(RawVisibility, ABooleanResultIsEachHalfOfTheCountAtMostOne)
{
    Recorder r;
    raw::VisibilityQueries q;
    q.set(r.p, raw::kVisibilityBoolean, kResults, kSlot0);
    q.finish(r.p);
    r.take();
    q.finishResult(r.p, 0);
    const auto words = r.take();
    EXPECT_LE(words.size(), raw::VisibilityQueries::kResultWords);
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

TEST(RawVisibility, AnEncoderTakesAtMostItsQueryLimit)
{
    Recorder r;
    raw::VisibilityQueries q;
    for (uint32_t i = 0; i < raw::kVisibilityQueriesPerEncoder; i++) {
        ASSERT_EQ(q.set(r.p, raw::kVisibilityCounting, kResults + 8 * i, kSlot0 + 16 * i),
                  raw::VisibilityQueries::Set::Done);
        r.take();
    }
    EXPECT_EQ(q.set(r.p, raw::kVisibilityCounting, kResults + 8 * raw::kVisibilityQueriesPerEncoder, kSlot0),
              raw::VisibilityQueries::Set::Full);
}

}  // namespace

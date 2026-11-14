// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchHost1xFlipRun.h against a model of host1x's display channel and DC A's window A: each flip is a stream held
// behind its producer's syncpoint and judged at the next frame end; a producer that fails, a channel that never runs
// the stream, a latch that is refused or never comes, and the display stopping each end in window A on the surface it
// started on, with the stream torn down before the gate writes, and the run bounded in frames.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "../SwitchHost1xFlipRun.h"

namespace {

using switch_host1x_flip::Active;
using switch_host1x_flip::Run;
using switch_host1x_flip::Surface;

constexpr uint32_t kProducer = 170, kDisplay = 171;
constexpr Surface kFront = {0x40000000, 0, 5120, 0}, kAlias = {0x60000000, 0, 5120, 0};

struct Model {
    Run run;
    Surface assembly = kFront, shown = kFront;
    uint32_t control = 0;
    uint32_t producerSyncpt = 40, displaySyncpt = 900;
    uint32_t words[64] = {}, wordCount = 0;
    bool queued = false, stuck = false, freeze = false, refuseNext = false, quietNow = true, underflowNext = false;
    bool reserveFails = false, raiseFails = false;
    uint32_t delayFrames = 0, latched = 0;
    const char *missing = nullptr;
    std::vector<std::string> calls, errors;
    std::vector<std::vector<uint32_t>> streams;
    bool done = false;

    bool quiet() const
    {
        return quietNow;
    }

    const char *surfaces(Surface &front, Surface &alias) const
    {
        if (missing)
            return missing;
        front = kFront;
        alias = kAlias;
        return nullptr;
    }

    bool underflowed()
    {
        const bool was = underflowNext;
        underflowNext = false;
        return was;
    }

    Active active() const
    {
        return {shown, control};
    }

    static uint32_t producer()
    {
        return kProducer;
    }

    static uint32_t display()
    {
        return kDisplay;
    }

    uint32_t producerValue() const
    {
        return producerSyncpt;
    }

    uint32_t displayValue() const
    {
        return displaySyncpt;
    }

    uint32_t *reserve(uint32_t capacity)
    {
        EXPECT_LE(capacity, 64u);
        EXPECT_FALSE(queued) << "one stream in flight at a time";
        return reserveFails ? nullptr : words;
    }

    int submit(const uint32_t *w, uint32_t n, uint32_t threshold)
    {
        EXPECT_EQ(w, words);
        EXPECT_EQ(threshold, displaySyncpt + 1);
        streams.emplace_back(w, w + n);
        wordCount = n;
        queued = true;
        calls.emplace_back("submit");
        execute();
        EXPECT_TRUE(queued) << "the stream waits for its producer";
        return 0;
    }

    int raiseProducer(uint32_t at)
    {
        if (raiseFails)
            return -5;
        EXPECT_EQ(at, producerSyncpt + 1);
        producerSyncpt = at;
        calls.emplace_back("raise");
        execute();
        return 0;
    }

    void releaseProducer(uint32_t at)
    {
        producerSyncpt = at;
        calls.emplace_back("release");
        execute();
    }

    void teardown()
    {
        queued = false;
        calls.emplace_back("teardown");
    }

    void restart()
    {
        calls.emplace_back("restart");
    }

    void gateWrite(const Surface &s)
    {
        EXPECT_FALSE(queued) << "the gate writes only once no stream can";
        assembly = s;
        control |= switch_host1x_flip::kRequest;
        calls.emplace_back(s.start == kFront.start ? "gate front" : "gate alias");
    }

    void error(const char *what)
    {
        errors.emplace_back(what);
    }

    void latchedByHost1x()
    {
        ++latched;
    }

    void finished()
    {
        done = true;
    }

    // host1x: the channel's WAIT holds until the producer reaches its threshold; then DC A takes the class 0x70 words
    // in order and returns the display syncpoint's index.
    void execute()
    {
        if (!queued || stuck || delayFrames)
            return;
        ASSERT_GE(wordCount, 4u);
        ASSERT_EQ(words[0], tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_HOST1X, 0x4e, 5));
        if (!switch_host1x_flip::reached(producerSyncpt, words[1]) || words[2] != kProducer)
            return;
        ASSERT_EQ(words[3], tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_DC, 0, 0));
        for (uint32_t i = 4; i < wordCount; ++i) {
            const uint32_t op = words[i] >> 28, offset = (words[i] >> 16) & 0xfff;
            if (op == 4) {
                ASSERT_EQ(offset, switch_host1x_flip::kIncrSyncpt);
                ASSERT_EQ(words[i] & 0xff, kDisplay);
                ++displaySyncpt;
                continue;
            }
            ASSERT_EQ(op, 1u);
            ASSERT_EQ(words[i] & 0xffff, 1u);
            const uint32_t value = words[++i];
            if (offset == switch_host1x_flip::kWinAStart)
                assembly.start = value;
            else if (offset == switch_host1x_flip::kStateControl && (value & switch_host1x_flip::kRequest))
                control |= switch_host1x_flip::kRequest;
            else
                ASSERT_EQ(offset, switch_host1x_flip::kStateControl);
        }
        queued = false;
    }

    // DC A's frame boundary, the frame end the display's action runs the trial at, then the frame after it, in which
    // a delayed stream runs.
    void frameEnd()
    {
        if (control && !freeze) {
            shown = refuseNext ? Surface{0x12340000, 0, 5120, 0} : assembly;
            refuseNext = false;
            control = 0;
        }
        run.frameEnd(*this);
        if (delayFrames && !--delayFrames)
            execute();
    }

    unsigned runToEnd(unsigned bound = 400)
    {
        unsigned n = 0;
        while (!done && n < bound) {
            frameEnd();
            ++n;
        }
        return n;
    }

    uint32_t gateWrites() const
    {
        uint32_t n = 0;
        for (const auto &call : calls)
            n += call.rfind("gate", 0) == 0;
        return n;
    }
};

TEST(Host1xFlipStream, WaitThenWindowAStartThenItsRequestThenDcAsIncrement)
{
    uint32_t w[switch_host1x_flip::kStreamWords] = {};
    ASSERT_EQ(switch_host1x_flip::stream(w, 0x60000000, kProducer, 41, kDisplay), switch_host1x_flip::kStreamWords);
    const uint32_t expected[] = {
        tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_HOST1X, 0x4e, 5),
        41,
        kProducer,
        0x70u << 6,
        0x1bc00001,
        0x60000000,
        0x10410001,
        0x300,
        0x10410001,
        0x3,
        0x400000ab,
    };
    for (uint32_t i = 0; i < switch_host1x_flip::kStreamWords; ++i)
        EXPECT_EQ(w[i], expected[i]) << i;
}

TEST(Host1xFlipRun, EveryFlipIsWrittenByHost1xBehindItsProducerAndLatchedAsWritten)
{
    Model m;
    m.run.begin(6, false);
    const unsigned frames = m.runToEnd();
    ASSERT_TRUE(m.done);
    EXPECT_EQ(frames, 7u) << "one quiet frame end, then one per flip";
    EXPECT_EQ(m.run.why, nullptr);
    EXPECT_EQ(m.run.totals.host1x, 6u);
    EXPECT_EQ(m.run.totals.latched, 6u);
    EXPECT_EQ(m.run.totals.written, 6u);
    EXPECT_EQ(m.latched, 6u);
    EXPECT_EQ(m.run.totals.gate + m.run.totals.teardowns + m.run.totals.unwritten + m.run.totals.notAsWritten, 0u);
    EXPECT_EQ(m.gateWrites(), 0u);
    EXPECT_TRUE(m.errors.empty());
    EXPECT_EQ(m.displaySyncpt - m.run.displayStart, m.run.totals.host1x);
    EXPECT_EQ(m.shown.start, kFront.start) << "an even count ends where it began";
    ASSERT_EQ(m.streams.size(), 6u);
    for (size_t i = 0; i < m.streams.size(); ++i)
        EXPECT_EQ(m.streams[i][5], i % 2 ? kFront.start : kAlias.start) << i;
}

TEST(Host1xFlipRun, AnOddCountGoesBackToTheFrontThroughTheGate)
{
    Model m;
    m.run.begin(3, false);
    m.runToEnd();
    ASSERT_TRUE(m.done);
    EXPECT_EQ(m.run.totals.host1x, 3u);
    EXPECT_EQ(m.calls.back(), "gate front");
    EXPECT_EQ(m.shown.start, kFront.start);
}

// The forced arm: the middle flip's producer fails. The channel is torn down before its producer's syncpoint is
// released, so the stream never writes; the gate writes that flip; the restarted channel carries the rest.
TEST(Host1xFlipRun, AFailedProducersStreamIsTornDownBeforeTheGateWritesItsFlip)
{
    Model m;
    m.run.begin(6, true);
    m.runToEnd();
    ASSERT_TRUE(m.done);
    EXPECT_EQ(m.run.why, nullptr) << "a forced failure does not end the run";
    EXPECT_EQ(m.run.totals.host1x, 5u);
    EXPECT_EQ(m.run.totals.gate, 1u);
    EXPECT_EQ(m.run.totals.teardowns, 1u);
    EXPECT_EQ(m.run.totals.afterTeardown, 2u);
    EXPECT_EQ(m.run.totals.latched, 6u);
    EXPECT_EQ(m.displaySyncpt - m.run.displayStart, m.run.totals.host1x)
        << "the torn-down stream never wrote, though its producer's syncpoint was released";
    ASSERT_EQ(m.errors.size(), 1u);
    EXPECT_EQ(m.errors[0], "its producer failed");
    std::vector<std::string> taken;
    for (size_t i = 0; i < m.calls.size(); ++i) {
        if (m.calls[i] == "teardown")
            taken.assign(m.calls.begin() + static_cast<long>(i), m.calls.begin() + static_cast<long>(i) + 4);
    }
    EXPECT_EQ(taken, (std::vector<std::string>{"teardown", "release", "restart", "gate front"}));
    EXPECT_EQ(m.shown.start, kFront.start);
}

// A stream host1x runs only after the first frame end that follows its issue (a frame-end pass that ran late, the next
// one at once) is still host1x's: the run tears down nothing.
TEST(Host1xFlipRun, AStreamOneFrameLateIsNotTornDown)
{
    Model m;
    m.run.begin(4, false);
    m.frameEnd();
    m.delayFrames = 2;
    m.runToEnd();
    ASSERT_TRUE(m.done);
    EXPECT_EQ(m.run.totals.teardowns, 0u);
    EXPECT_EQ(m.run.totals.host1x, 4u);
    EXPECT_TRUE(m.errors.empty());
}

TEST(Host1xFlipRun, AChannelThatNeverRunsTheStreamIsTornDownAndTheGateWritesTheFlip)
{
    Model m;
    m.stuck = true;
    m.run.begin(6, false);
    const unsigned frames = m.runToEnd();
    ASSERT_TRUE(m.done);
    EXPECT_LE(frames, 6u);
    EXPECT_STREQ(m.run.why, "unwritten");
    EXPECT_EQ(m.run.totals.unwritten, 1u);
    EXPECT_EQ(m.run.totals.gate, 1u);
    EXPECT_EQ(m.run.totals.host1x, 0u);
    EXPECT_EQ(m.calls,
              (std::vector<std::string>{"submit", "raise", "teardown", "restart", "gate alias", "gate front"}));
    EXPECT_EQ(m.shown.start, kFront.start);
    EXPECT_EQ(m.displaySyncpt, m.run.displayStart);
}

TEST(Host1xFlipRun, AFlipLatchedAsSomethingElseGoesThroughTheGateAndEndsTheRun)
{
    Model m;
    m.run.begin(6, false);
    m.frameEnd();
    m.refuseNext = true;
    m.runToEnd();
    ASSERT_TRUE(m.done);
    EXPECT_STREQ(m.run.why, "not_as_written");
    EXPECT_EQ(m.run.totals.notAsWritten, 1u);
    EXPECT_EQ(m.run.totals.gate, 1u);
    ASSERT_EQ(m.errors.size(), 1u);
    EXPECT_EQ(m.shown.start, kFront.start);
}

// The trial's own bound replaces the display's "never latched" panic: a request that never latches ends the flips in
// kLatchFrames, and a restore that never latches ends the run in kLatchFrames more.
TEST(Host1xFlipRun, ARequestThatNeverLatchesEndsTheRunInBoundedFrames)
{
    Model m;
    m.run.begin(6, false);
    m.frameEnd();
    m.frameEnd();
    ASSERT_EQ(m.shown.start, kAlias.start);
    m.freeze = true;
    const unsigned frames = m.runToEnd();
    ASSERT_TRUE(m.done);
    EXPECT_EQ(frames, 2 * switch_host1x_flip::kLatchFrames);
    EXPECT_STREQ(m.run.why, "not_latched");
    EXPECT_EQ(m.errors, (std::vector<std::string>{"window A did not latch it", "window A did not take the restore"}));
    EXPECT_EQ(m.calls.back(), "gate front");
}

TEST(Host1xFlipRun, AFlipThatNeverLeftTheFrontNeedsNoRestoreLatch)
{
    Model m;
    m.run.begin(6, false);
    m.frameEnd();
    m.freeze = true;
    EXPECT_EQ(m.runToEnd(), switch_host1x_flip::kLatchFrames + 1);
    EXPECT_STREQ(m.run.why, "not_latched");
    EXPECT_EQ(m.errors, (std::vector<std::string>{"window A did not latch it"}));
    EXPECT_EQ(m.shown.start, kFront.start);
}

TEST(Host1xFlipRun, ABusyDisplayEndsTheRunUntouched)
{
    Model m;
    m.quietNow = false;
    m.run.begin(6, false);
    EXPECT_EQ(m.runToEnd(), switch_host1x_flip::kWaitFrames);
    EXPECT_STREQ(m.run.why, "busy");
    EXPECT_TRUE(m.calls.empty());
}

TEST(Host1xFlipRun, NoSurfaceToFlipToEndsTheRunUntouched)
{
    Model m;
    m.missing = "no_alias_slot";
    m.run.begin(6, false);
    EXPECT_EQ(m.runToEnd(), 1u);
    EXPECT_STREQ(m.run.why, "no_alias_slot");
    EXPECT_TRUE(m.calls.empty());
}

TEST(Host1xFlipRun, AStreamTheChannelRefusesGoesThroughTheGate)
{
    Model m;
    m.reserveFails = true;
    m.run.begin(6, false);
    m.runToEnd();
    ASSERT_TRUE(m.done);
    EXPECT_STREQ(m.run.why, "submit");
    EXPECT_EQ(m.calls, (std::vector<std::string>{"gate alias", "gate front"}));
    EXPECT_EQ(m.shown.start, kFront.start);
}

TEST(Host1xFlipRun, AProducerSyncpointThatDoesNotRiseIsTakenBackWithoutARelease)
{
    Model m;
    m.raiseFails = true;
    m.run.begin(6, false);
    m.runToEnd();
    ASSERT_TRUE(m.done);
    EXPECT_STREQ(m.run.why, "producer");
    EXPECT_EQ(m.run.totals.unwritten, 0u);
    EXPECT_EQ(m.calls, (std::vector<std::string>{"submit", "teardown", "restart", "gate alias", "gate front"}));
}

TEST(Host1xFlipRun, TheDisplayStoppingTearsTheStreamDownBeforeTheFrontGoesBack)
{
    Model m;
    m.stuck = true;
    m.run.begin(6, false);
    m.frameEnd();
    m.run.abort(m, "the display stopped");
    ASSERT_TRUE(m.done);
    EXPECT_STREQ(m.run.why, "the display stopped");
    EXPECT_EQ(m.calls, (std::vector<std::string>{"submit", "raise", "teardown", "restart", "gate front"}));
    m.stuck = false;
    m.execute();
    EXPECT_EQ(m.displaySyncpt, m.run.displayStart) << "nothing of the torn-down stream ran";
    m.frameEnd();
    EXPECT_EQ(m.shown.start, kFront.start);
}

TEST(Host1xFlipRun, UnderflowsWhileFlippingAreCounted)
{
    Model m;
    m.run.begin(4, false);
    m.frameEnd();
    m.underflowNext = true;
    m.runToEnd();
    EXPECT_EQ(m.run.totals.underflows, 1u);
}

}  // namespace

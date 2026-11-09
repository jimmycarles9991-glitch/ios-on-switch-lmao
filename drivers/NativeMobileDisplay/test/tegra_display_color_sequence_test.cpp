// Host test of drivers/NativeMobileDisplay/TegraDisplayColorSequence.h against a raster model of DC A: the OLED's 1301
// line frame (1280 active, FRAME_END at the last active line, 21 blanking lines), CMU_ENABLE latching at a frame start
// after its GENERAL_ACT_REQ, and LUT1, CSC and LUT2 words that change the moment they are written. The filter runs a
// random interrupt latency after each frame end and the action a random time after it, as TegraDisplayColor.cpp drives
// them; other requests hold GENERAL_ACT_REQ at random. No LUT word may change while the latched CMU scans, no CSC word
// while it scans an active line, and once the inputs stop DC A shows exactly the last tables.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <random>

#include "../TegraDisplayColorSequence.h"

using namespace switch_color;

namespace {

constexpr int64_t kActiveLines = 1280, kFrameLines = 1301;

bool always()
{
    return true;
}

bool never()
{
    return false;
}

struct Dc {
    Tables words = {};
    bool assemblyEnable = false, latchedEnable = false, actRequest = false;
    unsigned lutWritesScanned = 0, cscWritesScanned = 0;
};

bool inBlank(int64_t line)
{
    return line % kFrameLines >= kActiveLines;
}

void latchFramesUpTo(Dc &dc, int64_t &nextFrameStart, int64_t line)
{
    while (nextFrameStart <= line) {
        if (dc.actRequest) {
            dc.latchedEnable = dc.assemblyEnable;
            dc.actRequest = false;
        }
        nextFrameStart += kFrameLines;
    }
}

void writeWords(Dc &dc, const Tables &next, const Tables *written, bool cscOnly, int64_t line)
{
    const bool lutsChange = !cscOnly && (!written || memcmp(written->lut1, next.lut1, sizeof(next.lut1)) != 0 ||
                                         memcmp(written->lut2, next.lut2, sizeof(next.lut2)) != 0);
    const bool cscChanges = !written || memcmp(written->csc, next.csc, sizeof(next.csc)) != 0;
    if (dc.latchedEnable && lutsChange)
        ++dc.lutWritesScanned;
    if (dc.latchedEnable && cscChanges && !inBlank(line))
        ++dc.cscWritesScanned;
    if (!cscOnly) {
        memcpy(dc.words.lut1, next.lut1, sizeof(next.lut1));
        memcpy(dc.words.lut2, next.lut2, sizeof(next.lut2));
    }
    memcpy(dc.words.csc, next.csc, sizeof(next.csc));
}

Inputs defaults()
{
    Inputs in;
    identityGamma(in.gamma);
    return in;
}

// One random change of what IOMobileFramebuffer asks for: a white point step, a brightness scale, a blank, an
// inversion, a gamma table or contrast, or back to nothing at all.
void change(Inputs &in, std::mt19937 &random)
{
    switch (random() % 7) {
    case 0:
    case 1:
        for (int r = 0; r < 3; ++r)
            in.harmony.m[r][r] = kOne - static_cast<Q32>(random() % (kOne / 4));
        break;
    case 2:
        in.correction = kUnity16 / 2 + static_cast<uint32_t>(random() % (kUnity16 / 2));
        break;
    case 3:
        in.blank = !in.blank;
        break;
    case 4:
        in.remapMode = in.remapMode == kRemapInverted ? kRemapNormal : kRemapInverted;
        break;
    case 5:
        in.gamma[random() % 256] = static_cast<uint8_t>(random());
        in.contrast = random() % 2 ? kUnity16 : kUnity16 + kUnity16 / 4;
        break;
    default:
        in = defaults();
        break;
    }
}

struct Outcome {
    unsigned lutWritesScanned, cscWritesScanned, cscDeferred, bypasses;
    bool settledShowsTheLastTables;
};

Outcome run(uint32_t seed, int64_t maxFilterLatency)
{
    std::mt19937 random(seed);
    Dc dc;
    Sequence s;
    Tables pending = {}, active = {};
    Inputs in = defaults();
    build(in, pending);
    s.pendingDirty = true;
    int64_t nextFrameStart = kFrameLines;
    unsigned cscDeferred = 0, bypasses = 0;
    constexpr int kFrames = 4000, kQuietFrames = 30;
    for (int frame = 0; frame < kFrames + kQuietFrames; ++frame) {
        const int64_t frameEnd = int64_t(frame) * kFrameLines + kActiveLines;
        if (frame < kFrames && random() % 3 == 0) {
            change(in, random);
            build(in, pending);
            s.pendingDirty = true;
        }
        const int64_t filterAt = frameEnd + int64_t(random() % uint32_t(maxFilterLatency + 1));
        latchFramesUpTo(dc, nextFrameStart, filterAt);
        if (frame < kFrames && random() % 4 == 0)
            dc.actRequest = true;
        const Tables *written = s.activeValid ? &active : nullptr;
        const bool wasDirty = s.pendingDirty;
        const auto actRequestOut = [&] {
            return dc.actRequest;
        };
        const auto filterInBlank = [&] {
            return inBlank(filterAt);
        };
        const Write write = atFrameEnd(s, pending, written, actRequestOut, filterInBlank);
        if (write != Write::kNothing) {
            writeWords(dc, pending, written, write == Write::kCsc, filterAt);
            active = pending;
        } else if (wasDirty && pending.enabled && !s.bypassWanted) {
            ++cscDeferred;
        }
        const int64_t actionAt = filterAt + int64_t(random() % (kFrameLines - kActiveLines / 4));
        latchFramesUpTo(dc, nextFrameStart, actionAt);
        const Enable enable = atFrameEndAction(s, pending.enabled, random() % 5 != 0 || frame >= kFrames);
        if (enable != Enable::kKeep) {
            dc.assemblyEnable = enable == Enable::kOn;
            dc.actRequest = true;
            enableWritten(s, enable == Enable::kOn);
            bypasses += enable == Enable::kOff && pending.enabled ? 1 : 0;
        }
    }
    latchFramesUpTo(dc, nextFrameStart, nextFrameStart);
    bool shows = settled(s, pending.enabled) && dc.latchedEnable == pending.enabled;
    if (pending.enabled) {
        shows = shows && memcmp(dc.words.lut1, pending.lut1, sizeof(pending.lut1)) == 0 &&
                memcmp(dc.words.csc, pending.csc, sizeof(pending.csc)) == 0 &&
                memcmp(dc.words.lut2, pending.lut2, sizeof(pending.lut2)) == 0;
    }
    return {dc.lutWritesScanned, dc.cscWritesScanned, cscDeferred, bypasses, shows};
}

}  // namespace

TEST(Sequence, NoWordChangesUnderTheScanWithPromptFilters)
{
    for (uint32_t seed = 1; seed <= 40; ++seed) {
        const Outcome r = run(seed, 4);
        EXPECT_EQ(r.lutWritesScanned, 0u) << "seed " << seed;
        EXPECT_EQ(r.cscWritesScanned, 0u) << "seed " << seed;
        EXPECT_TRUE(r.settledShowsTheLastTables) << "seed " << seed;
        EXPECT_GT(r.bypasses, 0u) << "seed " << seed << ": LUT changes with the CMU on bypass it first";
    }
}

TEST(Sequence, LateFiltersDeferTheCscToALaterBlank)
{
    for (uint32_t seed = 1; seed <= 40; ++seed) {
        const Outcome r = run(seed, 3 * (kFrameLines - kActiveLines));
        EXPECT_EQ(r.lutWritesScanned, 0u) << "seed " << seed;
        EXPECT_EQ(r.cscWritesScanned, 0u) << "seed " << seed;
        EXPECT_TRUE(r.settledShowsTheLastTables) << "seed " << seed;
        EXPECT_GT(r.cscDeferred, 0u) << "seed " << seed;
    }
}

TEST(Sequence, AWhitePointRampIsCscOnlyAndNeverBypasses)
{
    Sequence s;
    Tables pending = {}, active = {};
    Inputs in = defaults();
    in.harmony.m[2][2] = kOne / 2;
    build(in, pending);
    s.pendingDirty = true;
    ASSERT_EQ(atFrameEnd(s, pending, nullptr, never, always), Write::kAll);
    active = pending;
    ASSERT_EQ(atFrameEndAction(s, true, true), Enable::kOn);
    enableWritten(s, true);
    for (int step = 0; step < 120; ++step) {
        in.harmony.m[2][2] = kOne / 2 + step * (kOne / 512);
        in.blank = step % 30 == 0;
        build(in, pending);
        s.pendingDirty = true;
        EXPECT_EQ(atFrameEnd(s, pending, &active, never, always), Write::kCsc);
        active = pending;
        EXPECT_EQ(atFrameEndAction(s, true, true), Enable::kKeep) << "step " << step;
    }
}

TEST(Sequence, ALutChangeWaitsForTheBypassToLatch)
{
    Sequence s;
    Tables pending = {}, active = {};
    Inputs in = defaults();
    in.harmony.m[0][0] = kOne / 2;
    build(in, pending);
    s.pendingDirty = true;
    ASSERT_EQ(atFrameEnd(s, pending, nullptr, never, always), Write::kAll);
    active = pending;
    enableWritten(s, true);
    in.remapMode = kRemapInverted;
    build(in, pending);
    s.pendingDirty = true;
    EXPECT_EQ(atFrameEnd(s, pending, &active, never, always), Write::kNothing);
    EXPECT_EQ(atFrameEndAction(s, true, true), Enable::kOff);
    enableWritten(s, false);
    EXPECT_EQ(atFrameEnd(s, pending, &active, always, always), Write::kNothing)
        << "GENERAL_ACT_REQ still out: the CMU may still scan";
    EXPECT_EQ(atFrameEnd(s, pending, &active, never, never), Write::kAll) << "latched: any line will do";
    EXPECT_EQ(atFrameEndAction(s, true, true), Enable::kOn);
}

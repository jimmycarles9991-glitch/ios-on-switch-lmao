// SPDX-License-Identifier: MIT
// draw_times.h and draw_capture.h: a frame-weights mark's words, its read-back as the time since the mark before and
// the fragment invocations between them, and a capture's command buffers taken, handed over, read and logged in numbered
// lines.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include "drivers/TegraGM20BMetal/draw_capture.h"

namespace {
using namespace gm20b;

constexpr uint64_t kMarksVa = 0x40'0000'0000ull;
constexpr uint32_t kSerial = 7;
// PTIMER units of 625/384 ns (stamps.h): 384 units are 625 ns
constexpr uint64_t kUnit = 384;

// What the GPU does with marks' words: each wait for idle lets PTIMER run on by `step` units and the invocations counter by
// `pixels`, each release writes its payload and PTIMER, each invocations report the counter and PTIMER
struct Gpu {
    std::vector<TimedMark> memory = std::vector<TimedMark>(kTimedMarks);
    uint64_t ptimer = 1000, counter = 0;

    void run(const uint32_t *w, const uint32_t *end, uint64_t step, uint64_t pixels)
    {
        while (w < end) {
            const uint32_t op = w[0] >> 29, method = (w[0] & 0x1fff) << 2, n = w[0] >> 16 & 0x1fff;
            if (op == 4) {
                if (method == NVB197_WAIT_FOR_IDLE) {
                    ptimer += step;
                    counter += pixels;
                }
                w++;
                continue;
            }
            if (method == NVB197_SET_REPORT_SEMAPHORE_A && n == 4) {
                uint8_t *at = reinterpret_cast<uint8_t *>(memory.data()) + ((uint64_t(w[1]) << 32 | w[2]) - kMarksVa);
                if ((w[4] & 3) == NVB197_SET_REPORT_SEMAPHORE_D_OPERATION_RELEASE) {
                    const StampRelease r{w[3], 0, ptimer};
                    memcpy(at, &r, sizeof r);
                } else {
                    const uint64_t r[2] = {counter, ptimer};
                    memcpy(at, r, sizeof r);
                }
            }
            w += 1 + n;
        }
    }

    void mark(uint32_t index, uint32_t serial, uint64_t step, uint64_t pixels)
    {
        if (index == kNoMark)
            return;
        uint32_t words[kTimedMarkWords];
        Push p{words, words + kTimedMarkWords};
        emitTimedMark(p, kMarksVa + uint64_t(index) * sizeof(TimedMark), serial, false);
        run(words, p.at, step, pixels);
    }
};

TimedLabel draw(uint16_t state, uint16_t pass)
{
    TimedLabel l;
    l.kind = TimedKind::Draw;
    l.state = state;
    l.pass = pass;
    l.topology = 4;
    l.vertices = 3;
    l.instances = 1;
    l.box = 1280 * 720;
    return l;
}

TimedState fill(uint64_t key, const char *name)
{
    TimedState s;
    s.key = key;
    s.name = name;
    s.fragmentWords = 40;
    s.vertexWords = 20;
    s.form = uint8_t(SpanForm::Whole);
    s.gprs = 16;
    return s;
}

// A capture driven as the device drives it: begin(), the marks of a pass of draws, commit; a completer that sees the GPU
// ran each seqno in `ran` (lost: in `lost`) and reads the stamps this GPU wrote
struct Capture {
    DrawTimes times;
    Gpu gpu;
    std::map<uint32_t, std::pair<uint64_t, uint64_t>> stamps;
    std::set<uint32_t> ran, lost;
    int describes = 0;
    int ids[4] = {};

    // One command buffer: its start, one 1280x720 pass and `draws` full-screen draws of the states `ids[state[i]]`, each
    // 6250 ns; null where the capture did not take it
    TimedBuffer *buffer(uint32_t stamp, const std::vector<int> &states)
    {
        TimedBuffer *b = times.begin(stamp);
        if (!b)
            return nullptr;
        const uint64_t start = gpu.ptimer;
        TimedLabel l;
        l.kind = TimedKind::Start;
        gpu.mark(times.reserve(*b, l), times.serial(), kUnit, 0);
        TimedPass tp;
        tp.format = 80;
        tp.width = 1280;
        tp.height = 720;
        tp.blockLinear = true;
        l.pass = times.pass(*b, tp);
        l.kind = TimedKind::Between;
        gpu.mark(times.reserve(*b, l), times.serial(), kUnit, 0);
        l.kind = TimedKind::Prologue;
        gpu.mark(times.reserve(*b, l), times.serial(), 2 * kUnit, 0);
        for (int s : states) {
            const uint16_t index = times.state(&ids[s], [&] {
                describes++;
                return fill(0x3e01 + uint64_t(s), s ? "Glass" : "Fill");
            });
            gpu.mark(times.reserve(*b, draw(index, l.pass)), times.serial(), 10 * kUnit, 1280 * 720);
        }
        stamps[stamp] = {start, gpu.ptimer + kUnit};
        gpu.ptimer += kUnit;
        return b;
    }

    std::vector<std::string> report()
    {
        return times.report(
            gpu.memory.data(), "test",
            [&](uint32_t seqno, uint64_t) {
                return lost.count(seqno) ? -1 : ran.count(seqno) ? 1 : 0;
            },
            [&](uint32_t stamp, uint64_t &start, uint64_t &end) {
                const auto it = stamps.find(stamp);
                if (it == stamps.end())
                    return false;
                start = it->second.first;
                end = it->second.second;
                return true;
            },
            [](uint32_t stamp) {
                return int64_t(stamp) * 1000;
            });
    }
};

TEST(GM20BDrawTimes, AMarkFlushesTheTiledCacheWaitsForIdleThenReleasesItsTimeAndReportsInvocations)
{
    uint32_t words[kTimedMarkWords];
    Push p{words, words + kTimedMarkWords};
    emitTimedMark(p, 0x12'3456'7880ull, kSerial, true);
    ASSERT_EQ(p.at, words + kTimedMarkWords);
    EXPECT_EQ(words[0], Push::hdr(4, SUBC_3D, SWITCH_NVB197_TILED_CACHE_FLUSH, 0));
    EXPECT_EQ(words[1], Push::hdr(4, SUBC_3D, NVB197_WAIT_FOR_IDLE, 0));
    EXPECT_EQ(words[2], Push::hdr(1, SUBC_3D, NVB197_SET_REPORT_SEMAPHORE_A, 4));
    EXPECT_EQ(words[3], 0x12u);
    EXPECT_EQ(words[4], 0x34567880u);
    EXPECT_EQ(words[5], kSerial);
    // OPERATION 1:0 RELEASE, FLUSH_DISABLE 2:2 TRUE, RELEASE 4:4 after every write, PIPELINE_LOCATION 15:12 ALL,
    // STRUCTURE_SIZE 28:28 four words
    EXPECT_EQ(words[6], 1u << 2 | 1u << 4 | 0xfu << 12);
    EXPECT_EQ(words[7], Push::hdr(1, SUBC_3D, NVB197_SET_REPORT_SEMAPHORE_A, 4));
    EXPECT_EQ(words[9], 0x34567890u);
    // OPERATION REPORT_ONLY (2), FLUSH_DISABLE 2:2, PIPELINE_LOCATION PIXEL_SHADER (0xa), REPORT 27:23 PS_INVOCATIONS
    // (0x13), STRUCTURE_SIZE four words: the 64-bit counter, then PTIMER
    EXPECT_EQ(words[11], 2u | 1u << 2 | 0xau << 12 | 0x13u << 23);
}

TEST(GM20BDrawTimes, WithoutTheTiledCacheAMarkStartsAtItsWaitForIdle)
{
    uint32_t words[kTimedMarkWords];
    Push p{words, words + kTimedMarkWords};
    emitTimedMark(p, kMarksVa, kSerial, false);
    EXPECT_EQ(p.at, words + kTimedMarkWords - 1);
    EXPECT_EQ(words[0], Push::hdr(4, SUBC_3D, NVB197_WAIT_FOR_IDLE, 0));
}

TEST(GM20BDrawTimes, ThePixelRegistersAreTheLastCountSetAndNeverADataWord)
{
    constexpr uint32_t kCount = NVB197_SET_PIPELINE_REGISTER_COUNT(NVB197_SET_PIPELINE_SHADER_TYPE_PIXEL);
    std::vector<uint32_t> w(64);
    Push p{w.data(), w.data() + w.size()};
    const auto registers = [&] {
        return pixelRegisters(w.data(), size_t(p.at - w.data()));
    };
    // a constant upload whose data is the immediate header that would set 200 registers
    const uint32_t lookalike[2] = {Push::hdr(4, SUBC_3D, kCount, 200), 0};
    p.nonInc(NVB197_LOAD_CONSTANT_BUFFER(0), lookalike, 2);
    EXPECT_EQ(registers(), 0u);
    p.m(kCount, 48);
    EXPECT_EQ(registers(), 48u);
    p.inc(kCount - 4, {1, 96});
    EXPECT_EQ(registers(), 96u);
    p.m(kCount, 12, SUBC_COMPUTE);
    EXPECT_EQ(registers(), 96u);
}

TEST(GM20BDrawTimes, EachMarkIsTheTimeSinceTheLastWrittenOneAndItsInvocationsTheCounterStep)
{
    Gpu gpu;
    TimedLabel labels[4] = {{}, draw(0, 0), draw(1, 0), draw(2, 0)};
    labels[0].kind = TimedKind::Start;
    const uint64_t start = gpu.ptimer;
    gpu.mark(0, kSerial, kUnit, 0);
    gpu.mark(1, kSerial, 10 * kUnit, 921600);
    // mark 2 was taken and never ran (its pass was refused); an earlier capture's release is still in its memory
    gpu.memory[2].release = {kSerial - 1, 0, gpu.ptimer + 1};
    gpu.mark(3, kSerial, 2 * kUnit, 100);
    const uint64_t end = gpu.ptimer + kUnit;
    const TimedReadback rb = readTimedMarks(gpu.memory.data(), labels, 0, 4, kSerial, start, end);
    EXPECT_EQ(rb.unwritten, 1u);
    ASSERT_EQ(rb.rows.size(), 4u);
    EXPECT_EQ(rb.rows[0].label.kind, TimedKind::Start);
    EXPECT_EQ(rb.rows[0].ns, 625u);
    EXPECT_EQ(rb.rows[0].invocations, -1);
    EXPECT_EQ(rb.rows[1].label.state, 0u);
    EXPECT_EQ(rb.rows[1].ns, 6250u);
    EXPECT_EQ(rb.rows[1].invocations, 921600);
    EXPECT_EQ(rb.rows[2].label.state, 2u);
    EXPECT_EQ(rb.rows[2].ns, 1250u);
    EXPECT_EQ(rb.rows[2].invocations, 100);
    EXPECT_EQ(rb.rows[3].label.kind, TimedKind::Tail);
    EXPECT_EQ(rb.rows[3].ns, 625u);
}

TEST(GM20BDrawTimes, ACaptureLogsItsCommandBuffersInNumberedLinesAndDropsAnAbandonedOne)
{
    Capture c;
    c.times.ask(2);
    EXPECT_TRUE(c.times.asking());
    TimedBuffer *b0 = c.buffer(10, {0, 0});
    ASSERT_NE(b0, nullptr);
    c.times.commit(*b0, 100, 1);
    ASSERT_NE(c.buffer(11, {1}), nullptr);  // abandoned: no commit
    EXPECT_EQ(c.times.begin(12), nullptr);
    EXPECT_FALSE(c.times.asking());
    c.ran = {100};
    const std::vector<std::string> lines = c.report();
    const std::vector<std::string> expected = {
        "c1.0 capture test asked 2 stamp 10 ref 8000 9000",
        "c1.1 cb b0 s10 start 1000 gpu 15625 marks 5 unwritten 0 full 0",
        "c1.2 passes b0 0:80/1280x720/1/bl",
        "c1.3 state 0 0000000000003e01 whole fs40 vs20 r16 x1 Fill",
        "c1.4 d b0 s:-:-:625 b:0:0:625 p:0:0:1250 d0:0:4:3x1:921600:921600:6250 d0:0:4:3x1:921600:921600:6250 "
        "t:-:-:625",
        "c1.5 end taken 2 reported 1 dropped 1 lines 6",
    };
    EXPECT_EQ(lines, expected);
    EXPECT_TRUE(c.report().empty());
    const DrawTimesTaken t = c.times.taken();
    EXPECT_TRUE(t.ended);
    EXPECT_EQ(t.reference, (std::vector<int64_t>{8000, 9000}));
    ASSERT_EQ(t.buffers.size(), 1u);
    EXPECT_EQ(t.buffers[0].gpuNs, 15625u);
    ASSERT_EQ(t.rows.size(), 6u);
    EXPECT_EQ(t.rows[3].state.key, 0x3e01u);
    EXPECT_EQ(t.rows[3].pass.width, 1280u);
    EXPECT_EQ(t.rows[3].row.invocations, 921600);
}

TEST(GM20BDrawTimes, ACompleterWaitsForTheGpuAndDropsACommandBufferLostWithItsChannel)
{
    Capture c;
    c.times.ask(2);
    c.times.commit(*c.buffer(10, {0}), 100, 1);
    c.times.commit(*c.buffer(11, {0}), 101, 1);
    EXPECT_TRUE(c.report().empty());
    EXPECT_TRUE(c.times.committed());
    c.ran = {100};
    c.lost = {101};
    const std::vector<std::string> lines = c.report();
    ASSERT_EQ(lines.size(), 7u);
    EXPECT_EQ(lines[1], "c1.1 cb b0 s10 start 1000 gpu 9375 marks 4 unwritten 0 full 0");
    EXPECT_EQ(lines[5], "c1.5 dropped b1 lost");
    EXPECT_EQ(lines[6], "c1.6 end taken 2 reported 1 dropped 1 lines 7");
    EXPECT_FALSE(c.times.committed());
}

TEST(GM20BDrawTimes, ANewAskStartsANewCaptureAndNamesTheOneItCut)
{
    Capture c;
    c.times.ask(1);
    c.times.commit(*c.buffer(10, {0}), 100, 1);
    c.times.ask(1);
    TimedBuffer *b = c.buffer(11, {0});
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(c.times.serial(), 2u);
    c.times.commit(*b, 101, 1);
    c.ran = {100, 101};
    const std::vector<std::string> lines = c.report();
    ASSERT_FALSE(lines.empty());
    EXPECT_EQ(lines[0], "c2.0 capture test asked 1 stamp 11 ref 10000 supersedes c1");
    EXPECT_EQ(lines[1].rfind("c2.1 cb b0 s11 ", 0), 0u);
    for (const std::string &line : lines)
        EXPECT_EQ(line.find(" s10 "), std::string::npos) << line;
}

TEST(GM20BDrawTimes, EachStateIsNamedOnceAndAFullCaptureCountsTheMarksItRefused)
{
    Capture c;
    c.times.ask(1);
    TimedBuffer *b = c.buffer(10, {0, 1, 0, 1});
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(c.describes, 2);
    for (uint32_t i = b->marks; i < kTimedMarks; i++)
        c.times.reserve(*b, draw(0, 0));
    EXPECT_EQ(c.times.reserve(*b, draw(0, 0)), kNoMark);
    EXPECT_EQ(c.times.reserve(*b, draw(0, 0)), kNoMark);
    c.times.commit(*b, 100, 1);
    c.ran = {100};
    uint32_t named = 0, full = 0;
    for (const std::string &line : c.report()) {
        named += line.find(" state ") != std::string::npos;
        full += line.find(" full 2") != std::string::npos;
    }
    EXPECT_EQ(named, 2u);
    EXPECT_EQ(full, 1u);
}

TEST(GM20BDrawTimes, AFramesHundredsOfDrawsFitFewLinesUnderTheLimit)
{
    Capture c;
    c.times.ask(1);
    c.times.commit(*c.buffer(10, std::vector<int>(600, 0)), 100, 1);
    c.ran = {100};
    size_t entries = 0, drawLines = 0;
    for (const std::string &line : c.report()) {
        EXPECT_LE(line.size(), kTimedLineBytes + 16) << line.substr(0, 40);
        if (line.find(" d b0 ") == std::string::npos)
            continue;
        drawLines++;
        const std::string body = line.substr(line.find(" d b0 ") + 6);
        entries += size_t(std::count(body.begin(), body.end(), ' ')) + 1;
    }
    EXPECT_EQ(entries, 600u + 4u);
    EXPECT_LE(drawLines, 30u);
}

}  // namespace

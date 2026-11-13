// SPDX-License-Identifier: MIT
// raw_span_marks.h: the render server's marks after each draw and pass prologue, read back as spans of one state each, and
// the hooks that write them where the GPU runs them in the order they were taken.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "bazel/testing/source_text.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_span_marks.h"

namespace {
using namespace raw;
using switch_ios::test::Body;
using switch_ios::test::SourceText;

constexpr uint64_t kPageVa = 0x40'0000'0000ull;

struct FakePipeline {
    uint32_t fragmentInstructions = 0;
    const char *caName = nullptr;
    const char *spanName = nullptr;
};

// What the GPU does with one report: payload and PTIMER at its address in the marks memory
void release(const uint32_t *words, std::vector<uint8_t> &memory, uint64_t ptimer)
{
    const uint64_t at = (uint64_t(words[1]) << 32 | words[2]) - kPageVa;
    ASSERT_LE(at + sizeof(SpanMark), memory.size());
    const SpanMark m{words[3], 0, ptimer};
    memcpy(memory.data() + at, &m, sizeof m);
}

// PTIMER units for `ns` (raw_stamps.h: 625/384 ns a unit)
uint64_t units(uint64_t ns)
{
    return ns * 384 / 625;
}

TEST(RawSpanMarks, AMarkIsAFourWordTimestampReportAfterEveryWrite)
{
    uint32_t words[kSpanMarkWords];
    Push p{words, words + kSpanMarkWords};
    emitSpanMark(p, 0x12'3456'7890ull, 9);
    ASSERT_EQ(p.at, words + kSpanMarkWords);
    EXPECT_EQ(words[0], Push::hdr(1, SUBC_3D, NVB197_SET_REPORT_SEMAPHORE_A, 4));
    EXPECT_EQ(words[1], 0x12u);
    EXPECT_EQ(words[2], 0x34567890u);
    EXPECT_EQ(words[3], 9u);
    // SET_REPORT_SEMAPHORE_D: OPERATION 1:0 RELEASE, RELEASE 4:4 after writes, PIPELINE_LOCATION 15:12 ALL, STRUCTURE_SIZE
    // 28:28 four words (clb197.h): no wait for idle, unlike the stamps' host release
    EXPECT_EQ(words[4], 0u | 1u << 4 | 0xfu << 12);
}

TEST(RawSpanMarks, ConsecutiveDrawsOfOneStateAreOneSpanAndTheTailEndsAtTheEndStamp)
{
    SpanMarks marks;
    std::vector<uint8_t> memory(SpanMarks::kBytes);
    marks.open(kPageVa, memory.data());
    const uint32_t stamp = 34;  // slot 1: the memory's second command buffer
    marks.begin(stamp);
    FakePipeline generic{50401, "PBGRAXmw_A3Xghfc", nullptr};
    FakePipeline resolve{642, nullptr, "raw.resolve.resolve_bgra8_frag"};
    const uint32_t prologue = marks.reserve();
    std::vector<std::vector<uint32_t>> pushed;
    const auto mark = [&](uint32_t index, const SpanLabel &label) {
        std::vector<uint32_t> w(kSpanMarkWords);
        Push p{w.data(), w.data() + w.size()};
        marks.markAt(p, index, label);
        pushed.push_back(w);
    };
    mark(marks.reserve(), drawSpanLabel(generic, 0x64ed02fd5d7a1865ull, 921, 1));
    mark(marks.reserve(), drawSpanLabel(generic, 0x64ed02fd5d7a1865ull, 921, 1));
    mark(marks.reserve(), drawSpanLabel(resolve, 0x77ull, 921, 1));
    SpanLabel clear;
    clear.kind = SpanKind::Prologue;
    clear.kpx = 3686;
    clear.samples = 4;
    mark(prologue, clear);
    // the GPU runs the prologue first, then the draws: the marks' order is the order they were taken
    const uint64_t start = 1000;
    release(pushed[3].data(), memory, start + units(2'000'000));
    release(pushed[0].data(), memory, start + units(32'000'000));
    release(pushed[1].data(), memory, start + units(63'000'000));
    release(pushed[2].data(), memory, start + units(75'000'000));
    const std::vector<StateSpan> spans = marks.take(stamp, start, start + units(75'500'000));
    ASSERT_EQ(spans.size(), 4u);
    EXPECT_EQ(spans[0].label.kind, SpanKind::Prologue);
    EXPECT_NEAR(double(spans[0].ns), 2e6, 2e3);
    EXPECT_STREQ(spans[1].label.name, "PBGRAXmw_A3Xghfc");
    EXPECT_EQ(spans[1].marks, 2u);
    EXPECT_NEAR(double(spans[1].ns), 61e6, 2e3);
    EXPECT_STREQ(spans[2].label.name, "raw.resolve.resolve_bgra8_frag");
    EXPECT_EQ(spans[3].label.kind, SpanKind::Tail);
    char line[112];
    formatSpan(line, sizeof line, spans[1]);
    EXPECT_EQ(std::string(line), "61000 us PBGRAXmw_A3Xghfc k64ed02fd5d7a1865 50401 instr x2 921k");
    formatSpan(line, sizeof line, spans[0]);
    EXPECT_EQ(std::string(line), "2000 us pass prologue k0000000000000000 0 instr x1 3686km");
}

TEST(RawSpanMarks, AMarkTheGpuNeverWroteGivesItsTimeToTheNextAndALaterBufferTakesTheSlot)
{
    SpanMarks marks;
    std::vector<uint8_t> memory(SpanMarks::kBytes);
    marks.open(kPageVa, memory.data());
    marks.begin(1);
    (void)marks.reserve();  // a pass refused after its prologue's mark was taken
    std::vector<uint32_t> w(kSpanMarkWords);
    Push p{w.data(), w.data() + w.size()};
    FakePipeline plain{100, nullptr, nullptr};
    marks.markAt(p, marks.reserve(), drawSpanLabel(plain, 5, 1, 1));
    release(w.data(), memory, 100 + units(6'000'000));
    std::vector<StateSpan> spans = marks.take(1, 100, 100 + units(6'000'000));
    ASSERT_EQ(spans.size(), 2u);
    EXPECT_EQ(spans[0].label.key, 5u);
    EXPECT_NEAR(double(spans[0].ns), 6e6, 2e3);
    marks.begin(1 + kSpanSlots);
    EXPECT_TRUE(marks.take(1, 100, 100 + units(6'000'000)).empty());
}

TEST(RawSpanMarks, AFullBufferTakesNoMoreMarks)
{
    SpanMarks marks;
    std::vector<uint8_t> memory(SpanMarks::kBytes);
    marks.open(kPageVa, memory.data());
    marks.begin(3);
    for (uint32_t i = 0; i < kSpanMarksPerBuffer; i++)
        ASSERT_EQ(marks.reserve(), i);
    EXPECT_EQ(marks.reserve(), SpanMarks::kNone);
    uint32_t w[kSpanMarkWords] = {};
    Push p{w, w + kSpanMarkWords};
    marks.markAt(p, SpanMarks::kNone, SpanLabel{});
    EXPECT_EQ(p.at, w);
}

TEST(RawSpanMarks, OnlyTheRenderServerMarksAndEveryDrawAndPrologueIsMarked)
{
    const std::string gpu = SourceText("../raw/runtime/raw_gpu.cpp");
    EXPECT_NE(gpu.find("if (role_ == Role::RenderServer && k_.allocControl(SpanMarks::kBytes"), std::string::npos);
    const std::string finish = Body(gpu, "void RenderEncoder::finishDraw(");
    EXPECT_NE(finish.find("markSpan(p_, dev_->spans_.reserve(), SpanKind::Draws)"), std::string::npos);
    const std::string end = Body(gpu, "void RenderEncoder::end()");
    EXPECT_NE(end.find("markSpan(p, prologueMark_, SpanKind::Prologue)"), std::string::npos);
    EXPECT_NE(Body(gpu, "RenderEncoder *CommandBuffer::render(").find("e.prologueMark_ = dev_->spans_.reserve()"),
              std::string::npos);
    EXPECT_NE(Body(gpu, "CommandBuffer *Device::begin()").find("spans_.begin(cb.stamp)"), std::string::npos);
}
}  // namespace

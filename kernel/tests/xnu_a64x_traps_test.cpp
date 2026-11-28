// <switch/a64x_traps.h> on the host: the per-CPU ring of translator traps, the window it drains into and its line;
// both writers of a64x_traps' notes are a64x_emulate_user's handled paths.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "bazel/testing/source_text.h"
#include "switch/a64x_traps.h"

using testing::HasSubstr;

namespace {

switch_a64x_note Note(int32_t pid, const char *name, bool patched = false)
{
    switch_a64x_note n = {};
    n.pid = pid;
    n.patched = patched;
    std::strncpy(n.name, name, sizeof(n.name) - 1);
    return n;
}

void Push(switch_a64x_ring *r, int32_t pid, const char *name, bool patched = false)
{
    const switch_a64x_note n = Note(pid, name, patched);
    switch_a64x_ring_push(r, &n);
}

std::string Line(const switch_a64x_window &w)
{
    char line[512];
    switch_a64x_window_format(line, sizeof(line), &w);
    return line;
}

TEST(XnuA64xTraps, AWindowCountsEachProcessAndOrdersTheMostFirst)
{
    auto r = std::make_unique<switch_a64x_ring>();
    for (int i = 0; i < 3; i++)
        Push(r.get(), 40, "WebContent");
    for (int i = 0; i < 5; i++)
        Push(r.get(), 412, "jsc", i < 2);
    Push(r.get(), 7, "logd");
    switch_a64x_window w = {};
    switch_a64x_ring_drain(r.get(), &w);
    EXPECT_EQ(Line(w), "by process jsc[412] 5 (2 patched), WebContent[40] 3 (0 patched), logd[7] 1 (0 patched); "
                       "0 processes more, 0 notes dropped");
    switch_a64x_window again = {};
    switch_a64x_ring_drain(r.get(), &again);
    EXPECT_EQ(again.count, 0u) << "a drained note is never seen twice";
}

TEST(XnuA64xTraps, AFullRingDropsAndCountsWhatItCouldNotKeep)
{
    auto r = std::make_unique<switch_a64x_ring>();
    for (uint32_t i = 0; i < SWITCH_A64X_RING + 10; i++)
        Push(r.get(), 1, "a");
    switch_a64x_window w = {};
    switch_a64x_ring_drain(r.get(), &w);
    EXPECT_EQ(w.processes[0].emulated, SWITCH_A64X_RING);
    EXPECT_EQ(w.dropped, 10u);
    Push(r.get(), 1, "a");
    switch_a64x_window next = {};
    switch_a64x_ring_drain(r.get(), &next);
    EXPECT_EQ(next.dropped, 0u) << "drops are counted once";
    EXPECT_EQ(next.processes[0].emulated, 1u) << "a drained ring takes notes again";
}

TEST(XnuA64xTraps, ProcessesPastTheTableAreCountedNotListed)
{
    auto r = std::make_unique<switch_a64x_ring>();
    for (int32_t pid = 1; pid <= static_cast<int32_t>(SWITCH_A64X_PROCESSES) + 3; pid++)
        Push(r.get(), pid, "p");
    switch_a64x_window w = {};
    switch_a64x_ring_drain(r.get(), &w);
    EXPECT_EQ(w.count, SWITCH_A64X_PROCESSES);
    EXPECT_EQ(w.unlisted, 3u);
    EXPECT_THAT(Line(w), HasSubstr("; 15 processes more, 0 notes dropped"));
}

TEST(XnuA64xTraps, OneWriterAndOneReaderLoseNothingTheRingKept)
{
    auto r = std::make_unique<switch_a64x_ring>();
    constexpr uint32_t kNotes = 200000;
    std::thread writer([&] {
        for (uint32_t i = 0; i < kNotes; i++)
            Push(r.get(), 9, "w");
    });
    switch_a64x_window w = {};
    while (w.processes[0].emulated + w.dropped < kNotes)
        switch_a64x_ring_drain(r.get(), &w);
    writer.join();
    switch_a64x_ring_drain(r.get(), &w);
    EXPECT_EQ(w.processes[0].emulated + w.dropped, kNotes);
}

TEST(XnuA64xTrapsSource, EveryHandledTrapIsNoted)
{
    const std::string xnu = switch_ios::test::SourceText("../switch/translate/a64x_xnu.c");
    const std::string emulate = switch_ios::test::Body(xnu, "int a64x_emulate_user(arm_saved_state_t *state");
    ASSERT_FALSE(emulate.empty());
    size_t returns = 0;
    for (size_t at = emulate.find("return 1;"); at != std::string::npos; at = emulate.find("return 1;", at + 1)) {
        ++returns;
        const size_t line = emulate.rfind('\n', emulate.rfind('\n', at) - 1);
        EXPECT_THAT(emulate.substr(line, at - line), HasSubstr("switch_a64x_trap_noted(c, "))
            << "the handled return at offset " << at;
    }
    EXPECT_EQ(returns, 5u);
}

}  // namespace

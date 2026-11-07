// kernel/switch/diag/frames.h on the host: the commit cadence and touch delivery the display's lines report, and the
// two trees' ipc_kmsg_send hooks that feed it.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "diag/frames.h"

using testing::HasSubstr;

namespace {

constexpr uint64_t kPeriod = 16667;

std::string line(const switch_frames_cadence &c, const switch_frames_delivery *d, size_t size = 512)
{
    std::string out(size, '\0');
    const size_t n = switch_frames_format(&c, d, 4, out.data(), size);
    EXPECT_EQ(n, strlen(out.c_str()));
    out.resize(n);
    return out;
}

// One transaction: a message per context, back to back.
void commit(switch_frames_cadence &c, int pid, const char *name, uint64_t at, unsigned contexts = 5)
{
    for (unsigned i = 0; i < contexts; i++)
        switch_frames_note_message(&c, pid, name, at + i * 150, kPeriod);
}

std::string source(const char *path)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

}  // namespace

TEST(Frames, BurstsAreCommitsAndGapsAreInPeriods)
{
    switch_frames_cadence c = {};
    uint64_t t = 1000000;
    commit(c, 27, "SpringBoard", t);
    for (int i = 0; i < 10; i++)
        commit(c, 27, "SpringBoard", t += kPeriod);
    commit(c, 27, "SpringBoard", t += 2 * kPeriod);
    commit(c, 27, "SpringBoard", t += 3 * kPeriod);
    commit(c, 27, "SpringBoard", t += 5 * kPeriod);
    commit(c, 27, "SpringBoard", t += 4000);  // a second commit inside one frame
    EXPECT_EQ(c.client[0].commits, 15u);
    EXPECT_EQ(line(c, nullptr), "SpringBoard[27] 15 commits <1:1 1:10 2:1 3:1 4+:1");
}

TEST(Frames, StillClientsAndRestartsMeasureNoGap)
{
    switch_frames_cadence c = {};
    commit(c, 5, "Widget", 1000000);
    commit(c, 5, "Widget", 1000000 + SWITCH_FRAMES_STILL_US + 1);
    EXPECT_EQ(line(c, nullptr), "Widget[5] 2 commits <1:0 1:0 2:0 3:0 4+:0");
    switch_frames_cadence_restart(&c);
    EXPECT_EQ(line(c, nullptr), "");
    // the gap across the window's edge still counts in the next window
    commit(c, 5, "Widget", 1000000 + SWITCH_FRAMES_STILL_US + 1 + 3 * kPeriod);
    EXPECT_EQ(line(c, nullptr), "Widget[5] 1 commits <1:0 1:0 2:0 3:1 4+:0");
}

TEST(Frames, MostCommitsFirst)
{
    switch_frames_cadence c = {};
    uint64_t t = 1000000;
    for (int pid = 1; pid <= SWITCH_FRAMES_CLIENTS; pid++)
        for (int i = 0; i < pid; i++)
            commit(c, pid, "p", t + uint64_t(i) * 20000, 1);
    EXPECT_EQ(c.used, uint32_t(SWITCH_FRAMES_CLIENTS));
    EXPECT_EQ(line(c, nullptr), "p[8] 8 commits <1:0 1:7 2:0 3:0 4+:0, p[7] 7 commits <1:0 1:6 2:0 3:0 4+:0, "
                                "p[6] 6 commits <1:0 1:5 2:0 3:0 4+:0, p[5] 5 commits <1:0 1:4 2:0 3:0 4+:0");
}

// The first senders after boot (launchd, notifyd, ...) go quiet; a client that starts drawing later takes the place of
// the one quiet the longest, and the others keep their counts.
TEST(Frames, ANinthClientDisplacesTheIdlest)
{
    switch_frames_cadence c = {};
    uint64_t t = 1000000;
    for (int pid = 1; pid <= SWITCH_FRAMES_CLIENTS; pid++)
        commit(c, pid, "boot", t + uint64_t(pid) * 1000, 1);
    t += 100000;
    for (int frame = 0; frame < 3; frame++, t += kPeriod)
        for (int pid = 1; pid <= SWITCH_FRAMES_CLIENTS; pid++)
            if (pid != 3)
                commit(c, pid, "boot", t + uint64_t(pid) * 10, 1);
    for (int frame = 0; frame < 4; frame++, t += kPeriod)
        commit(c, 27, "SpringBoard", t);
    EXPECT_EQ(c.used, uint32_t(SWITCH_FRAMES_CLIENTS));
    EXPECT_EQ(c.client[2].pid, 27);
    EXPECT_EQ(c.client[2].commits, 4u);
    EXPECT_EQ(c.client[0].commits, 4u);
    EXPECT_THAT(line(c, nullptr), HasSubstr("SpringBoard[27] 4 commits <1:0 1:3 2:0 3:0 4+:0"));
    for (const switch_frames_client &e : c.client)
        EXPECT_NE(e.pid, 3);
}

TEST(Frames, DeliveryIsTheOldestPendingReportToTheNextEventMessage)
{
    switch_frames_cadence c = {};
    switch_frames_delivery d = {};
    switch_frames_note_delivery(&d, 27, "SpringBoard", 500);  // nothing pending: no delivery
    switch_frames_note_report(&d, 1000);
    switch_frames_note_report(&d, 6000);  // still waiting on the first one
    switch_frames_note_delivery(&d, 27, "SpringBoard", 9000);
    switch_frames_note_report(&d, 12000);
    switch_frames_note_delivery(&d, 27, "SpringBoard", 12300);
    EXPECT_EQ(d.latency.runs, 2u);
    EXPECT_EQ(d.latency.max_us, 8000u);
    EXPECT_EQ(line(c, &d), "delivery 2/3 reports p50/p99/max 512/8000/8000 us to SpringBoard[27]");
}

TEST(Frames, LinesAreCutAndTerminated)
{
    switch_frames_cadence c = {};
    switch_frames_delivery d = {};
    commit(c, 27, "a-process-name-longer-than-sixteen", 1000000);
    switch_frames_note_report(&d, 1);
    switch_frames_note_delivery(&d, 27, "x", 2);
    EXPECT_STREQ(c.client[0].name, "a-process-name-l");
    for (size_t size = 1; size < 64; size++)
        EXPECT_LT(line(c, &d, size).size(), size);
}

TEST(FramesSource, BothTreesFeedItUnderThePortLock)
{
    for (const char *path : {IPC_KMSG_10063, IPC_KMSG_12377}) {
        const std::string s = source(path);
        EXPECT_THAT(s, HasSubstr("#include <switch/frames.h>"));
        EXPECT_THAT(
            s, HasSubstr("\tlck_spin_unlock(&switch_render_senders_lock);\n\tswitch_frames_render_message(task, id, "
                         "task->itk_space == switch_render_server_space);\n"));
        const size_t send = s.find("\nipc_kmsg_send(\n");
        ASSERT_NE(send, std::string::npos);
        const std::string body = s.substr(send, s.find("\n}\n", send) - send);
        const size_t sent = body.find("switch_frames_render_server_sent(port);");
        ASSERT_NE(sent, std::string::npos);
        EXPECT_LT(body.find("ip_mq_lock(port);"), sent);
        EXPECT_LT(sent, body.find("ipc_mqueue_send_locked(&port->ip_messages, kmsg,"));
    }
}

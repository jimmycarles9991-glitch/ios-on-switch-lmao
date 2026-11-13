// drivers/SwitchJoyCon/SwitchJoyConProbe.h: the probe's stages against model Joy-Cons that answer only when asked, or
// push by themselves, or push 0x3F reports on presses.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../SwitchJoyConProbe.h"

namespace {

struct Packet {
    uint64_t at;
    std::vector<uint8_t> bytes;
};

std::vector<uint8_t> railPacket(uint8_t cmd, uint8_t sub, uint8_t status, std::vector<uint8_t> payload = {})
{
    std::vector<uint8_t> p = {0x19, 0x81, 0x03, 0, 0, cmd, sub, 0, 0, status, 0, 0};
    p.insert(p.end(), payload.begin(), payload.end());
    return p;
}

std::vector<uint8_t> report(uint8_t id, uint8_t subcommand = 0)
{
    std::vector<uint8_t> r(49, 0);
    r[0] = id;
    if (id == 0x21) {
        r[13] = 0x80;
        r[14] = subcommand;
    }
    return railPacket(0x92, 0, 0, r);
}

// A rail Joy-Con: answers 1F with 0x30 (not in 0x3F mode), 91/xx with 94/xx (91/12 with a rate other than 5/10/15:
// status 1, hekate joycon.c 311), subcommands with 0x21; optionally pushes 0x30 at its rate, or 0x3F on presses;
// leaves HID mode 2 s after the host's last 0x92 packet.
struct JoyCon {
    bool pushes = false;
    std::vector<uint64_t> presses;
    uint32_t rate = 15;
    bool simple = false, alive = true;
    uint64_t lastHid = 0, lastPush = 0;
    std::vector<Packet> out;

    void hear(uint64_t now, uint8_t action, const switch_joycon_probe_stage *stage)
    {
        if (now - lastHid > 2000)
            alive = false;
        if (!alive)
            return;
        if (action == SWITCH_JOYCON_PROBE_REQUEST) {
            lastHid = now;
            if (!simple)
                out.push_back({now + 5, report(0x30)});
        } else if (action == SWITCH_JOYCON_PROBE_INIT) {
            uint8_t status = 0;
            if (stage->command == 0x12) {
                status = stage->argument == 5 || stage->argument == 10 || stage->argument == 15 ? 0 : 1;
                if (!status)
                    rate = stage->argument;
            }
            out.push_back({now + 5, railPacket(0x94, stage->command, status)});
        } else if (action == SWITCH_JOYCON_PROBE_MODE) {
            lastHid = now;
            simple = stage->argument == 0x3f;
            out.push_back({now + 5, report(0x21, stage->command)});
        }
    }

    void run(uint64_t now)
    {
        if (now - lastHid > 2000)
            alive = false;
        if (!alive)
            return;
        if (pushes && !simple && now - lastPush >= rate) {
            lastPush = now;
            out.push_back({now, report(0x30)});
        }
        for (uint64_t press : presses) {
            if (simple && press == now)
                out.push_back({now, report(0x3f)});
        }
    }
};

struct ProbeRun {
    switch_joycon_probe probe{};
    uint64_t end = 0, longestWithoutHid = 0;
    uint64_t windows[SWITCH_JOYCON_PROBE_STAGES] = {};  // when each stage's window opened
};

ProbeRun probe(JoyCon &joycon)
{
    ProbeRun run;
    switch_joycon_probe_start(&run.probe);
    uint64_t lastHid = 0;
    joycon.lastHid = 0;
    for (uint64_t now = 0; now < 30000 && run.probe.active; ++now) {
        joycon.run(now);
        for (auto it = joycon.out.begin(); it != joycon.out.end();) {
            if (it->at > now) {
                ++it;
                continue;
            }
            switch_joycon_probe_packet(&run.probe, it->bytes.data(), it->bytes.size(), now);
            it = joycon.out.erase(it);
        }
        if (now % 15 != 0)
            continue;
        const switch_joycon_probe_stage *stage = nullptr;
        const uint8_t action = switch_joycon_probe_tick(&run.probe, now, &stage);
        if (action == SWITCH_JOYCON_PROBE_REQUEST || action == SWITCH_JOYCON_PROBE_MODE) {
            run.longestWithoutHid = std::max(run.longestWithoutHid, now - lastHid);
            lastHid = now;
        }
        joycon.hear(now, action, stage);
        if (run.probe.active && run.probe.phase == SWITCH_JOYCON_PROBE_WINDOW && !run.windows[run.probe.stage])
            run.windows[run.probe.stage] = now;
        run.end = now;
    }
    return run;
}

const switch_joycon_probe_result &result(const ProbeRun &run, const char *name)
{
    for (unsigned i = 0; i < SWITCH_JOYCON_PROBE_STAGES; ++i) {
        if (!strcmp(switch_joycon_probe_stages[i].name, name))
            return run.probe.results[i];
    }
    ADD_FAILURE() << name;
    return run.probe.results[0];
}

TEST(SwitchJoyConProbe, AJoyConThatOnlyAnswersShowsNothingUnaskedAndKeepsHidMode)
{
    JoyCon joycon;
    const ProbeRun run = probe(joycon);
    EXPECT_TRUE(run.probe.done);
    EXPECT_LT(run.end, 10000u);
    EXPECT_LE(run.longestWithoutHid, SWITCH_JOYCON_PROBE_SILENT_MS + 15u);
    for (unsigned i = 0; i < SWITCH_JOYCON_PROBE_STAGES; ++i) {
        const switch_joycon_probe_result &r = run.probe.results[i];
        const char *name = switch_joycon_probe_stages[i].name;
        EXPECT_EQ(r.unasked, 0u) << name;
        EXPECT_EQ(r.answered, switch_joycon_probe_stages[i].action != SWITCH_JOYCON_PROBE_NOTHING) << name;
        EXPECT_EQ(r.resumed, strcmp(name, "simple") != 0) << name;
    }
    EXPECT_EQ(result(run, "rate15").status, 0u);
    EXPECT_EQ(result(run, "rate5").status, 0u);
    EXPECT_EQ(result(run, "rate30").status, 1u);
    EXPECT_EQ(result(run, "rate100").status, 1u);
    EXPECT_EQ(result(run, "simple").status, 0x80u);
    EXPECT_EQ(result(run, "simple").keepalives, 2u);
    EXPECT_EQ(result(run, "simple").kept, 0u);
    EXPECT_TRUE(joycon.alive);
}

TEST(SwitchJoyConProbe, AJoyConThatPushesIsCountedAtItsRate)
{
    JoyCon joycon;
    joycon.pushes = true;
    const ProbeRun run = probe(joycon);
    EXPECT_NEAR(result(run, "silence").unasked, 1000 / 15, 3);
    EXPECT_GE(result(run, "rate5").unasked, 500u / 5u);
    EXPECT_LE(result(run, "rate5").unasked, 500u / 5u + 10u);
    EXPECT_LE(result(run, "silence").first_ms, 15u);
}

TEST(SwitchJoyConProbe, PressesIn0x3FModeAreCountedAsSimpleReports)
{
    JoyCon joycon;
    const ProbeRun first = probe(joycon);
    const uint64_t window = first.windows[8];
    ASSERT_STREQ(switch_joycon_probe_stages[8].name, "simple");
    ASSERT_NE(window, 0u);
    JoyCon pressed;
    pressed.presses = {window + 500, window + 1200, window + 2200};
    const ProbeRun run = probe(pressed);
    EXPECT_EQ(result(run, "simple").simple, 3u);
    EXPECT_EQ(result(run, "simple").unasked, 3u);
}

TEST(SwitchJoyConProbe, ResultsFormatAsMetricFields)
{
    JoyCon joycon;
    const ProbeRun run = probe(joycon);
    char line[320];
    ASSERT_GT(switch_joycon_probe_format(&run.probe, 3, line, sizeof(line)), 0);
    EXPECT_EQ(std::string(line), "rate30_answered=1 rate30_status=1 rate30_data=00000000 rate30_unasked=0 "
                                 "rate30_simple=0 rate30_first_ms=-1 rate30_keepalives=0 rate30_kept=0 "
                                 "rate30_resumed=1");
}

TEST(SwitchJoyConProbe, TheStateLineNamesEachRail)
{
    switch_joycon_probe idle{}, right{};
    const switch_joycon_probe *probes[2] = {&right, &idle};
    char line[4096];
    switch_joycon_probe_state(probes, "RL", 2, line, sizeof(line));
    EXPECT_STREQ(line, "idle");
    switch_joycon_probe_start(&right);
    switch_joycon_probe_state(probes, "RL", 2, line, sizeof(line));
    EXPECT_STREQ(line, "running; R stage=silence");
    JoyCon joycon;
    right = probe(joycon).probe;
    switch_joycon_probe_state(probes, "RL", 2, line, sizeof(line));
    const std::string text(line);
    EXPECT_EQ(text.rfind("done; R silence_answered=0 ", 0), 0u) << text;
    EXPECT_NE(text.find(" full_resumed=1"), std::string::npos) << text;
    EXPECT_EQ(text.find("; L"), std::string::npos);
    char tiny[40];
    switch_joycon_probe_state(probes, "RL", 2, tiny, sizeof(tiny));
    EXPECT_EQ(strlen(tiny), sizeof(tiny) - 1);
}

}  // namespace

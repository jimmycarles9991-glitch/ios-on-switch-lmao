// SPDX-License-Identifier: BSD-2-Clause-Patent
// What a running rail Joy-Con does when the host stops asking (debug kernels: SwitchJoyConRail.cpp runs it when HW Test
// input.joycon_probe writes debug.switch_joycon_probe; the host test test/switch_joycon_probe_test.cpp drives this
// header). The sources say the console asks every 15 ms and never show the Joy-Con speaking first (deku
// packet_parse/j2c.txt), but none of them stopped asking; this does, on the device.
//
// Each stage sends one command, waits up to a second for its answer, then sends nothing for its window (but the 1F
// keepalives a stage names) and counts every packet the Joy-Con sends unasked, then sends one 1F whose answer shows the
// Joy-Con kept HID mode. No window, with its wait, passes 1.5 s without a 0x92 packet: the Joy-Con's HID timeout is 2 s
// from the last one (hekate joycon.c 431, 1255-1257).
//   silence  nothing            does it push at its session's 15 ms (91/12 at the handshake)?
//   rate15   91/12 15 ms        the same right after the interval command
//   rate5    91/12 5 ms         hekate joycon.c 192: "5/10/15 supported"; does a faster rate make it push?
//   rate30   91/12 30 ms        longer intervals: hekate 311, status 1 is a wrong rate
//   rate100  91/12 100 ms
//   getrate  91/13              the rate it holds now (hekate's JC_WIRED_CMD_GET_HIDRATE)
//   wake     91/07              hekate's JC_WIRED_CMD_WAKE_REASON: what it answers
//   restore  91/12 15 ms
//   simple   subcommand 03 3F   input report mode 0x3F: does it push on a press (the HW Test asks for presses now), and
//                               do 1F keepalives a second apart, answered or not, keep HID mode for 3 s?
//   full     subcommand 03 30   back to the rail's mode; the rail's silence rule handshakes it again if it is gone
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum switch_joycon_probe_action {
    SWITCH_JOYCON_PROBE_OFF,      // not probing: the rail ticks as always
    SWITCH_JOYCON_PROBE_NOTHING,  // send nothing this tick
    SWITCH_JOYCON_PROBE_REQUEST,  // send 1F
    SWITCH_JOYCON_PROBE_INIT,     // send 91/`command` with `rate` (91/12 only)
    SWITCH_JOYCON_PROBE_MODE      // send subcommand 03 `mode`
};

struct switch_joycon_probe_stage {
    const char *name;
    uint8_t action;  // INIT, MODE or NOTHING
    uint8_t command, argument;
    uint16_t window_ms, keepalive_ms;
};

#define SWITCH_JOYCON_PROBE_STAGES 10

static const struct switch_joycon_probe_stage switch_joycon_probe_stages[SWITCH_JOYCON_PROBE_STAGES] = {
    {"silence", SWITCH_JOYCON_PROBE_NOTHING, 0, 0, 1000, 0},
    {"rate15", SWITCH_JOYCON_PROBE_INIT, 0x12, 15, 1000, 0},
    {"rate5", SWITCH_JOYCON_PROBE_INIT, 0x12, 5, 500, 0},
    {"rate30", SWITCH_JOYCON_PROBE_INIT, 0x12, 30, 500, 0},
    {"rate100", SWITCH_JOYCON_PROBE_INIT, 0x12, 100, 500, 0},
    {"getrate", SWITCH_JOYCON_PROBE_INIT, 0x13, 0, 0, 0},
    {"wake", SWITCH_JOYCON_PROBE_INIT, 0x07, 0, 0, 0},
    {"restore", SWITCH_JOYCON_PROBE_INIT, 0x12, 15, 0, 0},
    {"simple", SWITCH_JOYCON_PROBE_MODE, 0x03, 0x3f, 3000, 1000},
    {"full", SWITCH_JOYCON_PROBE_MODE, 0x03, 0x30, 0, 0},
};

#define SWITCH_JOYCON_PROBE_ANSWER_MS 1000u
#define SWITCH_JOYCON_PROBE_RESUME_MS 100u
#define SWITCH_JOYCON_PROBE_SILENT_MS 1500u

struct switch_joycon_probe_result {
    bool answered, resumed;
    uint8_t status;   // 0x94's status byte, or the 0x21's ACK
    uint8_t data[4];  // the answer's first payload bytes (0x21: from its subcommand data)
    uint16_t unasked, simple, first_ms, keepalives, kept;
};

enum {
    SWITCH_JOYCON_PROBE_SEND,
    SWITCH_JOYCON_PROBE_ANSWER,
    SWITCH_JOYCON_PROBE_WINDOW,
    SWITCH_JOYCON_PROBE_RESUME
};

struct switch_joycon_probe {
    bool active, done;
    uint8_t stage, phase;
    uint64_t phase_at, window_end, keepalive_at, keepalive_until;
    struct switch_joycon_probe_result results[SWITCH_JOYCON_PROBE_STAGES];
};

static inline void switch_joycon_probe_start(struct switch_joycon_probe *p)
{
    memset(p, 0, sizeof(*p));
    p->active = true;
}

static inline void switch_joycon_probe_phase(struct switch_joycon_probe *p, uint8_t phase, uint64_t now)
{
    p->phase = phase;
    p->phase_at = now;
}

// The window opens: it ends `window_ms` later, and at most SILENT_MS after the stage's command went out.
static inline void switch_joycon_probe_window(struct switch_joycon_probe *p, uint64_t now)
{
    const struct switch_joycon_probe_stage *stage = &switch_joycon_probe_stages[p->stage];
    const uint64_t sent = p->phase == SWITCH_JOYCON_PROBE_ANSWER ? p->phase_at : now;
    const uint64_t end = now + stage->window_ms, latest = sent + SWITCH_JOYCON_PROBE_SILENT_MS;
    p->window_end = stage->keepalive_ms || end < latest ? end : latest;
    p->keepalive_at = now + stage->keepalive_ms;
    switch_joycon_probe_phase(p, SWITCH_JOYCON_PROBE_WINDOW, now);
}

// At a tick of the rail's clock (15 ms while probing): what to send. `out` gets the stage for INIT and MODE.
static inline uint8_t switch_joycon_probe_tick(struct switch_joycon_probe *p, uint64_t now,
                                               const struct switch_joycon_probe_stage **out)
{
    if (!p->active)
        return SWITCH_JOYCON_PROBE_OFF;
    const struct switch_joycon_probe_stage *stage = &switch_joycon_probe_stages[p->stage];
    struct switch_joycon_probe_result *r = &p->results[p->stage];
    switch (p->phase) {
    case SWITCH_JOYCON_PROBE_SEND:
        *out = stage;
        if (stage->action == SWITCH_JOYCON_PROBE_NOTHING) {
            switch_joycon_probe_window(p, now);
            return SWITCH_JOYCON_PROBE_NOTHING;
        }
        switch_joycon_probe_phase(p, SWITCH_JOYCON_PROBE_ANSWER, now);
        return stage->action;
    case SWITCH_JOYCON_PROBE_ANSWER:
        if (now - p->phase_at >= SWITCH_JOYCON_PROBE_ANSWER_MS)
            switch_joycon_probe_window(p, now);
        return SWITCH_JOYCON_PROBE_NOTHING;
    case SWITCH_JOYCON_PROBE_WINDOW:
        if (now >= p->window_end) {
            switch_joycon_probe_phase(p, SWITCH_JOYCON_PROBE_RESUME, now);
            return SWITCH_JOYCON_PROBE_REQUEST;
        }
        if (stage->keepalive_ms && now >= p->keepalive_at) {
            p->keepalive_at = now + stage->keepalive_ms;
            p->keepalive_until = now + SWITCH_JOYCON_PROBE_RESUME_MS;
            ++r->keepalives;
            return SWITCH_JOYCON_PROBE_REQUEST;
        }
        return SWITCH_JOYCON_PROBE_NOTHING;
    default:
        if (!r->resumed && now - p->phase_at < SWITCH_JOYCON_PROBE_RESUME_MS)
            return SWITCH_JOYCON_PROBE_NOTHING;
        if (++p->stage == SWITCH_JOYCON_PROBE_STAGES) {
            p->active = false;
            p->done = true;
            return SWITCH_JOYCON_PROBE_OFF;
        }
        switch_joycon_probe_phase(p, SWITCH_JOYCON_PROBE_SEND, now);
        return switch_joycon_probe_tick(p, now, out);
    }
}

// The packet answers the stage's command: 94/command, or a 0x21 reply to subcommand `command`.
static inline bool switch_joycon_probe_is_answer(const struct switch_joycon_probe_stage *stage, const uint8_t *packet,
                                                 size_t length)
{
    if (stage->action == SWITCH_JOYCON_PROBE_INIT)
        return packet[5] == 0x94 && packet[6] == stage->command;
    return packet[5] == 0x92 && length >= 12 + 15 && packet[12] == 0x21 && packet[12 + 14] == stage->command;
}

// Every packet of the Joy-Con's while probing (the whole rail packet, magic on; at least 7 bytes).
static inline void switch_joycon_probe_packet(struct switch_joycon_probe *p, const uint8_t *packet, size_t length,
                                              uint64_t now)
{
    if (!p->active)
        return;
    const struct switch_joycon_probe_stage *stage = &switch_joycon_probe_stages[p->stage];
    struct switch_joycon_probe_result *r = &p->results[p->stage];
    const bool report = packet[5] == 0x92 && length > 12;
    if (p->phase == SWITCH_JOYCON_PROBE_ANSWER && length >= 12 &&
        switch_joycon_probe_is_answer(stage, packet, length)) {
        r->answered = true;
        const bool reply = packet[5] == 0x92;
        r->status = reply ? packet[12 + 13] : packet[9];
        const uint8_t *data = packet + 12 + (reply ? 15 : 0);
        for (unsigned i = 0; i < 4 && (size_t)(data - packet) + i < length; ++i)
            r->data[i] = data[i];
        switch_joycon_probe_window(p, now);
        return;
    }
    if (p->phase == SWITCH_JOYCON_PROBE_RESUME && report && !r->resumed) {
        r->resumed = true;
        return;
    }
    if (p->phase == SWITCH_JOYCON_PROBE_WINDOW && report && now < p->keepalive_until && packet[12] != 0x3f) {
        p->keepalive_until = 0;
        ++r->kept;
        return;
    }
    if (!r->unasked)
        r->first_ms = (uint16_t)(now - p->phase_at);
    ++r->unasked;
    if (report && packet[12] == 0x3f)
        ++r->simple;
}

// One stage's result as `key=value` fields (the HW Test's metrics), each prefixed with the stage's name.
static inline int switch_joycon_probe_format(const struct switch_joycon_probe *p, unsigned stage, char *out,
                                             size_t size)
{
    const struct switch_joycon_probe_result *r = &p->results[stage];
    const char *name = switch_joycon_probe_stages[stage].name;
    return snprintf(out, size,
                    "%s_answered=%d %s_status=%u %s_data=%02x%02x%02x%02x %s_unasked=%u %s_simple=%u %s_first_ms=%d "
                    "%s_keepalives=%u %s_kept=%u %s_resumed=%d",
                    name, r->answered, name, r->status, name, r->data[0], r->data[1], r->data[2], r->data[3], name,
                    r->unasked, name, r->simple, name, r->unasked ? (int)r->first_ms : -1, name, r->keepalives, name,
                    r->kept, name, r->resumed);
}

// debug.switch_joycon_probe's text over the rails' probes: "idle"; "running" and each probing rail's stage
// ("running; R stage=rate30"); or "done" and each rail's results ("done; R silence_answered=0 ...; L ...").
static inline void switch_joycon_probe_state(const struct switch_joycon_probe *const probes[], const char *letters,
                                             unsigned count, char *out, size_t size)
{
    bool running = false, done = false;
    for (unsigned i = 0; i < count; ++i) {
        running |= probes[i]->active;
        done |= probes[i]->done;
    }
    size_t used = (size_t)snprintf(out, size, "%s", running ? "running" : done ? "done" : "idle");
    for (unsigned i = 0; i < count && used < size; ++i) {
        const struct switch_joycon_probe *p = probes[i];
        if (running && p->active) {
            used += (size_t)snprintf(out + used, size - used, "; %c stage=%s", letters[i],
                                     switch_joycon_probe_stages[p->stage].name);
        }
        if (running || !p->done || used >= size)
            continue;
        used += (size_t)snprintf(out + used, size - used, "; %c", letters[i]);
        for (unsigned stage = 0; stage < SWITCH_JOYCON_PROBE_STAGES && used + 1 < size; ++stage) {
            out[used++] = ' ';
            out[used] = 0;
            used += (size_t)switch_joycon_probe_format(p, stage, out + used, size - used);
        }
    }
}

#ifdef __cplusplus
}
#endif

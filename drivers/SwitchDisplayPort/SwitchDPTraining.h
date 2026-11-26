// SPDX-License-Identifier: GPL-2.0-only
// DisplayPort link training (DP 1.2 section 3.5.1.2): clock recovery with TPS1, then channel equalisation with TPS2
// (TPS3 at HBR2 when the sink has it), following the sink's adjust requests. Plain C for the host tests; the caller
// drives SOR1's pattern and lane drive, writes the DPCD, waits and reads the status back, one step per event.
#ifndef SWITCH_DP_TRAINING_H
#define SWITCH_DP_TRAINING_H

#include <stdbool.h>
#include <stdint.h>

#define SWITCH_DPCD_REV                      0x000u
#define SWITCH_DPCD_MAX_LINK_RATE            0x001u
#define SWITCH_DPCD_MAX_LANE_COUNT           0x002u
#define SWITCH_DPCD_MAX_DOWNSPREAD           0x003u
#define SWITCH_DPCD_TRAINING_LANE0_1_SET2     0x10Fu
#define SWITCH_DPCD_TRAINING_AUX_RD_INTERVAL 0x00Eu
#define SWITCH_DPCD_LINK_BW_SET              0x100u
#define SWITCH_DPCD_TRAINING_PATTERN_SET     0x102u
#define SWITCH_DPCD_TRAINING_LANE0_SET       0x103u
#define SWITCH_DPCD_DOWNSPREAD_CTRL          0x107u
#define SWITCH_DPCD_SINK_COUNT               0x200u
#define SWITCH_DPCD_LANE0_1_STATUS           0x202u
#define SWITCH_DPCD_SET_POWER                0x600u
#define SWITCH_DPCD_EXT_CAP_BASE             0x2200u
#define SWITCH_DPCD_EXT_CAP_PRESENT          (1u << 7)
#define SWITCH_DPCD_ENHANCED_FRAME           (1u << 7)
#define SWITCH_DPCD_TPS3                     (1u << 6)
#define SWITCH_DPCD_SCRAMBLING_DISABLE       (1u << 5)
#define SWITCH_DPCD_MAX_SWING_REACHED        (1u << 2)
#define SWITCH_DPCD_MAX_PRE_EMPHASIS_REACHED (1u << 5)
#define SWITCH_DPCD_CR_DONE                  (1u << 0)
#define SWITCH_DPCD_EQ_DONE                  (1u << 1)
#define SWITCH_DPCD_SYMBOL_LOCKED            (1u << 2)
#define SWITCH_DPCD_INTERLANE_ALIGN_DONE     (1u << 0)

#define SWITCH_DP_CR_US             100u
#define SWITCH_DP_CR_SAME_SWING_MAX 5u
#define SWITCH_DP_CR_TRIES_MAX      10u
#define SWITCH_DP_EQ_TRIES_MAX      5u

static inline bool switch_dp_caps_update(uint8_t caps[16], const uint8_t *reply, bool extended)
{
    if (extended && reply[SWITCH_DPCD_REV] < caps[SWITCH_DPCD_REV])
        return false;
    for (unsigned i = 0; i < 16; ++i) {
        caps[i] = reply[i];
    }
    return !extended && (caps[SWITCH_DPCD_TRAINING_AUX_RD_INTERVAL] & SWITCH_DPCD_EXT_CAP_PRESENT) != 0;
}

enum switch_dp_train_phase {
    SWITCH_DP_TRAIN_CLOCK_RECOVERY,
    SWITCH_DP_TRAIN_EQUALISATION,
    SWITCH_DP_TRAIN_TRAINED,
    SWITCH_DP_TRAIN_FAILED,
};

enum switch_dp_train_act {
    SWITCH_DP_TRAIN_DRIVE,  // program SOR1 (pattern, drive), write DPCD 0x102.. (`dpcd`), wait `wait_us`, read status
    SWITCH_DP_TRAIN_DONE,   // trained: SOR1's pattern off with the scrambler, DPCD 0x102 = 0 (in `dpcd`)
    SWITCH_DP_TRAIN_FAIL,   // this rate does not train: a lower one, or no link
};

struct switch_dp_train {
    enum switch_dp_train_phase phase;
    uint8_t lanes;
    bool tps3;
    uint32_t cr_wait_us, eq_wait_us;
    uint8_t swing[4], emphasis[4], postcursor;
    unsigned tries, same_swing;
    const char *why;
};

struct switch_dp_train_out {
    enum switch_dp_train_act act;
    uint8_t pattern;  // 0 none, 1 TPS1, 2 TPS2, 3 TPS3
    uint32_t dpcd_addr;
    uint8_t dpcd[5];  // TRAINING_PATTERN_SET + TRAINING_LANEn_SET on pattern change, else TRAINING_LANEn_SET only
    uint8_t dpcd_len;
    uint32_t wait_us;
};

static inline struct switch_dp_train_out switch_dp_train_drive(const struct switch_dp_train *t, bool write_pattern)
{
    struct switch_dp_train_out out = {
        SWITCH_DP_TRAIN_DRIVE,
        1,
        write_pattern ? SWITCH_DPCD_TRAINING_PATTERN_SET : SWITCH_DPCD_TRAINING_LANE0_SET,
        {0},
        0,
        t->cr_wait_us,
    };
    if (t->phase == SWITCH_DP_TRAIN_EQUALISATION) {
        out.pattern = t->tps3 ? 3 : 2;
        out.wait_us = t->eq_wait_us;
    }
    unsigned offset = 0;
    if (write_pattern) {
        out.dpcd[0] = (uint8_t)(out.pattern | SWITCH_DPCD_SCRAMBLING_DISABLE);
        offset = 1;
    }
    for (unsigned lane = 0; lane < t->lanes; ++lane) {
        uint8_t set = (uint8_t)(t->swing[lane] | (t->emphasis[lane] << 3));
        if (t->swing[lane] == 3) {
            set |= SWITCH_DPCD_MAX_SWING_REACHED;
        }
        if (t->emphasis[lane] == 3 || t->swing[lane] + t->emphasis[lane] >= 3) {
            set |= SWITCH_DPCD_MAX_PRE_EMPHASIS_REACHED;
        }
        out.dpcd[offset + lane] = set;
    }
    out.dpcd_len = (uint8_t)(offset + t->lanes);
    return out;
}

static inline struct switch_dp_train_out switch_dp_train_start(struct switch_dp_train *t, uint8_t lanes, bool tps3,
                                                               uint8_t revision, uint8_t rd_interval)
{
    const uint32_t interval = (rd_interval & 0x7Fu) * 4000u;
    const struct switch_dp_train fresh = {
        SWITCH_DP_TRAIN_CLOCK_RECOVERY, lanes, tps3, revision >= 0x14 || !interval ? SWITCH_DP_CR_US : interval,
        interval ? interval : 400u, {0}, {0}, 0, 0, 0, 0};
    *t = fresh;
    return switch_dp_train_drive(t, true);
}

static inline uint8_t switch_dp_lane_status(const uint8_t *status, unsigned lane)
{
    return (uint8_t)((status[lane / 2] >> (4u * (lane % 2))) & 0xFu);
}

static inline bool switch_dp_all_lanes(const uint8_t *status, uint8_t lanes, uint8_t bits)
{
    for (unsigned lane = 0; lane < lanes; ++lane) {
        if ((switch_dp_lane_status(status, lane) & bits) != bits) {
            return false;
        }
    }
    return true;
}

static inline void switch_dp_train_set2(const struct switch_dp_train *t, uint8_t levels[2])
{
    levels[0] = levels[1] = 0;
    for (unsigned lane = 0; lane < t->lanes; ++lane) {
        const unsigned pc = (t->postcursor >> (lane * 2)) & 3u;
        levels[lane / 2] |= (uint8_t)((pc | (pc == 3 ? 4u : 0u)) << ((lane % 2) * 4));
    }
}

// The sink's adjust requests (DPCD 0x206/0x207), clamped to what SOR1's tables hold (swing + emphasis <= 3). True
// when the swing of any lane changed.
static inline bool switch_dp_train_adjust(struct switch_dp_train *t, const uint8_t *status)
{
    bool changed = false;
    if (t->tps3)
        t->postcursor = status[10];
    for (unsigned lane = 0; lane < t->lanes; ++lane) {
        const uint8_t request = (uint8_t)(status[4 + lane / 2] >> (4u * (lane % 2)));
        const uint8_t swing = request & 3u;
        uint8_t emphasis = (request >> 2) & 3u;
        if (swing + emphasis > 3) {
            emphasis = (uint8_t)(3u - swing);
        }
        changed = changed || swing != t->swing[lane];
        t->swing[lane] = swing;
        t->emphasis[lane] = emphasis;
    }
    return changed;
}

static inline struct switch_dp_train_out switch_dp_train_fail(struct switch_dp_train *t, const char *why)
{
    t->phase = SWITCH_DP_TRAIN_FAILED;
    t->why = why;
    struct switch_dp_train_out out = {SWITCH_DP_TRAIN_FAIL, 0, SWITCH_DPCD_TRAINING_PATTERN_SET, {0}, 1, 0};
    return out;
}

static inline struct switch_dp_train_out switch_dp_train_clock_recovery(struct switch_dp_train *t,
                                                                        const uint8_t *status)
{
    if (switch_dp_all_lanes(status, t->lanes, SWITCH_DPCD_CR_DONE)) {
        (void)switch_dp_train_adjust(t, status);
        t->phase = SWITCH_DP_TRAIN_EQUALISATION;
        t->tries = 0;
        return switch_dp_train_drive(t, true);
    }
    bool all_max_swing = true;
    for (unsigned lane = 0; lane < t->lanes; ++lane) {
        all_max_swing = all_max_swing && t->swing[lane] == 3;
    }
    const bool changed = switch_dp_train_adjust(t, status);
    if (all_max_swing && !changed) {
        return switch_dp_train_fail(t, "clock recovery failed at the highest swing");
    }
    t->same_swing = changed ? 0 : t->same_swing + 1;
    if (t->same_swing >= SWITCH_DP_CR_SAME_SWING_MAX || ++t->tries >= SWITCH_DP_CR_TRIES_MAX) {
        return switch_dp_train_fail(t, "clock recovery did not converge");
    }
    return switch_dp_train_drive(t, false);
}

static inline struct switch_dp_train_out switch_dp_train_equalisation(struct switch_dp_train *t, const uint8_t *status)
{
    if (!switch_dp_all_lanes(status, t->lanes, SWITCH_DPCD_CR_DONE)) {
        return switch_dp_train_fail(t, "clock recovery lost during equalisation");
    }
    const uint8_t locked = SWITCH_DPCD_EQ_DONE | SWITCH_DPCD_SYMBOL_LOCKED;
    if (switch_dp_all_lanes(status, t->lanes, locked) && (status[2] & SWITCH_DPCD_INTERLANE_ALIGN_DONE) != 0) {
        t->phase = SWITCH_DP_TRAIN_TRAINED;
        struct switch_dp_train_out out = {SWITCH_DP_TRAIN_DONE, 0, SWITCH_DPCD_TRAINING_PATTERN_SET, {0}, 1, 0};
        return out;
    }
    if (++t->tries >= SWITCH_DP_EQ_TRIES_MAX) {
        return switch_dp_train_fail(t, "channel equalisation did not converge");
    }
    (void)switch_dp_train_adjust(t, status);
    return switch_dp_train_drive(t, false);
}

// TPS3 includes ADJUST_REQUEST_POST_CURSOR2 (0x20c) in the same AUX status read.
static inline struct switch_dp_train_out switch_dp_train_status(struct switch_dp_train *t, const uint8_t *status)
{
    if (t->phase == SWITCH_DP_TRAIN_CLOCK_RECOVERY) {
        return switch_dp_train_clock_recovery(t, status);
    }
    if (t->phase == SWITCH_DP_TRAIN_EQUALISATION) {
        return switch_dp_train_equalisation(t, status);
    }
    return switch_dp_train_fail(t, "status after training ended");
}

// IRQ_HPD: the link still holds when every trained lane keeps clock recovery, equalisation and symbol lock and the
// lanes stay aligned (DPCD 0x202..0x204); otherwise it is trained again.
static inline bool switch_dp_link_holds(const uint8_t *status, uint8_t lanes)
{
    const uint8_t all = SWITCH_DPCD_CR_DONE | SWITCH_DPCD_EQ_DONE | SWITCH_DPCD_SYMBOL_LOCKED;
    return switch_dp_all_lanes(status, lanes, all) && (status[2] & SWITCH_DPCD_INTERLANE_ALIGN_DONE) != 0;
}

#endif

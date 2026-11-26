// SPDX-License-Identifier: GPL-2.0-only
// SwitchDPLink.h (link choice, SOR1 transfer units, PLL_D2) and SwitchDPTraining.h against a model sink.
#include <gtest/gtest.h>

#include <cstdint>

#include "../SwitchDPLink.h"
#include "../SwitchDPTraining.h"

namespace {

TEST(Capabilities, UsesOneCoherentReceiverCapabilityBlock)
{
    uint8_t caps[16] = {};
    uint8_t base[16] = {0x12, 0x14, 0xC4, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 2, 15};
    EXPECT_FALSE(switch_dp_caps_update(caps, base, false));
    for (unsigned i = 0; i < 16; ++i) {
        EXPECT_EQ(caps[i], base[i]);
    }
    base[SWITCH_DPCD_TRAINING_AUX_RD_INTERVAL] |= SWITCH_DPCD_EXT_CAP_PRESENT;
    EXPECT_TRUE(switch_dp_caps_update(caps, base, false));
    uint8_t extended[16] = {0x14, 0x0a, 0x82, 0};
    extended[SWITCH_DPCD_TRAINING_AUX_RD_INTERVAL] = 3;
    EXPECT_FALSE(switch_dp_caps_update(caps, extended, true));
    for (unsigned i = 0; i < 16; ++i)
        EXPECT_EQ(caps[i], extended[i]);
    EXPECT_FALSE(switch_dp_caps_update(caps, base, true));
    EXPECT_EQ(caps[2], extended[2]);
    EXPECT_EQ(caps[SWITCH_DPCD_TRAINING_AUX_RD_INTERVAL], 3u);
}

TEST(Link, VisitsOnlyFeasibleTuplesOnce)
{
    switch_dp_link link{};
    switch_dp_tu tu{};
    unsigned cursor = 0, tries = 0;
    bool seen[3][5] = {};
    while (switch_dp_next_link(148500, 1920, 2200, SWITCH_DP_RATE_HBR2, 4, true, true, &cursor, &link, &tu)) {
        if (!tries) {
            EXPECT_EQ(link.rate, SWITCH_DP_RATE_HBR);
            EXPECT_EQ(link.lanes, 2);
        }
        const unsigned rate = link.rate == SWITCH_DP_RATE_RBR ? 0 : link.rate == SWITCH_DP_RATE_HBR ? 1 : 2;
        EXPECT_FALSE(seen[rate][link.lanes]);
        seen[rate][link.lanes] = true;
        EXPECT_GT(switch_dp_payload_kbps(link.rate, link.lanes) * 995 / 1000, 148500u * 24);
        ++tries;
    }
    EXPECT_EQ(tries, 6u);
    cursor = 0;
    while (switch_dp_next_link(148500, 1920, 2200, SWITCH_DP_RATE_HBR2, 2, true, true, &cursor, &link, &tu))
        EXPECT_LE(link.lanes, 2);
    cursor = 0;
    EXPECT_FALSE(switch_dp_next_link(148500, 1920, 2200, SWITCH_DP_RATE_RBR, 2, true, false, &cursor, &link, &tu));
    cursor = 0;
    EXPECT_FALSE(switch_dp_next_link(53800, 1280, 1650, SWITCH_DP_RATE_RBR, 1, false, true, &cursor, &link, &tu));
    cursor = 0;
    EXPECT_TRUE(switch_dp_next_link(53800, 1280, 1650, SWITCH_DP_RATE_RBR, 1, false, false, &cursor, &link, &tu));
}

TEST(Link, CapsAtTheFastestBoardRate)
{
    EXPECT_EQ(switch_dp_rate_cap(0x1E), SWITCH_DP_RATE_HBR2);
    EXPECT_EQ(switch_dp_rate_cap(0), 0);
    EXPECT_EQ(switch_dp_rate_cap(0x0A), SWITCH_DP_RATE_HBR);
}

struct TuGolden {
    uint32_t pixel_khz, hactive, htotal;
    uint8_t rate, lanes;
    bool enhanced;
    switch_dp_tu tu;
};

// Golden fractions from integer enumeration of the 27.8.41 encodings (both polarities, F=1..15).
TEST(TransferUnit, MatchesHardwareSymbolFractions)
{
    const TuGolden cases[] = {
        {148500, 1920, 2200, SWITCH_DP_RATE_HBR, 2, true, {64, 52, 5, 1, 14, 487, 3423}},
        {148500, 1920, 2200, SWITCH_DP_RATE_RBR, 4, true, {64, 43, 1, 0, 18, 291, 2054}},
        {74250, 1280, 1650, SWITCH_DP_RATE_RBR, 2, true, {64, 43, 1, 0, 18, 783, 2716}},
        {74250, 1280, 1650, SWITCH_DP_RATE_HBR, 2, true, {60, 24, 4, 1, 19, 1311, 4541}},
        {25175, 640, 800, SWITCH_DP_RATE_RBR, 1, false, {37, 17, 4, 0, 14, 972, 3917}},
    };
    for (const TuGolden &c : cases) {
        switch_dp_tu tu{};
        ASSERT_TRUE(switch_dp_tu_config(c.pixel_khz, c.hactive, c.htotal, c.rate, c.lanes, c.enhanced, &tu));
        EXPECT_EQ(tu.tu_size, c.tu.tu_size) << c.pixel_khz;
        EXPECT_EQ(tu.active_count, c.tu.active_count) << c.pixel_khz;
        EXPECT_EQ(tu.active_frac, c.tu.active_frac) << c.pixel_khz;
        EXPECT_EQ(tu.active_polarity, c.tu.active_polarity) << c.pixel_khz;
        EXPECT_EQ(tu.watermark, c.tu.watermark) << c.pixel_khz;
        EXPECT_EQ(tu.hblank_symbols, c.tu.hblank_symbols) << c.pixel_khz;
        EXPECT_EQ(tu.vblank_symbols, c.tu.vblank_symbols) << c.pixel_khz;
    }
}

TEST(TransferUnit, OverfullLinkIsRefused)
{
    switch_dp_tu tu{};
    EXPECT_FALSE(switch_dp_tu_config(148500, 1920, 2200, SWITCH_DP_RATE_RBR, 2, true, &tu));
}

TEST(PllD2, PixelClocksAreExact)
{
    const bool socs[] = {false, true};
    for (bool b01 : socs) {
        SCOPED_TRACE(b01 ? "B01" : "Erista");
        switch_dp_plld2 pll{};
        ASSERT_TRUE(switch_dp_plld2_for(594000, 38400, b01, &pll));
        EXPECT_EQ(pll.out_hz, 594000000u);
        EXPECT_EQ(pll.n, 30u);
        EXPECT_EQ(pll.m, 1u);
        EXPECT_EQ(pll.pdiv, 2u);
        EXPECT_EQ(pll.pdiv_hw, 1u);
        EXPECT_EQ(pll.sdm_din, 0xE00);
        ASSERT_TRUE(switch_dp_plld2_for(148500, 38400, b01, &pll));
        EXPECT_EQ(pll.out_hz, 148500000u);
        EXPECT_EQ(pll.pdiv, 6u);
        EXPECT_EQ(pll.pdiv_hw, 5u);
        EXPECT_EQ(pll.n, 23u);
        EXPECT_EQ(pll.sdm_din, 0xF680);
        ASSERT_TRUE(switch_dp_plld2_for(74250, 38400, b01, &pll));
        EXPECT_EQ(pll.out_hz, 74250000u);
        EXPECT_EQ(pll.pdiv, 12u);
        EXPECT_EQ(pll.pdiv_hw, 9u);
        EXPECT_EQ(pll.n, 23u);
        EXPECT_EQ(pll.sdm_din, 0xF680);
        ASSERT_TRUE(switch_dp_plld2_for(196800, 38400, b01, &pll));
        EXPECT_EQ(pll.out_hz, 196800000u);
        EXPECT_EQ(pll.n, 20u);
        EXPECT_EQ(pll.pdiv, 4u);
        EXPECT_EQ(pll.pdiv_hw, 3u);
        EXPECT_EQ(pll.sdm_din, 0);
        ASSERT_TRUE(switch_dp_plld2_for(27000, 38400, b01, &pll));
        EXPECT_EQ(pll.out_hz, 27000000u);
    }
}

TEST(PllD2, SocLimitsAndVgaDividers)
{
    switch_dp_plld2 pll{};
    ASSERT_TRUE(switch_dp_plld2_for(25175, 38400, false, &pll));
    EXPECT_EQ(pll.pdiv, 30u);
    EXPECT_EQ(pll.pdiv_hw, 15u);
    EXPECT_NEAR(static_cast<double>(pll.out_hz), 25175000.0, 25175000.0 * 1e-4);
    ASSERT_TRUE(switch_dp_plld2_for(25175, 38400, true, &pll));
    EXPECT_EQ(pll.pdiv, 31u);
    EXPECT_EQ(pll.pdiv_hw, 16u);
    EXPECT_NEAR(static_cast<double>(pll.out_hz), 25175000.0, 25175000.0 * 1e-4);
    EXPECT_TRUE(switch_dp_plld2_for(25000, 38400, false, &pll));
    EXPECT_FALSE(switch_dp_plld2_for(25000, 38400, true, &pll));
    EXPECT_TRUE(switch_dp_plld2_for(1500000, 38400, false, &pll));
    EXPECT_FALSE(switch_dp_plld2_for(1500001, 38400, false, &pll));
    EXPECT_TRUE(switch_dp_plld2_for(1620000, 38400, true, &pll));
    EXPECT_FALSE(switch_dp_plld2_for(1620001, 38400, true, &pll));
}

TEST(PllD2, VcoStaysInRange)
{
    const bool socs[] = {false, true};
    for (bool b01 : socs) {
        SCOPED_TRACE(b01 ? "B01" : "Erista");
        for (uint32_t khz = 26000; khz <= 165000; khz += 250) {
            switch_dp_plld2 pll{};
            ASSERT_TRUE(switch_dp_plld2_for(khz, 38400, b01, &pll)) << khz;
            const uint64_t vco = pll.out_hz * pll.pdiv;
            EXPECT_GE(vco, (b01 ? 780000000u : 750000000u) - 5000u) << khz;
            EXPECT_LE(vco, (b01 ? 1620000000u : 1500000000u) + 5000u) << khz;
            EXPECT_GE(static_cast<int16_t>(pll.sdm_din), -4096) << khz;
            EXPECT_LE(static_cast<int16_t>(pll.sdm_din), 4095) << khz;
            EXPECT_NEAR(static_cast<double>(pll.out_hz), khz * 1000.0, khz * 1000.0 * 1e-4) << khz;
        }
    }
}

// A sink that recovers the clock once every lane swings at least `swing` and equalises once every lane also has
// `emphasis`; it asks for exactly that.
struct Sink {
    uint8_t lanes = 2;
    uint8_t swing = 1, emphasis = 1;
    bool never_locks = false, loses_clock_in_eq = false;
    uint8_t vs[4] = {}, pe[4] = {};
    uint8_t pattern = 0;

    void write(const switch_dp_train_out &out)
    {
        unsigned offset = 0;
        if (out.dpcd_addr == SWITCH_DPCD_TRAINING_PATTERN_SET) {
            pattern = out.dpcd[0] & 3;
            offset = 1;
        }
        for (unsigned l = 0; offset + l < out.dpcd_len; ++l) {
            vs[l] = out.dpcd[offset + l] & 3;
            pe[l] = (out.dpcd[offset + l] >> 3) & 3;
        }
    }

    void status(uint8_t s[6]) const
    {
        for (int i = 0; i < 6; ++i) {
            s[i] = 0;
        }
        for (unsigned l = 0; l < lanes; ++l) {
            uint8_t bits = 0;
            const bool cr = !never_locks && vs[l] >= swing && !(loses_clock_in_eq && pattern >= 2);
            if (cr) {
                bits |= SWITCH_DPCD_CR_DONE;
            }
            if (cr && pattern >= 2 && pe[l] >= emphasis) {
                bits |= SWITCH_DPCD_EQ_DONE | SWITCH_DPCD_SYMBOL_LOCKED;
            }
            s[l / 2] = static_cast<uint8_t>(s[l / 2] | (bits << (4 * (l % 2))));
            const uint8_t request = static_cast<uint8_t>((never_locks ? 3 : swing) | (emphasis << 2));
            s[4 + l / 2] = static_cast<uint8_t>(s[4 + l / 2] | (request << (4 * (l % 2))));
        }
        bool all = true;
        for (unsigned l = 0; l < lanes; ++l) {
            all = all && ((s[l / 2] >> (4 * (l % 2))) & 0x7) == 0x7;
        }
        s[2] = all ? SWITCH_DPCD_INTERLANE_ALIGN_DONE : 0;
    }
};

switch_dp_train_out train(Sink &sink, switch_dp_train &t, bool tps3, unsigned *drives)
{
    switch_dp_train_out out = switch_dp_train_start(&t, sink.lanes, tps3, 0x12, 0);
    *drives = 0;
    while (out.act == SWITCH_DP_TRAIN_DRIVE && *drives < 50) {
        ++*drives;
        sink.write(out);
        uint8_t status[11] = {};
        sink.status(status);
        out = switch_dp_train_status(&t, status);
    }
    return out;
}

TEST(Training, ConvergesOnTheSinksRequests)
{
    Sink sink;
    switch_dp_train t{};
    unsigned drives = 0;
    const switch_dp_train_out out = train(sink, t, false, &drives);
    EXPECT_EQ(out.act, SWITCH_DP_TRAIN_DONE);
    EXPECT_EQ(out.dpcd[0], 0);
    EXPECT_EQ(out.dpcd_len, 1);
    EXPECT_EQ(t.phase, SWITCH_DP_TRAIN_TRAINED);
    EXPECT_LE(drives, 4u);
}

TEST(Training, PatternsAndLaneSettings)
{
    switch_dp_train t{};
    switch_dp_train_out out = switch_dp_train_start(&t, 2, true, 0x12, 1);
    EXPECT_EQ(out.pattern, 1);
    EXPECT_EQ(out.dpcd_addr, SWITCH_DPCD_TRAINING_PATTERN_SET);
    EXPECT_EQ(out.dpcd[0], 0x21);
    EXPECT_EQ(out.dpcd_len, 3);
    EXPECT_EQ(out.wait_us, 4000u);
    const uint8_t cr_retry[11] = {0x00, 0, 0, 0, 0x55, 0};  // both lanes ask swing 1 emphasis 1
    out = switch_dp_train_status(&t, cr_retry);
    EXPECT_EQ(out.pattern, 1);
    EXPECT_EQ(out.dpcd_addr, SWITCH_DPCD_TRAINING_LANE0_SET);
    EXPECT_EQ(out.dpcd_len, 2);
    EXPECT_EQ(out.dpcd[0], 1 | (1 << 3));
    EXPECT_EQ(out.dpcd[1], 1 | (1 << 3));
    const uint8_t cr_done[11] = {0x11, 0, 0, 0, 0x0E, 0};  // lane 0 asks swing 2 emphasis 3: clamped to 1
    out = switch_dp_train_status(&t, cr_done);
    EXPECT_EQ(out.pattern, 3);
    EXPECT_EQ(out.dpcd_addr, SWITCH_DPCD_TRAINING_PATTERN_SET);
    EXPECT_EQ(out.dpcd[0], 0x23);
    EXPECT_EQ(out.dpcd[1], 2 | (1 << 3) | SWITCH_DPCD_MAX_PRE_EMPHASIS_REACHED);
    EXPECT_EQ(out.dpcd_len, 3);
    EXPECT_EQ(out.wait_us, 4000u);
    const uint8_t eq_retry[11] = {0x11, 0, 0, 0, 0xCE, 0};
    out = switch_dp_train_status(&t, eq_retry);
    EXPECT_EQ(out.dpcd_addr, SWITCH_DPCD_TRAINING_LANE0_SET);
    EXPECT_EQ(out.dpcd_len, 2);
    EXPECT_EQ(out.dpcd[0], 2 | (1 << 3) | SWITCH_DPCD_MAX_PRE_EMPHASIS_REACHED);
    EXPECT_EQ(out.dpcd[1], (3 << 3) | SWITCH_DPCD_MAX_PRE_EMPHASIS_REACHED);
    switch_dp_train t2{};
    out = switch_dp_train_start(&t2, 1, false, 0x12, 0);
    EXPECT_EQ(out.dpcd_addr, SWITCH_DPCD_TRAINING_PATTERN_SET);
    EXPECT_EQ(out.wait_us, 100u);
    const uint8_t done[6] = {0x01, 0, 0, 0, 0, 0};
    out = switch_dp_train_status(&t2, done);
    EXPECT_EQ(out.dpcd_addr, SWITCH_DPCD_TRAINING_PATTERN_SET);
    EXPECT_EQ(out.dpcd[0], 0x22);
    EXPECT_EQ(out.wait_us, 400u);
}

TEST(Training, PerLanePostcursorAndPartialLock)
{
    switch_dp_train t{};
    (void)switch_dp_train_start(&t, 4, true, 0x12, 0);
    const uint8_t mixed[11] = {0x11, 0x01, 0x80, 0, 0x33, 0x33, 0, 0, 0, 0, 0xe4};
    EXPECT_EQ(switch_dp_train_status(&t, mixed).pattern, 1);
    uint8_t levels[2] = {};
    switch_dp_train_set2(&t, levels);
    EXPECT_EQ(levels[0], 0x10);
    EXPECT_EQ(levels[1], 0x72);
    const uint8_t allCr[11] = {0x11, 0x11};
    EXPECT_EQ(switch_dp_train_status(&t, allCr).pattern, 3);
    const uint8_t eqMiss[11] = {0x77, 0x17, 0x80, 0x02, 0x66, 0x66};
    switch_dp_train_out result{};
    for (unsigned i = 0; i < SWITCH_DP_EQ_TRIES_MAX; ++i)
        result = switch_dp_train_status(&t, eqMiss);
    EXPECT_EQ(result.act, SWITCH_DP_TRAIN_FAIL);
    (void)switch_dp_train_start(&t, 2, false, 0x12, 0);
    EXPECT_EQ(switch_dp_train_status(&t, allCr).pattern, 2);
    const uint8_t twoLocked[6] = {0x77, 0, SWITCH_DPCD_INTERLANE_ALIGN_DONE};
    EXPECT_EQ(switch_dp_train_status(&t, twoLocked).act, SWITCH_DP_TRAIN_DONE);
}

TEST(Training, SinkIntervalDependsOnDpcdRevisionAndTrainingPhase)
{
    const uint8_t retry[6] = {0, 0, 0, 0, 0x11, 0};
    const uint8_t recovered[6] = {0x11, 0, 0, 0, 0, 0};
    for (uint8_t revision : {0x12, 0x14}) {
        switch_dp_train t{};
        const unsigned crWait = revision == 0x12 ? 8000u : 100u;
        auto out = switch_dp_train_start(&t, 2, false, revision, 0x82);
        EXPECT_EQ(out.wait_us, crWait);
        out = switch_dp_train_status(&t, retry);
        ASSERT_EQ(out.act, SWITCH_DP_TRAIN_DRIVE);
        EXPECT_EQ(out.pattern, 1);
        EXPECT_EQ(out.wait_us, crWait);
        out = switch_dp_train_status(&t, recovered);
        EXPECT_EQ(out.pattern, 2);
        EXPECT_EQ(out.wait_us, 8000u);
    }
}

TEST(Training, RecoveredLaneAtMaxSwingAllowsOtherLanesToAdjust)
{
    switch_dp_train t{};
    (void)switch_dp_train_start(&t, 2, false, 0x12, 0);
    const uint8_t first[6] = {0, 0, 0, 0, 0x13, 0};
    ASSERT_EQ(switch_dp_train_status(&t, first).act, SWITCH_DP_TRAIN_DRIVE);
    const uint8_t retry[6] = {0x01, 0, 0, 0, 0x23, 0};
    const switch_dp_train_out out = switch_dp_train_status(&t, retry);
    EXPECT_EQ(out.act, SWITCH_DP_TRAIN_DRIVE);
    EXPECT_EQ(out.pattern, 1);
    EXPECT_EQ(out.dpcd_addr, SWITCH_DPCD_TRAINING_LANE0_SET);
    EXPECT_EQ(out.dpcd_len, 2);
    EXPECT_EQ(out.dpcd[0], 3 | SWITCH_DPCD_MAX_SWING_REACHED | SWITCH_DPCD_MAX_PRE_EMPHASIS_REACHED);
    EXPECT_EQ(out.dpcd[1] & 3u, 2u);
    const uint8_t recovered[6] = {0x11, 0, 0, 0, 0, 0};
    EXPECT_EQ(switch_dp_train_status(&t, recovered).pattern, 2);
}

TEST(Training, ClockThatNeverLocksFails)
{
    Sink sink;
    sink.never_locks = true;
    switch_dp_train t{};
    unsigned drives = 0;
    EXPECT_EQ(train(sink, t, false, &drives).act, SWITCH_DP_TRAIN_FAIL);
    EXPECT_NE(t.why, nullptr);
    EXPECT_LE(drives, SWITCH_DP_CR_TRIES_MAX);
}

TEST(Training, ClockLostInEqualisationFails)
{
    Sink sink;
    sink.loses_clock_in_eq = true;
    switch_dp_train t{};
    unsigned drives = 0;
    EXPECT_EQ(train(sink, t, false, &drives).act, SWITCH_DP_TRAIN_FAIL);
}

TEST(Training, EqualisationGivesUpAfterFiveTries)
{
    Sink sink;
    sink.emphasis = 3;
    sink.swing = 1;  // asks for 1 + 3: SOR1 tops out at 1 + 2, so it never equalises
    switch_dp_train t{};
    unsigned drives = 0;
    EXPECT_EQ(train(sink, t, false, &drives).act, SWITCH_DP_TRAIN_FAIL);
    EXPECT_EQ(t.tries, SWITCH_DP_EQ_TRIES_MAX);
}

TEST(Training, LinkHolds)
{
    const uint8_t good[3] = {0x77, 0x77, 0x01};
    EXPECT_TRUE(switch_dp_link_holds(good, 4));
    const uint8_t lost[3] = {0x77, 0x37, 0x01};
    EXPECT_FALSE(switch_dp_link_holds(lost, 4));
    EXPECT_TRUE(switch_dp_link_holds(lost, 2));
    const uint8_t unaligned[3] = {0x77, 0x00, 0x00};
    EXPECT_FALSE(switch_dp_link_holds(unaligned, 2));
}

}  // namespace

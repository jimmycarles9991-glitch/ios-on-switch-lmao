// SPDX-License-Identifier: GPL-2.0-only
// The sink's modes from its EDID (base block detailed timings, the 640x480 established timing every DisplayPort sink
// must take, CTA-861 extension detailed timings and short video descriptors) and the one to drive. Plain C for the
// host tests.
//
// Output is capped at 1920x1080 and 60 Hz whatever the sink offers: the Tegra X1 composites the UI for the panel and
// the TV together, and more pixels than 1080p60 cost frame rate everywhere. 1080p60 first, then modes of 50 Hz and up
// by size, then the rest; a mode must also fit the trained link.
#ifndef SWITCH_DP_MODES_H
#define SWITCH_DP_MODES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// NOLINTBEGIN(modernize-avoid-c-style-cast,modernize-loop-convert): plain C, compiled by C host tests as well
#define SWITCH_DP_MAX_WIDTH      1920u
#define SWITCH_DP_MAX_HEIGHT     1080u
#define SWITCH_DP_MAX_REFRESH    60500u  // mHz: 60 Hz and 59.94 Hz
#define SWITCH_DP_SMOOTH         49900u  // mHz: 50 Hz and up count as smooth
#define SWITCH_DP_BITS_PER_PIXEL 24u
#define SWITCH_DP_MAX_MODES      32u

struct switch_dp_mode {
    uint32_t pixel_khz;
    uint16_t hactive, hfront, hsync, hback;
    uint16_t vactive, vfront, vsync, vback;
    bool hsync_high, vsync_high;
    bool preferred;
};

static inline uint32_t switch_dp_mode_htotal(const struct switch_dp_mode *m)
{
    return (uint32_t)m->hactive + m->hfront + m->hsync + m->hback;
}

static inline uint32_t switch_dp_mode_vtotal(const struct switch_dp_mode *m)
{
    return (uint32_t)m->vactive + m->vfront + m->vsync + m->vback;
}

static inline uint32_t switch_dp_mode_refresh_mhz(const struct switch_dp_mode *m)
{
    const uint64_t total = (uint64_t)switch_dp_mode_htotal(m) * switch_dp_mode_vtotal(m);
    return total == 0 ? 0 : (uint32_t)(((uint64_t)m->pixel_khz * 1000000u + total / 2) / total);
}

static inline bool switch_dp_edid_block_valid(const uint8_t *block)
{
    uint8_t sum = 0;
    for (unsigned i = 0; i < 128; ++i) {
        sum = (uint8_t)(sum + block[i]);
    }
    return sum == 0;
}

static inline bool switch_dp_edid_header_valid(const uint8_t *edid)
{
    static const uint8_t header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    for (unsigned i = 0; i < 8; ++i) {
        if (edid[i] != header[i]) {
            return false;
        }
    }
    return switch_dp_edid_block_valid(edid);
}

// An 18-byte detailed timing descriptor; false for display descriptors (pixel clock 0) and interlaced timings.
static inline bool switch_dp_edid_dtd(const uint8_t *d, struct switch_dp_mode *mode)
{
    const uint32_t clock = d[0] | ((uint32_t)d[1] << 8);
    if (clock == 0 || (d[17] & 0x80u) != 0) {
        return false;
    }
    const uint16_t hactive = (uint16_t)(d[2] | ((d[4] & 0xF0u) << 4));
    const uint16_t hblank = (uint16_t)(d[3] | ((d[4] & 0x0Fu) << 8));
    const uint16_t vactive = (uint16_t)(d[5] | ((d[7] & 0xF0u) << 4));
    const uint16_t vblank = (uint16_t)(d[6] | ((d[7] & 0x0Fu) << 8));
    const uint16_t hfront = (uint16_t)(d[8] | ((d[11] & 0xC0u) << 2));
    const uint16_t hsync = (uint16_t)(d[9] | ((d[11] & 0x30u) << 4));
    const uint16_t vfront = (uint16_t)(((uint32_t)d[10] >> 4) | ((d[11] & 0x0Cu) << 2));
    const uint16_t vsync = (uint16_t)((d[10] & 0x0Fu) | ((d[11] & 0x03u) << 4));
    if (hfront + hsync >= hblank || vfront + vsync >= vblank || hactive == 0 || vactive == 0) {
        return false;
    }
    const bool digital_separate = (d[17] & 0x18u) == 0x18u;
    const struct switch_dp_mode parsed = {
        clock * 10u,
        hactive,
        hfront,
        hsync,
        (uint16_t)(hblank - hfront - hsync),
        vactive,
        vfront,
        vsync,
        (uint16_t)(vblank - vfront - vsync),
        digital_separate && (d[17] & 0x02u) != 0,
        digital_separate && (d[17] & 0x04u) != 0,
        false,
    };
    *mode = parsed;
    return true;
}

// CTA-861 VICs a 1080p60 cap leaves (progressive, 1920x1080 or smaller), from the standard's timing tables.
static inline bool switch_dp_cta_vic(unsigned vic, struct switch_dp_mode *mode)
{
    static const struct {
        uint8_t vic;
        struct switch_dp_mode mode;
    } table[] = {
        {1, {25175, 640, 16, 96, 48, 480, 10, 2, 33, false, false, false}},
        {2, {27000, 720, 16, 62, 60, 480, 9, 6, 30, false, false, false}},
        {3, {27000, 720, 16, 62, 60, 480, 9, 6, 30, false, false, false}},
        {4, {74250, 1280, 110, 40, 220, 720, 5, 5, 20, true, true, false}},
        {16, {148500, 1920, 88, 44, 148, 1080, 4, 5, 36, true, true, false}},
        {17, {27000, 720, 12, 64, 68, 576, 5, 5, 39, false, false, false}},
        {18, {27000, 720, 12, 64, 68, 576, 5, 5, 39, false, false, false}},
        {19, {74250, 1280, 440, 40, 220, 720, 5, 5, 20, true, true, false}},
        {31, {148500, 1920, 528, 44, 148, 1080, 4, 5, 36, true, true, false}},
        {32, {74250, 1920, 638, 44, 148, 1080, 4, 5, 36, true, true, false}},
        {33, {74250, 1920, 528, 44, 148, 1080, 4, 5, 36, true, true, false}},
        {34, {74250, 1920, 88, 44, 148, 1080, 4, 5, 36, true, true, false}},
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (table[i].vic == vic) {
            *mode = table[i].mode;
            return true;
        }
    }
    return false;
}

struct switch_dp_mode_list {
    struct switch_dp_mode modes[SWITCH_DP_MAX_MODES];
    unsigned count;
};

static inline void switch_dp_modes_add(struct switch_dp_mode_list *list, const struct switch_dp_mode *mode)
{
    if (list->count < SWITCH_DP_MAX_MODES) {
        list->modes[list->count++] = *mode;
    }
}

static inline void switch_dp_edid_cta(const uint8_t *block, struct switch_dp_mode_list *list)
{
    const unsigned dtd_start = block[2];
    if (block[0] != 0x02 || dtd_start < 4 || dtd_start > 127) {
        return;
    }
    for (unsigned at = 4; at < dtd_start;) {
        const unsigned tag = block[at] >> 5;
        const unsigned length = block[at] & 0x1Fu;
        if (at + 1 + length > dtd_start) {
            break;
        }
        for (unsigned i = 0; tag == 2 && i < length; ++i) {
            const uint8_t svd = block[at + 1 + i];
            const unsigned vic = (svd >= 129 && svd <= 192) ? svd & 0x7Fu : svd;
            struct switch_dp_mode mode;
            if (switch_dp_cta_vic(vic, &mode)) {
                switch_dp_modes_add(list, &mode);
            }
        }
        at += 1 + length;
    }
    for (unsigned at = dtd_start; at + 18 <= 127; at += 18) {
        struct switch_dp_mode mode;
        if (switch_dp_edid_dtd(block + at, &mode)) {
            switch_dp_modes_add(list, &mode);
        }
    }
}

// Every mode of `edid` (`blocks` 128-byte blocks read). A bad base block yields only 640x480, which DisplayPort
// requires every sink to take; a bad extension is skipped.
static inline void switch_dp_edid_modes(const uint8_t *edid, unsigned blocks, struct switch_dp_mode_list *list)
{
    list->count = 0;
    struct switch_dp_mode vga;
    (void)switch_dp_cta_vic(1, &vga);
    if (blocks == 0 || !switch_dp_edid_header_valid(edid)) {
        switch_dp_modes_add(list, &vga);
        return;
    }
    for (unsigned at = 54; at + 18 <= 126; at += 18) {
        struct switch_dp_mode mode;
        if (switch_dp_edid_dtd(edid + at, &mode)) {
            mode.preferred = at == 54 && list->count == 0;
            switch_dp_modes_add(list, &mode);
        }
    }
    if ((edid[35] & 0x20u) != 0 || list->count == 0) {
        switch_dp_modes_add(list, &vga);
    }
    const unsigned extensions = edid[126] < blocks - 1 ? edid[126] : blocks - 1;
    for (unsigned b = 1; b <= extensions; ++b) {
        const uint8_t *block = edid + (size_t)128 * b;
        if (switch_dp_edid_block_valid(block)) {
            switch_dp_edid_cta(block, list);
        }
    }
}

static inline bool switch_dp_mode_allowed(const struct switch_dp_mode *m, uint64_t link_payload_kbps)
{
    const uint32_t refresh = switch_dp_mode_refresh_mhz(m);
    return m->hactive <= SWITCH_DP_MAX_WIDTH && m->vactive <= SWITCH_DP_MAX_HEIGHT && refresh != 0 &&
           refresh <= SWITCH_DP_MAX_REFRESH && (uint64_t)m->pixel_khz * SWITCH_DP_BITS_PER_PIXEL < link_payload_kbps;
}

static inline bool switch_dp_mode_is_1080p60(const struct switch_dp_mode *m)
{
    return m->hactive == 1920 && m->vactive == 1080 && switch_dp_mode_refresh_mhz(m) >= 59900;
}

// True when `a` is the better choice: 1080p60, then smooth (50 Hz and up) before slow, then more pixels, then a higher
// refresh, then the sink's own preference.
static inline bool switch_dp_mode_better(const struct switch_dp_mode *a, const struct switch_dp_mode *b)
{
    if (switch_dp_mode_is_1080p60(a) != switch_dp_mode_is_1080p60(b)) {
        return switch_dp_mode_is_1080p60(a);
    }
    const uint32_t ra = switch_dp_mode_refresh_mhz(a);
    const uint32_t rb = switch_dp_mode_refresh_mhz(b);
    if ((ra >= SWITCH_DP_SMOOTH) != (rb >= SWITCH_DP_SMOOTH)) {
        return ra >= SWITCH_DP_SMOOTH;
    }
    const uint32_t area_a = (uint32_t)a->hactive * a->vactive;
    const uint32_t area_b = (uint32_t)b->hactive * b->vactive;
    if (area_a != area_b) {
        return area_a > area_b;
    }
    if (ra / 100 != rb / 100) {
        return ra > rb;
    }
    return a->preferred && !b->preferred;
}

// The index of the mode to drive over a link carrying `link_payload_kbps`, or -1 when none fits.
static inline int switch_dp_pick_mode(const struct switch_dp_mode_list *list, uint64_t link_payload_kbps)
{
    int best = -1;
    for (unsigned i = 0; i < list->count; ++i) {
        const struct switch_dp_mode *m = &list->modes[i];
        if (switch_dp_mode_allowed(m, link_payload_kbps) &&
            (best < 0 || switch_dp_mode_better(m, &list->modes[best]))) {
            best = (int)i;
        }
    }
    return best;
}

// NOLINTEND(modernize-avoid-c-style-cast,modernize-loop-convert)

#endif

// SPDX-License-Identifier: GPL-2.0-only
// EDID base and CTA detailed timings, established VGA, and CTA short video descriptors.
#ifndef SWITCH_DP_MODES_H
#define SWITCH_DP_MODES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// NOLINTBEGIN(modernize-avoid-c-style-cast,modernize-loop-convert): plain C, compiled by C host tests as well
#define SWITCH_DP_MAX_WIDTH      4096u
#define SWITCH_DP_MAX_HEIGHT     4096u
#define SWITCH_DP_MAX_PIXEL_KHZ  750000u
#define SWITCH_DP_BITS_PER_PIXEL 24u
#define SWITCH_DP_MAX_MODES      64u

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

static inline uint64_t switch_dp_refresh_period(const struct switch_dp_mode *m)
{
    const uint64_t pixels = (uint64_t)switch_dp_mode_htotal(m) * switch_dp_mode_vtotal(m);
    return m->pixel_khz ? (pixels << 32) / ((uint64_t)m->pixel_khz * 1000u) : 0;
}

static inline uint32_t switch_dp_mode_refresh_mhz(const struct switch_dp_mode *m)
{
    const uint64_t total = (uint64_t)switch_dp_mode_htotal(m) * switch_dp_mode_vtotal(m);
    return total == 0 ? 0 : (uint32_t)(((uint64_t)m->pixel_khz * 1000000u + total / 2) / total);
}

static inline bool switch_dp_source_timing(const struct switch_dp_mode *m)
{
    const uint64_t total = (uint64_t)switch_dp_mode_htotal(m) * switch_dp_mode_vtotal(m);
    return m->hactive <= SWITCH_DP_MAX_WIDTH && m->vactive <= SWITCH_DP_MAX_HEIGHT &&
           m->pixel_khz <= SWITCH_DP_MAX_PIXEL_KHZ && (uint64_t)m->pixel_khz * 10000u <= total * 605u;
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

// Progressive CTA-861 timings. Interlaced and pixel-repeated modes need separate scanout support.
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
        {41, {148500, 1280, 440, 40, 220, 720, 5, 5, 20, true, true, false}},
        {47, {148500, 1280, 110, 40, 220, 720, 5, 5, 20, true, true, false}},
        {63, {297000, 1920, 88, 44, 148, 1080, 4, 5, 36, true, true, false}},
        {64, {297000, 1920, 528, 44, 148, 1080, 4, 5, 36, true, true, false}},
        {93, {297000, 3840, 1276, 88, 296, 2160, 8, 10, 72, true, true, false}},
        {94, {297000, 3840, 1056, 88, 296, 2160, 8, 10, 72, true, true, false}},
        {95, {297000, 3840, 176, 88, 296, 2160, 8, 10, 72, true, true, false}},
        {96, {594000, 3840, 1056, 88, 296, 2160, 8, 10, 72, true, true, false}},
        {97, {594000, 3840, 176, 88, 296, 2160, 8, 10, 72, true, true, false}},
        {98, {297000, 4096, 1020, 88, 296, 2160, 8, 10, 72, true, true, false}},
        {99, {297000, 4096, 968, 88, 128, 2160, 8, 10, 72, true, true, false}},
        {100, {297000, 4096, 88, 88, 128, 2160, 8, 10, 72, true, true, false}},
        {101, {594000, 4096, 968, 88, 128, 2160, 8, 10, 72, true, true, false}},
        {102, {594000, 4096, 88, 88, 128, 2160, 8, 10, 72, true, true, false}},
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

static inline bool switch_dp_prefer(const struct switch_dp_mode *m, const struct switch_dp_mode *best)
{
    if (m->preferred != best->preferred)
        return m->preferred;
    const unsigned pixels = (unsigned)m->hactive * m->vactive;
    const unsigned previous = (unsigned)best->hactive * best->vactive;
    return pixels != previous ? pixels > previous : switch_dp_mode_refresh_mhz(m) > switch_dp_mode_refresh_mhz(best);
}

static inline void switch_dp_modes_add(struct switch_dp_mode_list *list, const struct switch_dp_mode *mode)
{
    if (!switch_dp_source_timing(mode))
        return;
    for (unsigned i = 0; i < list->count; ++i) {
        struct switch_dp_mode *existing = &list->modes[i];
        if (__builtin_memcmp(existing, mode, offsetof(struct switch_dp_mode, preferred)) == 0) {
            existing->preferred |= mode->preferred;
            return;
        }
    }
    if (list->count < SWITCH_DP_MAX_MODES)
        list->modes[list->count++] = *mode;
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
                mode.preferred = svd >= 129 && svd <= 192;
                switch_dp_modes_add(list, &mode);
                if (vic == 4 || vic == 16 || vic == 95 || vic == 97 || vic == 100 || vic == 102) {
                    mode.pixel_khz = (mode.pixel_khz * 1000u + 500u) / 1001u;
                    mode.preferred = false;
                    switch_dp_modes_add(list, &mode);
                }
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

static inline bool switch_dp_standard_timing(uint8_t b0, uint8_t b1, bool edid13, struct switch_dp_mode *mode)
{
    if ((b0 == 0x01u && b1 == 0x01u) || (b0 == 0x00u && b1 == 0x00u) || (b0 == 0x20u && b1 == 0x20u))
        return false;
    if ((b1 & 0x3Fu) != 0)
        return false;
    const uint16_t hactive = (uint16_t)(((unsigned)b0 + 31u) * 8u);
    uint16_t vactive = 0;
    switch (b1 >> 6) {
    case 0:
        vactive = edid13 ? (uint16_t)(hactive * 10u / 16u) : hactive;
        break;
    case 1:
        vactive = (uint16_t)(hactive * 3u / 4u);
        break;
    case 2:
        vactive = (uint16_t)(hactive * 4u / 5u);
        break;
    case 3:
        vactive = (uint16_t)(hactive * 9u / 16u);
        break;
    }
    static const struct switch_dp_mode table[] = {
        {148500, 1920, 88, 44, 148, 1080, 4, 5, 36, true, true, false},
        {97750, 1600, 48, 32, 80, 900, 3, 5, 18, true, false, false},
        {74250, 1280, 110, 40, 220, 720, 5, 5, 20, true, true, false},
        {91000, 1280, 48, 32, 80, 1024, 3, 7, 20, true, false, false},
        {65000, 1024, 24, 136, 160, 768, 3, 6, 29, false, false, false},
        {35500, 800, 48, 32, 80, 600, 3, 4, 11, true, false, false},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (table[i].hactive == hactive && table[i].vactive == vactive) {
            *mode = table[i];
            return true;
        }
    }
    return false;
}

static inline void switch_dp_edid_displayid(const uint8_t *block, struct switch_dp_mode_list *list)
{
    const unsigned section_len = block[2];
    if (block[0] != 0x70u || section_len > 121u)
        return;
    uint8_t sum = 0;
    for (unsigned i = 1; i <= 5u + section_len; ++i)
        sum = (uint8_t)(sum + block[i]);
    if (sum != 0)
        return;
    const unsigned section_end = 5u + section_len;
    for (unsigned at = 5u; at + 3u <= section_end;) {
        const uint8_t tag = block[at];
        const unsigned len = block[at + 2u];
        if (at + 3u + len > section_end)
            break;
        if (tag == 0x03u || tag == 0x22u) {
            for (unsigned d = at + 3u; d + 20u <= at + 3u + len; d += 20u) {
                const uint8_t *t = block + d;
                if ((t[3] & 0x10u) != 0)
                    continue;
                const uint32_t raw_clk =
                    ((uint32_t)t[0] | ((uint32_t)t[1] << 8) | ((uint32_t)t[2] << 16)) + 1u;
                if (tag == 0x03u && raw_clk > SWITCH_DP_MAX_PIXEL_KHZ / 10u)
                    continue;
                const uint32_t pixel_khz = tag == 0x03u ? raw_clk * 10u : raw_clk;
                const uint32_t hactive = ((uint32_t)t[4] | ((uint32_t)t[5] << 8)) + 1u;
                const uint32_t hblank = ((uint32_t)t[6] | ((uint32_t)t[7] << 8)) + 1u;
                const uint32_t hfront = ((uint32_t)t[8] | (((uint32_t)t[9] & 0x7Fu) << 8)) + 1u;
                const uint32_t hsync = ((uint32_t)t[10] | ((uint32_t)t[11] << 8)) + 1u;
                const uint32_t vactive = ((uint32_t)t[12] | ((uint32_t)t[13] << 8)) + 1u;
                const uint32_t vblank = ((uint32_t)t[14] | ((uint32_t)t[15] << 8)) + 1u;
                const uint32_t vfront = ((uint32_t)t[16] | (((uint32_t)t[17] & 0x7Fu) << 8)) + 1u;
                const uint32_t vsync = ((uint32_t)t[18] | ((uint32_t)t[19] << 8)) + 1u;
                if (hactive > SWITCH_DP_MAX_WIDTH || vactive > SWITCH_DP_MAX_HEIGHT || hblank > 0x7FFFu ||
                    vblank > 0x7FFFu || hfront + hsync >= hblank || vfront + vsync >= vblank)
                    continue;
                const struct switch_dp_mode mode = {
                    pixel_khz,
                    (uint16_t)hactive,
                    (uint16_t)hfront,
                    (uint16_t)hsync,
                    (uint16_t)(hblank - hfront - hsync),
                    (uint16_t)vactive,
                    (uint16_t)vfront,
                    (uint16_t)vsync,
                    (uint16_t)(vblank - vfront - vsync),
                    (t[9] & 0x80u) != 0,
                    (t[17] & 0x80u) != 0,
                    (t[3] & 0x80u) != 0,
                };
                switch_dp_modes_add(list, &mode);
            }
        }
        at += 3u + len;
    }
}

static inline bool switch_dp_edid_block_modes(const uint8_t *edid, bool base, struct switch_dp_mode_list *list)
{
    if (!base) {
        if (!switch_dp_edid_block_valid(edid))
            return false;
        if (edid[0] == 0x02u)
            switch_dp_edid_cta(edid, list);
        else if (edid[0] == 0x70u)
            switch_dp_edid_displayid(edid, list);
        return true;
    }
    list->count = 0;
    if (!switch_dp_edid_header_valid(edid))
        return false;
    for (unsigned at = 54; at + 18 <= 126; at += 18) {
        struct switch_dp_mode mode;
        if (switch_dp_edid_dtd(edid + at, &mode)) {
            mode.preferred = at == 54 && list->count == 0;
            switch_dp_modes_add(list, &mode);
        }
    }
    const bool edid13 = edid[18] > 1 || (edid[18] == 1 && edid[19] >= 3);
    for (unsigned at = 38; at + 2 <= 54; at += 2) {
        struct switch_dp_mode mode;
        if (switch_dp_standard_timing(edid[at], edid[at + 1], edid13, &mode))
            switch_dp_modes_add(list, &mode);
    }
    if ((edid[35] & 0x20u) != 0) {
        struct switch_dp_mode vga;
        (void)switch_dp_cta_vic(1, &vga);
        switch_dp_modes_add(list, &vga);
    }
    return true;
}

static inline bool switch_dp_mode_allowed(const struct switch_dp_mode *m, uint64_t link_payload_kbps)
{
    return switch_dp_source_timing(m) && m->hactive >= 16 &&
           m->vactive >= 16 && m->hsync != 0 && m->vsync != 0 &&
           m->hfront >= 2 && m->vfront >= 2 && m->hsync + m->hback >= 20 &&
           switch_dp_mode_htotal(m) <= 0x7fffu && switch_dp_mode_vtotal(m) <= 0x7fffu &&
           m->pixel_khz >= 11719 && (uint64_t)m->pixel_khz * SWITCH_DP_BITS_PER_PIXEL < link_payload_kbps;
}

// NOLINTEND(modernize-avoid-c-style-cast,modernize-loop-convert)

#endif

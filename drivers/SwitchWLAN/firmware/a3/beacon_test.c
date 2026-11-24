/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "beacon.h"
#include <stddef.h>

static union {
    uint64_t alignment;
    uint8_t bytes[0x620];
} switch_wlc, switch_cfg, switch_info, switch_band;

static uint8_t switch_frame[96], switch_status[32];
static void *switch_primary;
volatile uint32_t switch_beacon_test_results[2], switch_beacon_test_done;

#define SWITCH_CHECK(expression)                             \
    do {                                                     \
        if (!(expression) && !switch_beacon_test_results[0]) \
            switch_beacon_test_results[0] = __LINE__;        \
        ++switch_beacon_test_results[1];                     \
    } while (0)

static void switch_zero(void *memory, size_t length)
{
    for (size_t index = 0; index < length; ++index)
        ((uint8_t *)memory)[index] = 0;
}

static void switch_write16(void *memory, uint16_t value)
{
    ((uint8_t *)memory)[0] = (uint8_t)value;
    ((uint8_t *)memory)[1] = (uint8_t)(value >> 8);
}

static void switch_timestamp(uint64_t timestamp)
{
    for (unsigned index = 0; index < 8; ++index)
        switch_frame[24 + index] = (uint8_t)(timestamp >> (8 * index));
}

void *switch_a3_station_bsscfg(void *wlc)
{
    return switch_primary && *(void **)switch_cfg.bytes == wlc && !switch_cfg.bytes[6] ? switch_primary : NULL;
}

uint16_t switch_a3_channel_control(uint32_t chanspec)
{
    return chanspec == 0xe02a ? 0xd024 : (uint16_t)chanspec;
}

static void switch_setup(void)
{
    switch_a3_beacon_reset(NULL);
    switch_zero(&switch_wlc, sizeof switch_wlc);
    switch_zero(&switch_cfg, sizeof switch_cfg);
    switch_zero(&switch_info, sizeof switch_info);
    switch_zero(&switch_band, sizeof switch_band);
    switch_zero(switch_frame, sizeof switch_frame);
    switch_zero(switch_status, sizeof switch_status);
    switch_primary = switch_cfg.bytes;
    switch_wlc.bytes[25] = 1;
    *(void **)(switch_wlc.bytes + 32) = switch_band.bytes;
    *(void **)switch_cfg.bytes = switch_wlc.bytes;
    *(void **)(switch_cfg.bytes + 0x110) = switch_info.bytes;
    switch_cfg.bytes[8] = 1;
    switch_cfg.bytes[16] = 1;
    switch_write16(switch_wlc.bytes + 0x466, 0x100b);
    switch_write16(switch_status + 22, 0x100b);
    switch_write16(switch_info.bytes + 0x2e, 100);
    switch_info.bytes[0x60] = 3;
    switch_frame[0] = 0x80;
    switch_write16(switch_frame + 32, 100);
    for (unsigned index = 0; index < 6; ++index) {
        const uint8_t byte = (uint8_t)(index * 10 + 10);
        switch_cfg.bytes[216 + index] = byte;
        switch_frame[10 + index] = byte;
        switch_frame[16 + index] = byte;
        switch_frame[4 + index] = 255;
    }
    switch_timestamp(103000);
    switch_frame[36] = 5;
    switch_frame[37] = 4;
    switch_frame[38] = 1;
    switch_frame[39] = 3;
}

static int switch_observe(uint64_t timestamp)
{
    return switch_a3_beacon_observe(switch_wlc.bytes, switch_status, sizeof switch_status, switch_frame, 46, timestamp);
}

static int switch_bound(uint16_t home, uint64_t now, uint64_t *bound)
{
    void *bsscfg = (void *)1;
    const int result = switch_a3_beacon_bound(switch_wlc.bytes, home, now, &bsscfg, bound);
    SWITCH_CHECK(result ? bsscfg == NULL && *bound == 0 : bsscfg == switch_cfg.bytes);
    return result;
}

static void switch_malformed(void)
{
    for (unsigned variant = 0; variant < 22; ++variant) {
        switch_setup();
        switch (variant) {
        case 0:
            switch_frame[0] = 0xd0;
            break;
        case 1:
            switch_frame[1] = 0x40;
            break;
        case 2:
            switch_frame[1] = 1;
            break;
        case 3:
            switch_frame[22] = 1;
            break;
        case 4:
            switch_frame[10] ^= 1;
            break;
        case 5:
            switch_frame[16] ^= 1;
            break;
        case 6:
            switch_status[16] = 1;
            break;
        case 7:
            switch_write16(switch_status + 4, 0x10);
            break;
        case 8:
            switch_write16(switch_status + 22, 0x1006);
            break;
        case 9:
            switch_write16(switch_frame + 32, 0);
            break;
        case 10:
            switch_write16(switch_frame + 32, 200);
            break;
        case 11:
            switch_frame[36] = 0;
            break;
        case 12:
            switch_frame[37] = 50;
            break;
        case 13:
            switch_frame[37] = 3;
            break;
        case 14:
            switch_frame[39] = 0;
            break;
        case 15:
            switch_frame[38] = 3;
            break;
        case 16:
            switch_frame[39] = 2;
            break;
        case 17:
            switch_frame[40] = 254;
            break;
        case 18:
            switch_cfg.bytes[8] = 0;
            break;
        case 19:
            switch_cfg.bytes[6] = 1;
            break;
        case 20:
            switch_primary = NULL;
            break;
        case 21:
            switch_wlc.bytes[25] = 0;
            break;
        }
        SWITCH_CHECK(!switch_observe(1000000));
        uint64_t bound;
        SWITCH_CHECK(switch_bound(0x100b, 1000000, &bound) != 0);
    }
}

void switch_beacon_test_run(void)
{
    uint64_t bound;
    switch_setup();
    SWITCH_CHECK(switch_bound(0x100b, 1000000, &bound) == -25);
    SWITCH_CHECK(switch_observe(1000000) == 1);
#if DEVELOPMENT || DEBUG
    const struct switch_a3_beacon_anchor *anchor = switch_a3_beacon_anchor(switch_wlc.bytes);
    SWITCH_CHECK(anchor && anchor->radio_tsf == 1000000 && anchor->ap_tsf == 103000 && anchor->chanspec == 0x100b);
    for (unsigned i = 0; i != 6; ++i)
        SWITCH_CHECK(anchor && anchor->bssid[i] == switch_frame[16 + i]);
#endif
    SWITCH_CHECK(switch_bound(0x100b, 1000000, &bound) == 0 && bound == 1101800);
    SWITCH_CHECK(switch_bound(0x100b, 1101799, &bound) == 0 && bound == 1101800);
    SWITCH_CHECK(switch_bound(0x100b, 1101800, &bound) == -16);
    SWITCH_CHECK(switch_bound(0x100b, 999999, &bound) == -16);
    SWITCH_CHECK(switch_bound(0x1006, 1000000, &bound) == -25);
    switch_cfg.bytes[216] ^= 1;
#if DEVELOPMENT || DEBUG
    SWITCH_CHECK(!switch_a3_beacon_anchor(switch_wlc.bytes));
#endif
    SWITCH_CHECK(switch_bound(0x100b, 1000000, &bound) == -25);
    switch_cfg.bytes[216] ^= 1;
    switch_info.bytes[0x60] = 2;
    SWITCH_CHECK(switch_bound(0x100b, 1000000, &bound) == -16);
    switch_info.bytes[0x60] = 3;
    switch_write16(switch_info.bytes + 0x2e, 200);
    SWITCH_CHECK(switch_bound(0x100b, 1000000, &bound) == -16);
    switch_write16(switch_info.bytes + 0x2e, 100);
    switch_a3_beacon_reset(&bound);
    SWITCH_CHECK(switch_bound(0x100b, 1000000, &bound) == 0);
    switch_a3_beacon_reset(switch_wlc.bytes);
#if DEVELOPMENT || DEBUG
    SWITCH_CHECK(!switch_a3_beacon_anchor(switch_wlc.bytes));
#endif
    SWITCH_CHECK(switch_bound(0x100b, 1000000, &bound) == -25);

    switch_setup();
    switch_frame[38] = 0;
    switch_frame[40] = 1;
    switch_observe(1000000);
    SWITCH_CHECK(switch_bound(0x100b, 1000000, &bound) == -16);
    switch_setup();
    switch_frame[38] = 0;
    switch_observe(1000000);
    SWITCH_CHECK(switch_bound(0x100b, 1000000, &bound) == 0);
    switch_setup();
    switch_frame[40] = 1;
    switch_observe(1000000);
    SWITCH_CHECK(switch_bound(0x100b, 1000000, &bound) == 0);

    switch_setup();
    switch_observe(1000000);
    SWITCH_CHECK(switch_observe(1000500) == 0);
    SWITCH_CHECK(switch_bound(0x100b, 1001000, &bound) == 0 && bound == 1101800);
    switch_timestamp(102999);
    switch_observe(1001000);
    SWITCH_CHECK(switch_bound(0x100b, 1001000, &bound) == 0 && bound == 1101800);
    switch_observe(500000);
    SWITCH_CHECK(switch_bound(0x100b, 500000, &bound) == 0 && bound == 601801);

    switch_setup();
    switch_timestamp(0x100000010ull);
    switch_observe(0x100000010ull);
    SWITCH_CHECK(switch_bound(0x100b, 0x100000010ull, &bound) == 0 && bound == 0x100000010ull + 98288);
    switch_setup();
    switch_observe(UINT64_MAX - 20);
    SWITCH_CHECK(switch_bound(0x100b, UINT64_MAX - 20, &bound) != 0);

    switch_setup();
    switch_write16(switch_wlc.bytes + 0x466, 0xe02a);
    switch_write16(switch_status + 22, 0xd024);
    switch_observe(1000000);
    SWITCH_CHECK(switch_bound(0xe02a, 1000000, &bound) == 0);
    SWITCH_CHECK(switch_bound(0xd024, 1000000, &bound) == 0);
    switch_setup();
    switch_write16(switch_band.bytes + 8, 8);
    switch_write16(switch_status + 4, 0x100);
    switch_observe(1000000);
    SWITCH_CHECK(switch_bound(0x100b, 1000000, &bound) == 0);

    switch_malformed();
    switch_beacon_test_done = 0x53574133;
}

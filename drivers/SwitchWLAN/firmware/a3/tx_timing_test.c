/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "tx_timing.h"

volatile uint32_t switch_timing_test_results[27];
static uint8_t switch_frame[112], switch_before[112];

#define SWITCH_REQUIRE(condition) \
    do {                          \
        if (!(condition))         \
            return __LINE__;      \
    } while (0)

static void switch_store32(uint8_t *bytes, uint32_t value)
{
    for (unsigned index = 0; index < 4; ++index)
        bytes[index] = value >> (index * 8);
}

struct switch_timing_case {
    unsigned build_tu, submit_us, expiry_us;
    unsigned stamp_tu, counter, remaining, aw_low;
    int result;
};

static const struct switch_timing_case switch_delays[] = {
    {4, 42000, 70000, 41, 23, 0, 0xfe, 1},
    {4, 16384, 70000, 16, 48, 0, 0xfd, 1},
    {63, 65300, 70000, 63, 1, 0, 0xff, 1},
    {63, 65536, 70000, 0, 0, 0, 0, -1},
    {4, 8192, 9000, 8, 56, 8, 0xfc, 1},
};

static uint32_t switch_test_timing(unsigned variant)
{
    static const uint8_t prefix[6] = {0x7f, 0, 0x17, 0xf2, 8, 0x10};
    static const uint8_t bssid[6] = {0, 0x25, 0, 0xff, 0x94, 0x73};
    for (unsigned index = 0; index < sizeof switch_frame; ++index)
        switch_frame[index] = 0;
    switch_frame[0] = variant == 0 ? 8 : 0xd0;
    for (unsigned index = 0; index < 6; ++index) {
        switch_frame[16 + index] = bssid[index];
        switch_frame[24 + index] = prefix[index];
    }
    switch_frame[30] = variant == 2 || variant == 23 ? 3 : 0;
    switch_store32(switch_frame + 32, 1000);
    switch_store32(switch_frame + 36, 512);
    switch_frame[40] = 4;
    switch_frame[41] = 33;
    switch_frame[44] = 64;
    switch_frame[58] = 16;
    uint32_t bytes = 76, lifetime = 0;
    uint64_t now = 1100, expiry = 4000;
    int wanted = 1;
    if (variant == 0)
        wanted = 0;
    if (variant == 3) {
        switch_frame[27] = 0;
        wanted = 0;
    }
    if (variant == 4) {
        switch_frame[21] = 0;
        wanted = 0;
    }
    if (variant == 5) {
        switch_frame[30] = 4;
        wanted = 0;
    }
    if (variant == 6) {
        expiry = 1101;
        wanted = -2;
    }
    if (variant == 7) {
        expiry = 1100 + 0x800100u;
        wanted = -1;
    }
    if (variant == 8) {
        now = UINT64_C(0x100000110);
        expiry = UINT64_C(0x100001000);
        switch_store32(switch_frame + 36, 0xffffff00u);
    }
    if (variant == 9) {
        switch_store32(switch_frame + 36, (uint32_t)(now - 65537u));
        wanted = -1;
    }
    if (variant == 10) {
        bytes = 42;
        wanted = -1;
    }
    if (variant == 11) {
        switch_frame[41] = 34;
        wanted = -1;
    }
    if (variant == 12) {
        switch_frame[40] = 9;
        wanted = -1;
    }
    if (variant == 13) {
        for (unsigned index = 0; index < 36; ++index)
            switch_frame[76 + index] = switch_frame[40 + index];
        bytes = 112;
        wanted = -1;
    }
    if (variant == 14) {
        switch_frame[41] = 32;
        bytes = 75;
        wanted = -1;
    }
    if (variant == 15) {
        switch_frame[44] = 0;
        wanted = -1;
    }
    if (variant == 16) {
        switch_frame[44] = 65;
        wanted = -1;
    }
    if (variant == 17) {
        switch_frame[31] = 1;
        wanted = -1;
    }
    if (variant == 18) {
        bytes = 32;
        wanted = -1;
    }
    if (variant == 19) {
        bytes = 77;
        wanted = -1;
    }
    if (variant == 20) {
        switch_frame[44] = 1;
        now = 1700;
        wanted = -1;
    }
    if (variant == 21) {
        switch_frame[44] = 1;
        switch_frame[58] = 0;
        switch_frame[72] = 3;
    }
    const uint64_t eaw = UINT64_C(0xffffff80);
    const struct switch_timing_case *delay = variant >= 22 ? &switch_delays[variant - 22] : 0;
    uint32_t expected_target = variant == 8 ? 0xffffff00u : 512u;
    uint32_t expected_lifetime = variant == 21 ? 1536u : (uint32_t)expiry & ~255u;
    if (delay) {
        const unsigned build_tu = delay->build_tu;
        now = eaw + delay->submit_us;
        expiry = eaw + delay->expiry_us;
        switch_store32(switch_frame + 32, (uint32_t)(eaw + build_tu * 1024 + 19));
        switch_store32(switch_frame + 36, (uint32_t)(eaw + build_tu * 1024));
        switch_frame[44] = (uint8_t)(64 - build_tu);
        switch_frame[58] = build_tu == 4 ? 12 : 0;
        switch_frame[72] = build_tu == 4 ? 0xfc : 0xff;
        switch_frame[73] = 0xff;
        expected_target = (uint32_t)(eaw + delay->stamp_tu * 1024);
        expected_lifetime = (uint32_t)(expiry < eaw + 65536 ? expiry : eaw + 65536) & ~255u;
        wanted = delay->result;
    }
    for (unsigned index = 0; index < sizeof switch_frame; ++index)
        switch_before[index] = switch_frame[index];
    SWITCH_REQUIRE(switch_a3_tx_timing(switch_frame, bytes, now, expiry, &lifetime) == wanted);
    if (wanted >= 0)
        SWITCH_REQUIRE(lifetime == expected_lifetime);
    if (wanted == 1) {
        switch_store32(switch_before + 32, (uint32_t)now);
        switch_store32(switch_before + 36, expected_target);
        if (delay) {
            switch_before[44] = (uint8_t)delay->counter;
            switch_before[58] = (uint8_t)delay->remaining;
            switch_before[72] = (uint8_t)delay->aw_low;
            const uint32_t owl_boundary = (uint32_t)now + (uint32_t)switch_frame[44] * 1024u;
            const uint32_t boundary = (uint32_t)(eaw + 65536u);
            SWITCH_REQUIRE(owl_boundary - boundary < 1024u);
            SWITCH_REQUIRE(owl_boundary - ((uint32_t)now - expected_target) == boundary);
        }
    }
    for (unsigned index = 0; index < sizeof switch_frame; ++index) {
        SWITCH_REQUIRE(switch_frame[index] == switch_before[index]);
    }
    return 0;
}

void switch_timing_test_run(void)
{
    for (unsigned index = 0; index < sizeof switch_timing_test_results / sizeof switch_timing_test_results[0]; ++index)
        switch_timing_test_results[index] = switch_test_timing(index);
}

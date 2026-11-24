/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "tx_timing.h"

static __attribute__((noinline)) uint16_t switch_a3_timing_read16(const uint8_t *bytes)
{
    return bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t switch_a3_timing_read32(const uint8_t *bytes)
{
    return switch_a3_timing_read16(bytes) | ((uint32_t)switch_a3_timing_read16(bytes + 2) << 16);
}

int switch_a3_tx_timing(uint8_t *frame, uint32_t bytes, uint64_t now, uint64_t expiry, uint32_t *lifetime)
{
    /* D11 stores TSF[23:8]; constrain deadlines to its half-wrap interval. */
    expiry &= ~(uint64_t)255u;
    if (expiry <= now)
        return -2;
    if (expiry - now > 0x7fffffu)
        return -1;
    *lifetime = (uint32_t)expiry;
    static const uint8_t prefix[6] = {0x7f, 0, 0x17, 0xf2, 8, 0x10};
    static const uint8_t bssid[6] = {0, 0x25, 0, 0xff, 0x94, 0x73};
    if (bytes < 32 || (frame[0] & 0xfcu) != 0xd0u || (frame[30] != 0 && frame[30] != 3))
        return 0;
    for (unsigned index = 0; index < 6; ++index)
        if (frame[24 + index] != prefix[index] || frame[16 + index] != bssid[index])
            return 0;
    if (bytes < 40 || frame[31])
        return -1;
    uint32_t sync = 0, counter = 0;
    for (uint32_t at = 40; at < bytes;) {
        if (bytes - at < 3)
            return -1;
        const uint32_t length = switch_a3_timing_read16(frame + at + 1);
        if (length > bytes - at - 3)
            return -1;
        if (frame[at] == 4) {
            if (sync || length < 33)
                return -1;
            counter = switch_a3_timing_read16(frame + at + 4);
            if (!counter || counter > 64)
                return -1;
            sync = at + 3;
        }
        at += length + 3;
    }
    const uint32_t elapsed = (uint32_t)now - switch_a3_timing_read32(frame + 36);
    if (!sync || elapsed >= counter * 1024u)
        return -1;
    uint64_t boundary = (now + counter * 1024u - elapsed) & ~(uint64_t)255u;
    if (boundary <= now)
        return -2;
    if (boundary < expiry)
        *lifetime = (uint32_t)boundary;
    /* Legacy peers use the countdown directly at RX. Keep its fractional TU in PHY minus target. */
    counter -= elapsed >> 10;
    frame[sync + 1] = (uint8_t)counter;
    frame[sync + 15] = counter > 48 ? (uint8_t)(counter - 48) : 0;
    frame[sync + 16] = 0;
    frame[sync + 29] = (uint8_t)((frame[sync + 29] & 0xfcu) | ((64 - counter) >> 4));
    uint32_t timestamp = (uint32_t)now;
    uint32_t target = timestamp - (elapsed & 1023u);
    for (unsigned index = 0; index < 4; ++index) {
        frame[32 + index] = (uint8_t)timestamp;
        frame[36 + index] = (uint8_t)target;
        timestamp >>= 8;
        target >>= 8;
    }
    return 1;
}

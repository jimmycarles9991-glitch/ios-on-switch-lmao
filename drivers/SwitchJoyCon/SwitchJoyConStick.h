// SPDX-License-Identifier: BSD-2-Clause-Patent
// A rail Joy-Con's analog stick as the Joy-Con describes it in its SPI flash (SwitchJoyConRail.cpp reads it at every
// session start; the host test test/switch_joycon_stick_test.cpp drives this header).
//
// Layouts (dekuNukem spi_flash_notes.md 92-126, 154-200, 262-280; L4T joycon-serdev.c 121-136, 951-990): factory
// calibration 0x603D (L) / 0x6046 (R), 9 bytes; user calibration 0x8010 (L) / 0x801B (R), the magic B2 A1 and 9 bytes,
// used instead of the factory one when the magic is there; stick parameters 0x6086 (L) / 0x6098 (R), 18 bytes. Each is
// 12-bit values packed two to three bytes. Calibration: the left stick's block is max above, centre, min below, the
// right stick's centre, min below, max above (x then y each time). Parameters: value 2 is the dead zone, radial ("used
// to all directions, so it isn't divided by 2"), 0xAE on a Joy-Con, about 15% of an axis; value 3 the range ratio.
// Horizon reads the same blocks at every attach (deku packet_parse/c2j.txt 17-25). Erased flash reads FF: such a block
// is absent and the defaults stay.
//
// A Joy-Con reports one stick: (L) in report bytes 6..8, (R) in 9..11; the other reads 0, 0 (deku j2c.txt).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_JOYCON_STICK_CENTRE    2048u
#define SWITCH_JOYCON_STICK_DEAD_ZONE 0xaeu  // every Joy-Con's parameter block in deku's notes and capture

enum switch_joycon_stick_read {
    SWITCH_JOYCON_STICK_FACTORY,
    SWITCH_JOYCON_STICK_USER,
    SWITCH_JOYCON_STICK_PARAMS,
    SWITCH_JOYCON_STICK_READS
};

struct switch_joycon_stick {
    bool left;
    bool user;    // the centre is the user calibration's
    bool params;  // the dead zone is the Joy-Con's own
    uint16_t centre[2];
    uint16_t dead_zone;
};

static inline void switch_joycon_stick_defaults(struct switch_joycon_stick *s, bool left)
{
    s->left = left;
    s->user = false;
    s->params = false;
    s->centre[0] = SWITCH_JOYCON_STICK_CENTRE;
    s->centre[1] = SWITCH_JOYCON_STICK_CENTRE;
    s->dead_zone = SWITCH_JOYCON_STICK_DEAD_ZONE;
}

// The `index`th 12-bit value of a packed block (deku spi_flash_notes.md 158-167).
static inline unsigned switch_joycon_stick_unpack(const uint8_t *block, unsigned index)
{
    const uint8_t *b = block + 3u * (index / 2u);
    return index % 2u == 0u ? (unsigned)(b[1] & 0x0fu) << 8 | b[0] : (unsigned)b[2] << 4 | (unsigned)b[1] >> 4;
}

static inline uint32_t switch_joycon_stick_address(bool left, unsigned read)
{
    static const uint32_t addresses[SWITCH_JOYCON_STICK_READS][2] = {
        {0x6046u, 0x603du}, {0x801bu, 0x8010u}, {0x6098u, 0x6086u}};
    return addresses[read][left ? 1 : 0];
}

static inline uint8_t switch_joycon_stick_size(unsigned read)
{
    return read == SWITCH_JOYCON_STICK_PARAMS ? 18u : read == SWITCH_JOYCON_STICK_USER ? 11u : 9u;
}

// Subcommand 0x10's data for `read` of this stick: address (LE), size.
static inline void switch_joycon_stick_request(const struct switch_joycon_stick *s, unsigned read, uint8_t out[5])
{
    const uint32_t address = switch_joycon_stick_address(s->left, read);
    out[0] = (uint8_t)address;
    out[1] = (uint8_t)(address >> 8);
    out[2] = (uint8_t)(address >> 16);
    out[3] = (uint8_t)(address >> 24);
    out[4] = switch_joycon_stick_size(read);
}

static inline bool switch_joycon_stick_erased(const uint8_t *bytes, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        if (bytes[i] != 0xffu)
            return false;
    }
    return true;
}

static inline bool switch_joycon_stick_calibration(struct switch_joycon_stick *s, const uint8_t *block)
{
    const unsigned at = s->left ? 2u : 0u;
    const unsigned x = switch_joycon_stick_unpack(block, at), y = switch_joycon_stick_unpack(block, at + 1u);
    for (unsigned i = 0; i < 6; ++i) {
        if (switch_joycon_stick_unpack(block, i) == 0u || switch_joycon_stick_unpack(block, i) == 0xfffu)
            return false;
    }
    s->centre[0] = (uint16_t)x;
    s->centre[1] = (uint16_t)y;
    return true;
}

// Subcommand 0x10's reply data (address LE, size, bytes) for `read`: false when it answers a different read. A block
// that is erased or out of range leaves what the earlier reads (or the defaults) gave.
static inline bool switch_joycon_stick_reply(struct switch_joycon_stick *s, unsigned read, const uint8_t *data,
                                             size_t length)
{
    const uint8_t size = switch_joycon_stick_size(read);
    if (length < 5u + size)
        return false;
    const uint32_t address = data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
    if (address != switch_joycon_stick_address(s->left, read) || data[4] != size)
        return false;
    const uint8_t *bytes = data + 5;
    if (switch_joycon_stick_erased(bytes, size))
        return true;
    if (read == SWITCH_JOYCON_STICK_FACTORY) {
        (void)switch_joycon_stick_calibration(s, bytes);
    } else if (read == SWITCH_JOYCON_STICK_USER) {
        if (bytes[0] == 0xb2u && bytes[1] == 0xa1u && switch_joycon_stick_calibration(s, bytes + 2))
            s->user = true;
    } else {
        const unsigned dead_zone = switch_joycon_stick_unpack(bytes, 2);
        if (dead_zone != 0u && dead_zone < SWITCH_JOYCON_STICK_CENTRE) {
            s->dead_zone = (uint16_t)dead_zone;
            s->params = true;
        }
    }
    return true;
}

// The stick's axis (0 x, 1 y) in a 0x21/0x30/0x31 report of at least 12 bytes.
static inline unsigned switch_joycon_stick_axis(const struct switch_joycon_stick *s, const uint8_t *report,
                                                unsigned axis)
{
    const uint8_t *b = report + (s->left ? 6 : 9);
    return axis == 0u ? (unsigned)(b[1] & 0x0fu) << 8 | b[0] : (unsigned)b[2] << 4 | (unsigned)b[1] >> 4;
}

// Squared distance of the report's stick from `from` (a calibrated centre or an earlier position).
static inline uint32_t switch_joycon_stick_distance2(const struct switch_joycon_stick *s, const uint8_t *report,
                                                     const uint16_t from[2])
{
    uint32_t d2 = 0;
    for (unsigned axis = 0; axis < 2; ++axis) {
        const unsigned v = switch_joycon_stick_axis(s, report, axis);
        const uint32_t d = v > from[axis] ? v - from[axis] : from[axis] - v;
        d2 += d * d;
    }
    return d2;
}

static inline bool switch_joycon_stick_within(const struct switch_joycon_stick *s, const uint8_t *report,
                                              const uint16_t from[2], unsigned radius)
{
    return switch_joycon_stick_distance2(s, report, from) <= (uint32_t)radius * radius;
}

#ifdef __cplusplus
}
#endif

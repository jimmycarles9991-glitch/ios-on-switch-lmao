// SPDX-License-Identifier: BSD-2-Clause-Patent
// What the host puts on a Joy-Con rail (SwitchJoyConRail.cpp, "Protocol" and "Pairing"): the packet frame, the
// Bluetooth output report 0x01 that carries a subcommand in it, and the pairing request.
//
// Every subcommand report Horizon sends on the rail is 0x31 bytes, whatever the subcommand carries (dekuNukem
// packet_parse/c2j.txt). The Joy-Con reads a pairing request's host record at fixed offsets, so from a shorter report it
// takes whatever its buffer held past the end.
//
// Pairing request (subcommand 0x01; deku bluetooth_hid_subcommands_notes.md, and the Joy-Con firmware's wired twin,
// Nwcp 0x18, in shuffle2's brcm_flash.c gist): a type, then for types 1 and 4 the host record, 0x20 bytes: BD_ADDR
// least significant byte first, Class of Device, name (20 bytes), capability, two zeros. Type 1 pairs to that host (the
// Joy-Con answers with its own BD_ADDR), 2 returns the link key, 3 saves the pairing with the record's capability; 4 is
// what Horizon sends at every attach: it answers 3 when the Joy-Con is already paired to that host and otherwise does
// what 1 does. Horizon's record (c2j.txt line 10): class 00 04 3c, "Nintendo Switch", capability 0x68 (secure
// connections, TBFC host, iTBFC page: hekate jc_hid_in_pair_data_t), the value deku's notes find in slots Horizon made.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_JOYCON_RAIL_HEADER        12u
#define SWITCH_JOYCON_SUBCOMMAND_REPORT  0x31u
#define SWITCH_JOYCON_SUBCOMMAND_DATA    (SWITCH_JOYCON_SUBCOMMAND_REPORT - 11u)
#define SWITCH_JOYCON_HOST_RECORD        0x20u
#define SWITCH_JOYCON_PAIRING_REQUEST    (1u + SWITCH_JOYCON_HOST_RECORD)
#define SWITCH_JOYCON_HOST_CAPABILITY    0x68u
#define SWITCH_JOYCON_PAIR_HOST          0x01u
#define SWITCH_JOYCON_PAIR_KEY           0x02u
#define SWITCH_JOYCON_PAIR_SAVE          0x03u
#define SWITCH_JOYCON_PAIR_UNLESS_PAIRED 0x04u

// CRC-8, polynomial 0x8D, initial 0, MSB first (deku README "Pesky checksums").
static inline uint8_t switch_joycon_crc8(const uint8_t *data, size_t length)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            const unsigned shifted = (unsigned)crc << 1;
            crc = (uint8_t)((crc & 0x80) ? shifted ^ 0x8du : shifted);
        }
    }
    return crc;
}

// One host packet into `out` (room for SWITCH_JOYCON_RAIL_HEADER + length); returns its length.
static inline size_t switch_joycon_rail_packet(uint8_t *out, uint8_t cmd, uint8_t subcmd, const uint8_t *payload,
                                               uint16_t length)
{
    const uint16_t rest = (uint16_t)(7u + length);
    const uint8_t header[SWITCH_JOYCON_RAIL_HEADER] = {0x19,
                                                       0x01,
                                                       0x03,
                                                       (uint8_t)rest,
                                                       (uint8_t)(rest >> 8),
                                                       cmd,
                                                       subcmd,
                                                       (uint8_t)length,
                                                       (uint8_t)(length >> 8),
                                                       0,
                                                       length ? switch_joycon_crc8(payload, length) : (uint8_t)0,
                                                       0};
    memcpy(out, header, SWITCH_JOYCON_RAIL_HEADER);
    out[11] = switch_joycon_crc8(out + 5, 6);
    if (length)
        memcpy(out + SWITCH_JOYCON_RAIL_HEADER, payload, length);
    return SWITCH_JOYCON_RAIL_HEADER + length;
}

// Output report 0x01: packet counter (mod 16), neutral rumble, subcommand `id` and its `data` (at most
// SWITCH_JOYCON_SUBCOMMAND_DATA bytes), zeros to the end.
static inline void switch_joycon_subcommand_report(uint8_t report[SWITCH_JOYCON_SUBCOMMAND_REPORT], uint8_t counter,
                                                   uint8_t id, const uint8_t *data, size_t length)
{
    static const uint8_t neutral[8] = {0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};
    memset(report, 0, SWITCH_JOYCON_SUBCOMMAND_REPORT);
    report[0] = 0x01;
    report[1] = (uint8_t)(counter & 0xf);
    memcpy(report + 2, neutral, sizeof(neutral));
    report[10] = id;
    if (length)
        memcpy(report + 11, data, length);
}

// Subcommand 0x01's data for `type`; types 1 and 4 carry the host record for `host` (display order). Returns its length.
static inline size_t switch_joycon_pairing_request(uint8_t out[SWITCH_JOYCON_PAIRING_REQUEST], uint8_t type,
                                                   const uint8_t host[6])
{
    static const uint8_t hostClass[3] = {0x00, 0x04, 0x3c};
    static const char hostName[20] = "Nintendo Switch";
    memset(out, 0, SWITCH_JOYCON_PAIRING_REQUEST);
    out[0] = type;
    if (type != SWITCH_JOYCON_PAIR_HOST && type != SWITCH_JOYCON_PAIR_UNLESS_PAIRED)
        return 1;
    for (unsigned i = 0; i < 6; ++i)
        out[1 + i] = host[5 - i];
    memcpy(out + 7, hostClass, sizeof(hostClass));
    memcpy(out + 10, hostName, sizeof(hostName));
    out[30] = SWITCH_JOYCON_HOST_CAPABILITY;
    return SWITCH_JOYCON_PAIRING_REQUEST;
}

#ifdef __cplusplus
}
#endif

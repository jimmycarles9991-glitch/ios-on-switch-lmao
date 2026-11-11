// SPDX-License-Identifier: BSD-2-Clause-Patent
// A Joy-Con's Bluetooth pairing log (SPI 0x2000, SwitchJoyConRail.cpp "Pairing"), as the rail reads it: 0x26-byte
// slots, magic 0x95 the one in use, 0x00 an earlier pairing, anything else (erased flash) the end of the log.
//
// Slot layout (deku spi_flash_notes.md; hekate jc_hid_in_pair_data_t): magic, size 0x22, checksum (2), host BD_ADDR
// (display order), LTK (16), zeros (10), host capability, zero. The checksum is the byte sum of offsets 4..0x25 XOR
// 0x55 (hekate _jc_validate_pairing_info). The capability is a bitfield (hekate: bit 3 secure connections, bit 5 paired
// to a TBFC host, bit 6 iTBFC page), 0x68 for a Switch and 0x08 for a PC, and something else again for a host such as
// iOS that pairs over the air: hekate assumes one of its two values because it reads only the first 0x1A bytes, and
// that assumption rejects every other host's slot. So the slot in use is read whole (an SPI read takes at most 0x1D
// bytes: its head, then its tail) and its checksum computed from what is there.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_JOYCON_SLOT_BYTES 0x26u
#define SWITCH_JOYCON_SLOT_HEAD  0x1Au  // magic .. LTK
#define SWITCH_JOYCON_SLOT_TAIL  (SWITCH_JOYCON_SLOT_BYTES - SWITCH_JOYCON_SLOT_HEAD)
#define SWITCH_JOYCON_SLOT_CAPS  0x24u
#define SWITCH_JOYCON_SLOT_MAGIC 0x95u
#define SWITCH_JOYCON_SLOT_SIZE  0x22u

enum switch_joycon_slot_kind {
    SWITCH_JOYCON_SLOT_EARLIER,  // an earlier pairing: the next slot
    SWITCH_JOYCON_SLOT_IN_USE,   // read its tail, then check it
    SWITCH_JOYCON_SLOT_END,      // erased or not a slot: no pairing in use
};

static inline enum switch_joycon_slot_kind switch_joycon_slot_head_kind(const uint8_t head[SWITCH_JOYCON_SLOT_HEAD])
{
    if (head[1] != SWITCH_JOYCON_SLOT_SIZE)
        return SWITCH_JOYCON_SLOT_END;
    if (head[0] == SWITCH_JOYCON_SLOT_MAGIC)
        return SWITCH_JOYCON_SLOT_IN_USE;
    return head[0] == 0x00 ? SWITCH_JOYCON_SLOT_EARLIER : SWITCH_JOYCON_SLOT_END;
}

static inline bool switch_joycon_slot_valid(const uint8_t slot[SWITCH_JOYCON_SLOT_BYTES])
{
    uint8_t sum = 0;
    for (unsigned i = 4; i < SWITCH_JOYCON_SLOT_BYTES; ++i)
        sum = (uint8_t)(sum + slot[i]);
    return (uint8_t)(sum ^ 0x55u) == slot[2];
}

#ifdef __cplusplus
}
#endif

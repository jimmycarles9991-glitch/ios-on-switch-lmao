/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWITCH_A3_IMAGE_H
#define SWITCH_A3_IMAGE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "abi.h"

enum {
    SWITCH_A3_IMAGE_BASE = 0x180000,
    SWITCH_A3_IMAGE_BYTES = 583641,
    SWITCH_A3_PAYLOAD_BASE = 0x20e7dc,
    SWITCH_A3_HEAP_CALL = 0x1e006c,
    SWITCH_A3_ATTACH_RETURN = 0x1e9b8e,
    SWITCH_A3_RX_CALL = 0x1a7d0c,
    SWITCH_A3_TX_POINTER = 0x1de59c,
    SWITCH_A3_TX_STATUS_CALL = 0x196a86,
    SWITCH_A3_PM_STATUS_CALL = 0x1969fc,
    SWITCH_A3_CHANNEL_CALL = 0x1cfb1a,
    SWITCH_A3_PM_PREPARE_BRANCH = 0x1cf6e6,
    SWITCH_A3_PM_SUBMIT_CALL = 0x19542c,
    SWITCH_A3_PM_SET_ENTRY = 0x193662,
};

static inline int switch_a3_state_range_valid(uint32_t address, uint32_t ram_begin, uint32_t ram_bytes)
{
    return !(address & 3u) && ram_bytes >= sizeof(struct switch_a3_caps) && ram_bytes <= UINT32_MAX - ram_begin &&
           address >= ram_begin && address - ram_begin <= ram_bytes - sizeof(struct switch_a3_caps);
}

struct switch_a3_candidate_entries {
    uint32_t heap, attach, rx, tx, tx_status, pm_status, channel, pm_prepare, pm_submit, pm_set;
};

static inline void switch_a3_branch(uint8_t *out, uint32_t site, uint32_t target, int link)
{
    const uint32_t offset = target - site - 4;
    const uint32_t sign = (offset >> 24) & 1;
    const uint32_t j1 = ~((offset >> 23) ^ sign) & 1;
    const uint32_t j2 = ~((offset >> 22) ^ sign) & 1;
    const uint16_t first = (uint16_t)(0xf000 | sign << 10 | ((offset >> 12) & 0x3ff));
    const uint16_t second = (uint16_t)((link ? 0xd000 : 0x9000) | j1 << 13 | j2 << 11 | ((offset >> 1) & 0x7ff));
    out[0] = (uint8_t)first;
    out[1] = (uint8_t)(first >> 8);
    out[2] = (uint8_t)second;
    out[3] = (uint8_t)(second >> 8);
}

static inline int switch_a3_patch_bounded_image(const uint8_t *image, size_t image_length, uint8_t *out,
                                                size_t out_length, const uint8_t *payload, size_t payload_length,
                                                uint32_t heap_entry, uint32_t attach_entry, uint32_t rx_entry,
                                                size_t maximum)
{
    static const uint8_t heap_original[4] = {0xff, 0xf7, 0x96, 0xfd};
    static const uint8_t attach_original[4] = {0x20, 0x46, 0x11, 0xb0};
    static const uint8_t rx_original[4] = {0xf5, 0xf7, 0x74, 0xf9};
    const size_t payload_offset = SWITCH_A3_PAYLOAD_BASE - SWITCH_A3_IMAGE_BASE;
    if (!image || !out || !payload || image_length != SWITCH_A3_IMAGE_BYTES || !payload_length ||
        payload_length > maximum || payload_length & 3 || out_length < payload_offset + payload_length ||
        heap_entry < SWITCH_A3_PAYLOAD_BASE || heap_entry >= SWITCH_A3_PAYLOAD_BASE + payload_length ||
        attach_entry < SWITCH_A3_PAYLOAD_BASE || attach_entry >= SWITCH_A3_PAYLOAD_BASE + payload_length ||
        rx_entry < SWITCH_A3_PAYLOAD_BASE || rx_entry >= SWITCH_A3_PAYLOAD_BASE + payload_length ||
        (heap_entry | attach_entry | rx_entry) & 1)
        return 0;
    if (memcmp(image + SWITCH_A3_HEAP_CALL - SWITCH_A3_IMAGE_BASE, heap_original, sizeof heap_original) ||
        memcmp(image + SWITCH_A3_ATTACH_RETURN - SWITCH_A3_IMAGE_BASE, attach_original, sizeof attach_original) ||
        memcmp(image + SWITCH_A3_RX_CALL - SWITCH_A3_IMAGE_BASE, rx_original, sizeof rx_original))
        return 0;
    memcpy(out, image, image_length);
    memset(out + image_length, 0, payload_offset - image_length);
    memcpy(out + payload_offset, payload, payload_length);
    switch_a3_branch(out + SWITCH_A3_HEAP_CALL - SWITCH_A3_IMAGE_BASE, SWITCH_A3_HEAP_CALL, heap_entry, 1);
    switch_a3_branch(out + SWITCH_A3_ATTACH_RETURN - SWITCH_A3_IMAGE_BASE, SWITCH_A3_ATTACH_RETURN, attach_entry, 0);
    switch_a3_branch(out + SWITCH_A3_RX_CALL - SWITCH_A3_IMAGE_BASE, SWITCH_A3_RX_CALL, rx_entry, 1);
    return 1;
}

static inline int switch_a3_patch_image(const uint8_t *image, size_t image_length, uint8_t *out, size_t out_length,
                                        const uint8_t *payload, size_t payload_length, uint32_t heap_entry,
                                        uint32_t attach_entry, uint32_t rx_entry)
{
    return switch_a3_patch_bounded_image(image, image_length, out, out_length, payload, payload_length, heap_entry,
                                         attach_entry, rx_entry, SWITCH_A3_BOOTSTRAP_MAX_BYTES);
}

static inline int switch_a3_patch_candidate_image(const uint8_t *image, size_t image_length, uint8_t *out,
                                                  size_t out_length, const uint8_t *payload, size_t payload_length,
                                                  const struct switch_a3_candidate_entries *entries)
{
    static const uint32_t sites[] = {SWITCH_A3_TX_POINTER, SWITCH_A3_TX_STATUS_CALL, SWITCH_A3_PM_STATUS_CALL,
                                     SWITCH_A3_CHANNEL_CALL, SWITCH_A3_PM_PREPARE_BRANCH, SWITCH_A3_PM_SUBMIT_CALL,
                                     SWITCH_A3_PM_SET_ENTRY};
    static const uint8_t original[][4] = {
        {0xa5, 0xc7, 0x18, 0x00},
        {0xc6, 0xf6, 0x21, 0xfe},
        {0xff, 0xf7, 0x98, 0xfa},
        {0xc3, 0xf7, 0x2b, 0xfe},
        {0x1b, 0x7a, 0x53, 0xb9},
        {0xff, 0xf7, 0x2e, 0xff},
        {0xd0, 0xf8, 0x20, 0x31},
    };
    if (!image || image_length != SWITCH_A3_IMAGE_BYTES || !entries || payload_length > SWITCH_A3_CANDIDATE_MAX_BYTES)
        return 0;
    const uint32_t targets[] = {entries->tx, entries->tx_status, entries->pm_status, entries->channel,
                                entries->pm_prepare, entries->pm_submit, entries->pm_set};
    for (size_t index = 0; index < sizeof sites / sizeof *sites; ++index)
        if (targets[index] < SWITCH_A3_PAYLOAD_BASE || targets[index] >= SWITCH_A3_PAYLOAD_BASE + payload_length ||
            (targets[index] & 1u) || memcmp(image + sites[index] - SWITCH_A3_IMAGE_BASE, original[index], 4))
            return 0;
    if (!switch_a3_patch_bounded_image(image, image_length, out, out_length, payload, payload_length, entries->heap,
                                       entries->attach, entries->rx, SWITCH_A3_CANDIDATE_MAX_BYTES))
        return 0;
    const uint32_t tx = entries->tx | 1u;
    uint8_t *pointer = out + SWITCH_A3_TX_POINTER - SWITCH_A3_IMAGE_BASE;
    for (uint32_t index = 0; index < 4; ++index)
        pointer[index] = (uint8_t)(tx >> (index * 8));
    for (size_t index = 1; index < sizeof sites / sizeof *sites; ++index)
        switch_a3_branch(out + sites[index] - SWITCH_A3_IMAGE_BASE, sites[index], targets[index],
                          sites[index] != SWITCH_A3_PM_PREPARE_BRANCH && sites[index] != SWITCH_A3_PM_SET_ENTRY);
    return 1;
}

#endif

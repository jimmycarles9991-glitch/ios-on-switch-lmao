// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchBluetooth's receive side by APB DMA (SwitchBluetooth.cpp, header "RX by APB DMA"): the H4 framing the
// interrupt path keeps of what it takes, the length of each DMA transfer, and what a transfer shows as moved. Plain C,
// so test/switch_bluetooth_rx_dma_test.cpp drives the very same code on the host against a byte-timed model of UART-D.
//
// Why each transfer is as long as it is. Every byte the controller sends goes through the channel, one APB read and one
// word each (TegraAPBDMA.h tegra_apbdma_start_rx_words: the UART's RX trigger at one byte), so no byte is ever left in
// the FIFO for PIO to race the channel for. The CPU looks at what moved when the line goes idle (EORD, 4 characters
// after the last byte: an isolated packet costs that one interrupt, as its RX timeout did under PIO) and when a
// transfer ends (end of count). Packets the controller sends back to back leave the line no idle gap, so a transfer's
// end is what bounds how long the end of one waits to be seen: each transfer ends at most SWITCH_BT_RX_DMA_REACH bytes
// after the end of the packet it was started in (the header's remainder, if the length is not known yet), or that many
// bytes on when started between packets. 5 is the longest H4 header (an ACL packet's type and 4 bytes), so an end of
// count that comes inside a packet always knows its length, and the next transfer runs to its end plus the reach: about
// two interrupts for any packet longer than the reach, back to back or not, and one for a shorter isolated one. A
// longer reach only makes a packet sent back to back wait longer; a shorter one costs a third interrupt per packet (the
// host model at the peak load: reach 4, 5, 8, 15 -> 1922, 1531, 1531, 1380 interrupts/s, the worst HID report seen 27,
// 30, 40, 64 us after its last byte; PIO at its 16-byte trigger: 4343/s and 64 us). A packet that goes on past a
// transfer's end is followed by one sized to its end. Framing lost (a line error, a byte that is no H4 packet type)
// keeps transfers at the reach until the work loop's parser, the reference, puts the framing back
// (SwitchBluetooth::syncFramerLocked).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_BT_RX_DMA_REACH 5u     // bytes a transfer runs past the end of the packet it was started in
#define SWITCH_BT_RX_DMA_WORDS 2048u  // the channel's buffer: one word per byte, every transfer from word 0

enum {
    SWITCH_BT_H4_ACL = 2,
    SWITCH_BT_H4_SCO = 3,
    SWITCH_BT_H4_EVENT = 4
};

// The controller's H4 stream as the interrupt path takes it: state 0 a packet type next, 1 in the header (have of
// need bytes after the type), 2 in the payload (left bytes to go). lost: not following (a line error or a byte that is
// no packet type) until the next sync.
struct switch_bt_h4_frame {
    uint8_t state, type, have, need;
    uint8_t header[4];
    uint32_t left;
    bool lost;
};

static inline uint8_t switch_bt_h4_header_bytes(uint8_t type)
{
    switch (type) {
    case SWITCH_BT_H4_EVENT:
        return 2;
    case SWITCH_BT_H4_ACL:
        return 4;
    case SWITCH_BT_H4_SCO:
        return 3;
    default:
        return 0;
    }
}

static inline uint32_t switch_bt_h4_length(const struct switch_bt_h4_frame *f)
{
    if (f->type == SWITCH_BT_H4_EVENT)
        return f->header[1];
    if (f->type == SWITCH_BT_H4_ACL)
        return f->header[2] | (uint32_t)f->header[3] << 8;
    return f->header[2];
}

// One more byte; true when it ends a packet.
static inline bool switch_bt_h4_frame_byte(struct switch_bt_h4_frame *f, uint8_t byte)
{
    if (f->lost)
        return false;
    if (f->state == 0) {
        f->type = byte;
        f->need = switch_bt_h4_header_bytes(byte);
        f->have = 0;
        f->state = 1;
        f->lost = f->need == 0;
        return false;
    }
    if (f->state == 1) {
        f->header[f->have++] = byte;
        if (f->have < f->need)
            return false;
        f->left = switch_bt_h4_length(f);
        f->state = f->left ? 2 : 0;
        return f->left == 0;
    }
    if (--f->left)
        return false;
    f->state = 0;
    return true;
}

// Bytes to the end of the packet being taken as far as they are known (the header's remainder while the length is not
// in yet; 0 between packets); false when the framing is lost.
static inline bool switch_bt_h4_frame_left(const struct switch_bt_h4_frame *f, uint32_t *left)
{
    uint32_t n = 0;
    if (f->state == 1)
        n = (uint32_t)(f->need - f->have);
    if (f->state == 2)
        n = f->left;
    *left = n;
    return !f->lost;
}

// Words for the next transfer (see the header): to the known end of the current packet and the reach past it, within
// the buffer; the reach alone when the framing is lost.
static inline uint32_t switch_bt_rx_dma_length(const struct switch_bt_h4_frame *f)
{
    uint32_t left = 0;
    const bool known = switch_bt_h4_frame_left(f, &left);
    uint32_t words = (known ? left : 0) + SWITCH_BT_RX_DMA_REACH;
    if (words > SWITCH_BT_RX_DMA_WORDS)
        words = SWITCH_BT_RX_DMA_WORDS;
    return words;
}

// Words of a transfer of `length` that the channel shows written, given its moved bytes (TegraAPBDMA.h
// tegra_apbdma_moved) and whether its end of count is pending, never fewer than the `taken` already (the count can
// read as reloaded just before the end of count shows).
static inline uint32_t switch_bt_rx_dma_visible(uint32_t length, uint32_t movedBytes, bool ended, uint32_t taken)
{
    uint32_t words = ended ? length : movedBytes / 4;
    if (words > length)
        words = length;
    return words < taken ? taken : words;
}

#ifdef __cplusplus
}
#endif

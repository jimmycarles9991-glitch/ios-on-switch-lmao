// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "SwitchDPAux.h"
#include "SwitchDPModes.h"

#define SWITCH_DP_CEC_INVALID_PHYSICAL 0xffffu
#define SWITCH_DP_CEC_CONTROL          0x5c0u
#define SWITCH_DP_CEC_RX_INFO          0x5c1u
#define SWITCH_DP_CEC_TX_CONTROL       0x5c2u
#define SWITCH_DP_CEC_RX_BUFFER        0x5d0u
#define SWITCH_DP_CEC_TX_END           0x5f0u

enum switch_dp_cec_phase {
    SWITCH_DP_CEC_OFF,
    SWITCH_DP_CEC_OUI,
    SWITCH_DP_CEC_CONFIG,
    SWITCH_DP_CEC_CLEAR_RX,
    SWITCH_DP_CEC_CLEAR_TX,
    SWITCH_DP_CEC_ENABLE,
    SWITCH_DP_CEC_IDLE,
    SWITCH_DP_CEC_LOGICAL,
    SWITCH_DP_CEC_DATA,
    SWITCH_DP_CEC_SEND,
    SWITCH_DP_CEC_WAIT,
    SWITCH_DP_CEC_STATUS,
    SWITCH_DP_CEC_STOP
};

enum switch_dp_cec_packet {
    SWITCH_DP_CEC_POLL,
    SWITCH_DP_CEC_IMAGE,
    SWITCH_DP_CEC_PHYSICAL,
    SWITCH_DP_CEC_ACTIVE,
    SWITCH_DP_CEC_REPLY,
    SWITCH_DP_CEC_DONE
};

enum switch_dp_cec_io {
    SWITCH_DP_CEC_IO_NONE,
    SWITCH_DP_CEC_IO_MAIN,
    SWITCH_DP_CEC_IO_IRQ_STATUS,
    SWITCH_DP_CEC_IO_IRQ,
    SWITCH_DP_CEC_IO_RX_INFO,
    SWITCH_DP_CEC_IO_RX_DATA,
    SWITCH_DP_CEC_IO_RX_CLEAR
};

struct switch_dp_cec {
    enum switch_dp_cec_phase phase;
    enum switch_dp_cec_packet packet, wake;
    enum switch_dp_cec_io io;
    uint16_t physical;
    uint8_t logical, candidate, control, message[16], length, reply[16], reply_length, rx_info;
    uint8_t rx_stage;
    bool enabled, irq, service, active, wait_pending, deadline, retry_on_hpd, wake_sent;
};

static inline uint16_t switch_dp_cec_physical(const uint8_t *edid, uint32_t valid_blocks)
{
    for (unsigned b = 1; (valid_blocks >>= 1) != 0; ++b) {
        if (!(valid_blocks & 1u))
            continue;
        const uint8_t *cta = edid + (size_t)b * 128;
        if (cta[0] != 2 || cta[2] < 4 || cta[2] > 127)
            continue;
        for (unsigned at = 4; at < cta[2];) {
            const unsigned length = cta[at] & 31u;
            if (at + 1 + length > cta[2])
                break;
            if ((cta[at] >> 5) == 3 && length >= 5 && cta[at + 1] == 3 && cta[at + 2] == 12 && cta[at + 3] == 0) {
                const uint16_t pa = (uint16_t)((uint16_t)cta[at + 4] << 8 | cta[at + 5]);
                bool ended = false, valid = pa != 0 && pa != SWITCH_DP_CEC_INVALID_PHYSICAL;
                for (int shift = 12; shift >= 0; shift -= 4) {
                    const unsigned digit = (pa >> (unsigned)shift) & 15u;
                    if (!digit)
                        ended = true;
                    else if (ended)
                        valid = false;
                }
                return valid ? pa : SWITCH_DP_CEC_INVALID_PHYSICAL;
            }
            at += 1 + length;
        }
    }
    return SWITCH_DP_CEC_INVALID_PHYSICAL;
}

static inline bool switch_dp_cec_bridge(const uint8_t *oui)
{
    return oui[0] == 0 && ((oui[1] == 0x80 && oui[2] == 0xe1) || (oui[1] == 0xe0 && oui[2] == 0x4c));
}

static inline uint8_t switch_dp_cec_candidate(unsigned candidate)
{
    return candidate == 0 ? 4 : candidate == 1 ? 8 : 11;
}

static inline void switch_dp_cec_start(struct switch_dp_cec *s)
{
    __builtin_memset(s, 0, sizeof(*s));
    s->phase = SWITCH_DP_CEC_OUI;
    s->logical = 15;
    s->physical = SWITCH_DP_CEC_INVALID_PHYSICAL;
    s->wake = SWITCH_DP_CEC_POLL;
    s->retry_on_hpd = true;
}

static inline void switch_dp_cec_reconnect(struct switch_dp_cec *s)
{
    const bool wake_sent = s->wake_sent;
    switch_dp_cec_start(s);
    s->wake_sent = wake_sent;
}

static inline void switch_dp_cec_release(struct switch_dp_cec *s)
{
    s->phase = s->enabled ? SWITCH_DP_CEC_STOP : SWITCH_DP_CEC_OFF;
    s->physical = SWITCH_DP_CEC_INVALID_PHYSICAL;
    s->active = s->irq = s->service = s->wait_pending = false;
    s->reply_length = s->rx_stage = 0;
}

static inline void switch_dp_cec_make_packet(struct switch_dp_cec *s)
{
    s->packet = s->reply_length ? SWITCH_DP_CEC_REPLY : s->wake;
    s->length = 0;
    if (s->packet == SWITCH_DP_CEC_REPLY) {
        s->length = s->reply_length;
        __builtin_memcpy(s->message, s->reply, s->length);
        s->reply_length = 0;
    } else if (s->packet == SWITCH_DP_CEC_POLL) {
        const uint8_t address = switch_dp_cec_candidate(s->candidate);
        s->message[0] = (uint8_t)((address << 4) | address);
        s->length = 1;
    } else if (s->packet == SWITCH_DP_CEC_IMAGE) {
        s->message[0] = (uint8_t)(s->logical << 4);
        s->message[1] = 4;
        s->length = 2;
    } else if ((s->packet == SWITCH_DP_CEC_PHYSICAL || s->packet == SWITCH_DP_CEC_ACTIVE) &&
               s->physical != SWITCH_DP_CEC_INVALID_PHYSICAL && (s->packet != SWITCH_DP_CEC_ACTIVE || s->active)) {
        s->message[0] = (uint8_t)((s->logical << 4) | 15);
        s->message[1] = s->packet == SWITCH_DP_CEC_PHYSICAL ? 0x84 : 0x82;
        s->message[2] = (uint8_t)(s->physical >> 8);
        s->message[3] = (uint8_t)s->physical;
        s->length = 4;
        if (s->packet == SWITCH_DP_CEC_PHYSICAL) {
            s->message[4] = 4;
            s->length = 5;
        }
    }
    if (s->length)
        s->phase = SWITCH_DP_CEC_DATA;
}

static inline void switch_dp_cec_receive(struct switch_dp_cec *s, const uint8_t *message, unsigned length)
{
    if (length < 2 || s->logical == 15 || s->reply_length)
        return;
    const uint8_t from = message[0] >> 4, to = message[0] & 15u, opcode = message[1];
    if (from == s->logical || (to != s->logical && to != 15))
        return;
    uint8_t *reply = s->reply;
    reply[0] = (uint8_t)((s->logical << 4) | from);
    if (s->physical != SWITCH_DP_CEC_INVALID_PHYSICAL &&
        ((opcode == 0x83 && to == s->logical) || (opcode == 0x85 && to == 15 && s->active) ||
         (opcode == 0x86 && to == 15 && length == 4 && s->active && message[2] == (s->physical >> 8) &&
          message[3] == (s->physical & 255u)))) {
        reply[0] = (uint8_t)((s->logical << 4) | 15);
        reply[1] = opcode == 0x83 ? 0x84 : 0x82;
        reply[2] = (uint8_t)(s->physical >> 8);
        reply[3] = (uint8_t)s->physical;
        s->reply_length = opcode == 0x83 ? 5 : 4;
        if (opcode == 0x83)
            reply[4] = 4;
    } else if (to == s->logical && from != 15) {
        if (opcode == 0x8f || opcode == 0x9f) {
            reply[1] = opcode == 0x8f ? 0x90 : 0x9e;
            reply[2] = opcode == 0x8f ? 0 : 5;
            s->reply_length = 3;
        } else if (opcode == 0x46) {
            static const uint8_t name[] = {'S', 'w', 'i', 't', 'c', 'h'};
            reply[1] = 0x47;
            __builtin_memcpy(reply + 2, name, sizeof(name));
            s->reply_length = (uint8_t)(2 + sizeof(name));
        } else if (opcode != 0) {
            reply[1] = 0;
            reply[2] = opcode;
            reply[3] = opcode == 0xff ? 4 : 0;
            s->reply_length = 4;
        }
    }
}

static inline bool switch_dp_cec_next(struct switch_dp_cec *s, struct switch_dp_aux_request *request, uint32_t *wait_ms)
{
    *wait_ms = 0;
    s->io = SWITCH_DP_CEC_IO_NONE;
    if (s->phase == SWITCH_DP_CEC_OFF)
        return false;
    if (s->service && s->enabled && s->phase != SWITCH_DP_CEC_STOP) {
        *request = switch_dp_aux_make(SWITCH_DP_AUX_NATIVE_READ, 0x201, 1);
        s->io = SWITCH_DP_CEC_IO_IRQ_STATUS;
        return true;
    }
    if (s->irq && s->enabled && s->phase != SWITCH_DP_CEC_STOP) {
        *request = switch_dp_aux_make(SWITCH_DP_AUX_NATIVE_WRITE, 0x201, 1);
        request->data[0] = 0x40;
        s->io = SWITCH_DP_CEC_IO_IRQ;
        return true;
    }
    if (s->rx_stage && s->enabled && s->phase != SWITCH_DP_CEC_STOP) {
        s->io = s->rx_stage == 1   ? SWITCH_DP_CEC_IO_RX_INFO
                : s->rx_stage == 2 ? SWITCH_DP_CEC_IO_RX_DATA
                                   : SWITCH_DP_CEC_IO_RX_CLEAR;
        *request = switch_dp_aux_make(s->rx_stage == 3 ? SWITCH_DP_AUX_NATIVE_WRITE : SWITCH_DP_AUX_NATIVE_READ,
                                      s->rx_stage == 2 ? SWITCH_DP_CEC_RX_BUFFER : SWITCH_DP_CEC_RX_INFO,
                                      s->rx_stage == 2 ? (uint8_t)((s->rx_info & 15u) + 1u) : 1);
        request->data[0] = (uint8_t)(s->rx_info & ~0x30u);
        return true;
    }
    if (s->phase == SWITCH_DP_CEC_IDLE)
        switch_dp_cec_make_packet(s);
    if (s->phase == SWITCH_DP_CEC_WAIT) {
        if (s->wait_pending) {
            // The branch reports CEC completion separately from AUX_DONE; its reference bounds TX by this full interval.
            *wait_ms = (s->length * 24u > 50u ? s->length * 24u : 50u) + 1010u;
            s->wait_pending = false;
        }
        return false;
    }
    s->io = SWITCH_DP_CEC_IO_MAIN;
    switch (s->phase) {
    case SWITCH_DP_CEC_OUI:
        *request = switch_dp_aux_make(SWITCH_DP_AUX_NATIVE_READ, 0x500, 3);
        break;
    case SWITCH_DP_CEC_CONFIG:
        *request = switch_dp_aux_make(SWITCH_DP_AUX_NATIVE_READ, SWITCH_DP_CEC_CONTROL, 1);
        break;
    case SWITCH_DP_CEC_CLEAR_RX:
        *request = switch_dp_aux_make(SWITCH_DP_AUX_NATIVE_WRITE, SWITCH_DP_CEC_RX_INFO, 1);
        break;
    case SWITCH_DP_CEC_CLEAR_TX:
        *request = switch_dp_aux_make(SWITCH_DP_AUX_NATIVE_WRITE, SWITCH_DP_CEC_TX_CONTROL, 1);
        break;
    case SWITCH_DP_CEC_ENABLE:
    case SWITCH_DP_CEC_LOGICAL:
        *request = switch_dp_aux_make(SWITCH_DP_AUX_NATIVE_WRITE, SWITCH_DP_CEC_CONTROL, 1);
        request->data[0] = (uint8_t)((s->control & ~0x1fu) | 0x10u | s->logical);
        break;
    case SWITCH_DP_CEC_DATA:
        // STDP2550 and RTD2172N require CEC TX packets right-aligned in the 16-byte mailbox.
        *request = switch_dp_aux_make(SWITCH_DP_AUX_NATIVE_WRITE, SWITCH_DP_CEC_TX_END - s->length, s->length);
        __builtin_memcpy(request->data, s->message, s->length);
        break;
    case SWITCH_DP_CEC_SEND:
        if (s->packet == SWITCH_DP_CEC_IMAGE)
            s->wake_sent = true;
        *request = switch_dp_aux_make(SWITCH_DP_AUX_NATIVE_WRITE, SWITCH_DP_CEC_TX_CONTROL, 1);
        request->data[0] = (uint8_t)((s->length - 1u) | 0x10u);
        break;
    case SWITCH_DP_CEC_STOP:
        *request = switch_dp_aux_make(SWITCH_DP_AUX_NATIVE_WRITE, SWITCH_DP_CEC_CONTROL, 1);
        request->data[0] = (uint8_t)(s->control & ~0x10u);
        break;
    case SWITCH_DP_CEC_STATUS:
        *request = switch_dp_aux_make(SWITCH_DP_AUX_NATIVE_READ, SWITCH_DP_CEC_TX_CONTROL, 1);
        break;
    default:
        s->io = SWITCH_DP_CEC_IO_NONE;
        return false;
    }
    return true;
}

static inline void switch_dp_cec_complete(struct switch_dp_cec *s, const uint8_t *data, bool ok)
{
    if (!ok) {
        s->phase = s->enabled && s->phase != SWITCH_DP_CEC_STOP ? SWITCH_DP_CEC_STOP : SWITCH_DP_CEC_OFF;
        if (s->phase == SWITCH_DP_CEC_OFF)
            s->enabled = false;
        return;
    }
    if (s->io == SWITCH_DP_CEC_IO_IRQ_STATUS) {
        s->service = false;
        s->irq = (data[0] & 0x40u) != 0;
        return;
    }
    if (s->io == SWITCH_DP_CEC_IO_IRQ) {
        s->irq = false;
        s->rx_stage = 1;
        return;
    }
    if (s->io == SWITCH_DP_CEC_IO_RX_INFO) {
        s->rx_info = data[0];
        s->rx_stage = (data[0] & 0x10u) && !(data[0] & 0x20u) ? 2 : 3;
        return;
    }
    if (s->io == SWITCH_DP_CEC_IO_RX_DATA) {
        switch_dp_cec_receive(s, data, (s->rx_info & 15u) + 1u);
        s->rx_stage = 3;
        return;
    }
    if (s->io == SWITCH_DP_CEC_IO_RX_CLEAR) {
        s->rx_stage = 0;
        if (s->phase == SWITCH_DP_CEC_WAIT) {
            s->phase = SWITCH_DP_CEC_STATUS;
            s->deadline = false;
        }
        return;
    }
    switch (s->phase) {
    case SWITCH_DP_CEC_OUI:
        s->phase = switch_dp_cec_bridge(data) ? SWITCH_DP_CEC_CONFIG : SWITCH_DP_CEC_OFF;
        if (s->phase == SWITCH_DP_CEC_OFF)
            s->retry_on_hpd = false;
        break;
    case SWITCH_DP_CEC_CONFIG:
        s->control = data[0];
        s->phase = SWITCH_DP_CEC_CLEAR_RX;
        break;
    case SWITCH_DP_CEC_CLEAR_RX:
        s->phase = SWITCH_DP_CEC_CLEAR_TX;
        break;
    case SWITCH_DP_CEC_CLEAR_TX:
        s->phase = SWITCH_DP_CEC_ENABLE;
        break;
    case SWITCH_DP_CEC_ENABLE:
        s->enabled = true;
        s->retry_on_hpd = false;
        s->phase = SWITCH_DP_CEC_IDLE;
        break;
    case SWITCH_DP_CEC_LOGICAL:
        s->phase = SWITCH_DP_CEC_IDLE;
        break;
    case SWITCH_DP_CEC_DATA:
        s->phase = SWITCH_DP_CEC_SEND;
        break;
    case SWITCH_DP_CEC_SEND:
        s->phase = SWITCH_DP_CEC_WAIT;
        s->wait_pending = true;
        break;
    case SWITCH_DP_CEC_STOP:
        s->phase = SWITCH_DP_CEC_OFF;
        s->enabled = false;
        s->logical = 15;
        break;
    case SWITCH_DP_CEC_STATUS:
        if (data[0] & 0x10u) {
            s->phase = s->deadline ? SWITCH_DP_CEC_STOP : SWITCH_DP_CEC_WAIT;
        } else if (s->packet == SWITCH_DP_CEC_POLL) {
            if (data[0] & 0x20u) {
                s->logical = switch_dp_cec_candidate(s->candidate);
                s->wake = s->wake_sent ? SWITCH_DP_CEC_PHYSICAL : SWITCH_DP_CEC_IMAGE;
                s->phase = SWITCH_DP_CEC_LOGICAL;
            } else {
                s->phase = ++s->candidate < 3 ? SWITCH_DP_CEC_IDLE : SWITCH_DP_CEC_STOP;
            }
        } else {
            if (s->packet != SWITCH_DP_CEC_REPLY)
                s->wake = (data[0] & 0x20u)                     ? SWITCH_DP_CEC_DONE
                          : s->packet == SWITCH_DP_CEC_IMAGE    ? SWITCH_DP_CEC_PHYSICAL
                          : s->packet == SWITCH_DP_CEC_PHYSICAL ? SWITCH_DP_CEC_ACTIVE
                                                                : SWITCH_DP_CEC_DONE;
            s->phase = SWITCH_DP_CEC_IDLE;
        }
        break;
    default:
        break;
    }
}

/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWITCH_WLAN_A3_WIRE_H
#define SWITCH_WLAN_A3_WIRE_H

#include <string.h>
#include "firmware/a3/wire.h"

static inline bool switch_a3_state_valid(const struct switch_a3_reply &state, size_t length)
{
    return state.version == SWITCH_A3_WIRE_VERSION && state.header_bytes == sizeof(state) &&
           state.total_bytes == length && state.clock_epoch && state.firmware_generation && state.state_revision &&
           !state.reserved && state.status <= 0 && state.fault_status <= 0 && state.state <= SWITCH_A3_FAULTED &&
           !(state.legal_social_channels & ~(SWITCH_A3_SOCIAL_6 | SWITCH_A3_SOCIAL_44 | SWITCH_A3_SOCIAL_149)) &&
           (!state.applied_generation || state.applied_generation == state.state_generation);
}

static inline bool switch_a3_tx_decode(const void *data, size_t length, uint32_t session, uint32_t epoch,
                                       struct switch_a3_tx_header *header)
{
    if (!data || !header || length < 14 + sizeof(*header) || !session || !epoch)
        return false;
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    static const uint8_t marker[6] = SWITCH_A3_TX_MARKER_BYTES;
    const uint32_t token = static_cast<uint32_t>(bytes[8]) | (static_cast<uint32_t>(bytes[9]) << 8) |
                           (static_cast<uint32_t>(bytes[10]) << 16) | (static_cast<uint32_t>(bytes[11]) << 24);
    memcpy(header, bytes + 14, sizeof(*header));
    if (memcmp(bytes, marker, sizeof marker) || bytes[6] != 0x02 || bytes[7] != 0x57 || bytes[12] != 0x88 ||
        bytes[13] != 0xb5 || header->magic != SWITCH_A3_TX_MAGIC || header->version != SWITCH_A3_WIRE_VERSION ||
        header->header_bytes != sizeof(*header) || header->total_bytes != length - 14 ||
        header->frame_bytes != length - 14 - sizeof(*header) || header->frame_bytes < 24 ||
        header->frame_bytes > SWITCH_A3_MAX_FRAME || !header->request_id || token != header->request_id ||
        !header->schedule_generation || header->session != session || header->clock_epoch != epoch ||
        header->firmware_generation != 1 || header->reserved ||
        (header->flags & ~static_cast<uint32_t>(SWITCH_A3_TX_NO_ACK | SWITCH_A3_TX_ON_CHANNEL_ONLY)) ||
        (header->expiry_tsf && header->earliest_tsf > header->expiry_tsf))
        return false;
    const uint8_t fc = bytes[14 + sizeof(*header)];
    return !(fc & 3) && ((fc & 0x0c) == 0 || (fc & 0x0c) == 8);
}

static inline bool switch_a3_reply_decode(const void *data, size_t length, const struct switch_a3_control *request,
                                          struct switch_a3_reply *reply)
{
    if (!data || !request || !reply || length != sizeof(*reply))
        return false;
    memcpy(reply, data, sizeof(*reply));
    return reply->magic == SWITCH_A3_REPLY_MAGIC && switch_a3_state_valid(*reply, length) &&
           reply->request_id == request->request_id &&
           (!request->session || reply->session == request->session) &&
           (!request->clock_epoch || reply->clock_epoch == request->clock_epoch ||
            reply->status == SWITCH_A3_CLOCK_STALE);
}

static inline bool switch_a3_rx_decode(const void *data, size_t length, uint32_t session, uint32_t epoch,
                                       uint32_t applied, struct switch_a3_raw_rx *header, size_t *frame_length)
{
    if (!data || !header || !frame_length || length < sizeof(*header) || !session || !epoch || !applied)
        return false;
    memcpy(header, data, sizeof(*header));
    if (header->magic != SWITCH_A3_RAW_RX_MAGIC || header->version != SWITCH_A3_RAW_RX_VERSION ||
        header->header_bytes != sizeof(*header) || header->frame_bytes != length - sizeof(*header) ||
        header->frame_bytes > SWITCH_A3_MAX_FRAME || !header->tsf || header->reserved || header->reserved2 ||
        (header->flags & ~SWITCH_A3_RX_FCS_PRESENT) || header->session != session || header->clock_epoch != epoch ||
        header->config_generation != applied)
        return false;
    size_t n = header->frame_bytes;
    if (header->flags & SWITCH_A3_RX_FCS_PRESENT) {
        if (n < 4)
            return false;
        n -= 4;
    }
    if (n < 24)
        return false;
    *frame_length = n;
    return true;
}

static inline bool switch_a3_tx_status_decode(const void *data, size_t length, uint32_t session, uint32_t epoch,
                                              uint32_t schedule, struct switch_a3_tx_status *status)
{
    if (!data || !status || length != sizeof(*status) || !session || !epoch || !schedule)
        return false;
    memcpy(status, data, sizeof(*status));
    return status->magic == SWITCH_A3_TX_STATUS_MAGIC && status->version == SWITCH_A3_WIRE_VERSION &&
           status->header_bytes == sizeof(*status) && status->total_bytes == length && status->session == session &&
           status->clock_epoch == epoch && status->schedule_generation == schedule && status->request_id &&
           status->status >= SWITCH_A3_TX_SENT && status->status <= SWITCH_A3_TX_FAILED &&
           !(status->flags &
             ~static_cast<uint32_t>(SWITCH_A3_TX_ACK_VALID | SWITCH_A3_TX_ACKED | SWITCH_A3_TX_TSF_VALID)) &&
           (!(status->flags & SWITCH_A3_TX_ACKED) || (status->flags & SWITCH_A3_TX_ACK_VALID));
}

#endif

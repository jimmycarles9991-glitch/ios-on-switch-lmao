/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "rx.h"
#include "rx_status.h"
#include "clock.h"
#if defined(SWITCH_A3_CANDIDATE) && SWITCH_A3_CANDIDATE
#include "beacon.h"
#include "scheduler.h"
#include "diagnostic.h"
#else
#define SWITCH_A3_DIAG_COUNT(name) ((void)0)
#define SWITCH_A3_DIAG(statement) ((void)0)
#endif

struct switch_a3_packet {
    uint32_t private_words[2];
    uint8_t *data;
    uint16_t length, flags;
    uint32_t private_word;
    uint16_t next, link;
};

struct switch_a3_fetch {
    struct switch_a3_packet *original;
    uint16_t headroom;
    int16_t host_offset;
    void (*callback)(struct switch_a3_packet *, struct switch_a3_packet *, void *, int);
    void *context;
    void *next;
};

_Static_assert(offsetof(struct switch_a3_packet, data) == 8, "A3 packet data");
_Static_assert(offsetof(struct switch_a3_packet, next) == 20, "A3 packet chain");
_Static_assert(sizeof(struct switch_a3_fetch) == 20, "A3 fetch request");

extern struct switch_a3_packet *switch_a3_packet_get(void *osh, uint32_t length);
extern void switch_a3_packet_free(void *osh, struct switch_a3_packet *packet, int send);
extern void *switch_a3_malloc_zero(uint32_t length);
extern void switch_a3_free(void *allocation);
extern int switch_a3_packet_fetch(struct switch_a3_fetch *request);
extern void switch_a3_raw_sendup(void *wl, void *interface, struct switch_a3_packet *packet);
extern void switch_a3_tsf_receive(void *wlc, const void *status, uint32_t *high, uint32_t *low);

static struct {
    uint8_t enabled;
    uint8_t address[6];
    void *wlc;
    uint32_t generation, session, clock_epoch, config_generation, pending;
} switch_a3_rx_state;

struct switch_a3_rx_pending {
    struct switch_a3_fetch fetch;
    struct switch_a3_raw_rx header;
    void *wlc;
    uint32_t generation;
    const uint8_t *prefix;
    uint16_t prefix_bytes, body_bytes;
};

__attribute__((noinline)) static uint16_t switch_a3_read16(const void *address)
{
    const uint8_t *bytes = address;
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static void *switch_a3_pointer(const void *base, size_t offset)
{
    return *(void *const *)((const uint8_t *)base + offset);
}

static __attribute__((noinline, noclone)) int switch_a3_rx_timestamp(void *wlc, const void *status, uint64_t *tsf)
{
    if (switch_a3_clock_read(wlc, tsf))
        return 0;
    uint32_t low = (uint32_t)*tsf, high = (uint32_t)(*tsf >> 32);
    switch_a3_tsf_receive(wlc, status, &high, &low);
    *tsf = ((uint64_t)high << 32) | low;
    return 1;
}

static __attribute__((noinline)) void switch_a3_copy(void *destination, const void *source, size_t length)
{
    uint8_t *out = destination;
    const uint8_t *in = source;
    for (size_t index = 0; index < length; ++index)
        out[index] = in[index];
}

static int switch_a3_equal(const uint8_t *left, const uint8_t *right)
{
    for (size_t index = 0; index < 6; ++index)
        if (left[index] != right[index])
            return 0;
    return 1;
}

int switch_a3_rx_configure(void *wlc, const uint8_t address[6], int enabled, uint32_t session, uint32_t clock_epoch,
                           uint32_t config_generation)
{
    if (enabled) {
        if (!wlc || !address || (address[0] & 1u) || !session || !clock_epoch || !config_generation)
            return -2;
        uint8_t nonzero = 0;
        for (size_t index = 0; index < 6; ++index)
            nonzero |= address[index];
        if (!nonzero)
            return -2;
    }
    switch_a3_rx_state.enabled = 0;
    ++switch_a3_rx_state.generation;
    switch_a3_rx_state.wlc = wlc;
    if (enabled) {
        switch_a3_copy(switch_a3_rx_state.address, address, 6);
        switch_a3_rx_state.session = session;
        switch_a3_rx_state.clock_epoch = clock_epoch;
        switch_a3_rx_state.config_generation = config_generation;
        switch_a3_rx_state.enabled = 1;
    }
    return 0;
}

uint32_t switch_a3_rx_pending(void)
{
    return switch_a3_rx_state.pending;
}

static void switch_a3_rx_deliver(void *wlc, struct switch_a3_packet *packet)
{
    packet->flags |= 0x80u;
    switch_a3_raw_sendup(switch_a3_pointer(wlc, 8), 0, packet);
}

__attribute__((noinline, noclone)) int switch_a3_rx_event(void *wlc, const void *data, uint32_t length)
{
    if (!wlc || !data || !length || length > SWITCH_A3_RX_BUFFER_BYTES - SWITCH_A3_RX_BUS_HEADROOM)
        return -2;
    struct switch_a3_packet *packet =
        switch_a3_packet_get(switch_a3_pointer(wlc, 4), SWITCH_A3_RX_BUS_HEADROOM + length);
    if (!packet)
        return -27;
    packet->data += SWITCH_A3_RX_BUS_HEADROOM;
    packet->length = length;
    switch_a3_copy(packet->data, data, length);
    switch_a3_rx_deliver(wlc, packet);
    return 0;
}

static void switch_a3_rx_fetched(struct switch_a3_packet *packet, struct switch_a3_packet *original, void *context,
                                 int cancelled)
{
    struct switch_a3_rx_pending *pending = context;
    void *wlc = pending->wlc;
    --switch_a3_rx_state.pending;
    void *osh = switch_a3_pointer(wlc, 4);
    const int valid = !cancelled && packet && original == pending->fetch.original && switch_a3_rx_state.enabled &&
                      switch_a3_rx_state.wlc == wlc && switch_a3_rx_state.generation == pending->generation &&
                      packet->length == pending->body_bytes;
    if (valid) {
        const uint16_t prepend = sizeof pending->header + pending->prefix_bytes;
        packet->data -= prepend;
        packet->length += prepend;
        switch_a3_copy(packet->data, &pending->header, sizeof pending->header);
        switch_a3_copy(packet->data + sizeof pending->header, pending->prefix, pending->prefix_bytes);
        SWITCH_A3_DIAG_COUNT(RX_EXPORT);
    } else {
        SWITCH_A3_DIAG(
            if (pending->generation == switch_a3_rx_state.generation)
                SWITCH_A3_DIAG_INDEX(SWITCH_A3_DIAG_RX_FETCH_FAILED));
    }
    if (original)
        switch_a3_packet_free(osh, original, 0);
    switch_a3_free(pending);
    if (valid)
        switch_a3_rx_deliver(wlc, packet);
    else if (packet)
        switch_a3_packet_free(osh, packet, 0);
}

int switch_a3_rx_consume(void *wlc, void *opaque_packet)
{
    static const uint8_t switch_bssid[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};
    if (!switch_a3_rx_state.enabled || wlc != switch_a3_rx_state.wlc || !opaque_packet)
        return 0;
    SWITCH_A3_DIAG_COUNT(RX_HOOK);
    struct switch_a3_packet *original = opaque_packet;
    if (original->next || original->link || !original->data)
        return 0;
    const uint32_t status_bytes = *(const uint32_t *)((const uint8_t *)wlc + 0x5f0);
    if (status_bytes < 32 || status_bytes > original->length)
        return 0;
    const uint8_t *status = original->data;
    const uint16_t status_flags = switch_a3_read16(status + 16);
    const uint32_t skip = status_bytes + ((status_flags & 4u) ? 2u : 0u) + 6u;
    if (skip + 24u > original->length)
        return 0;
    SWITCH_A3_DIAG_COUNT(RX_HEADER);
    if (!switch_a3_rx_phy_valid(wlc, status))
        return 0;
    SWITCH_A3_DIAG_COUNT(RX_PHY);
    const uint8_t *frame = status + skip;
    const uint16_t fc = switch_a3_read16(frame);
    const uint16_t prefix_bytes = original->length - skip;
#if (DEVELOPMENT || DEBUG) && defined(SWITCH_A3_CANDIDATE) && SWITCH_A3_CANDIDATE
    static const uint8_t vendor[6] = {0x7f, 0x00, 0x17, 0xf2, 0x08, 0x10};
    SWITCH_A3_DIAG(
        if ((fc & 0xffu) == 0xd0u) {
            SWITCH_A3_DIAG_COUNT(RX_ACTION);
            if (prefix_bytes >= 31 && switch_a3_equal(frame + 24, vendor)) {
                if (frame[30] == 0)
                    SWITCH_A3_DIAG_COUNT(RX_PSF);
                else if (frame[30] == 3)
                    SWITCH_A3_DIAG_COUNT(RX_MIF);
            }
        });
#endif
#if defined(SWITCH_A3_CANDIDATE) && SWITCH_A3_CANDIDATE
    if ((fc & 0x43ffu) == 0x80u) {
        uint64_t tsf;
        if (switch_a3_rx_timestamp(wlc, status, &tsf) &&
            switch_a3_beacon_observe(wlc, status, status_bytes, frame, prefix_bytes, tsf))
            switch_a3_scheduler_observed(wlc);
        return 0;
    }
#endif
    if ((fc & 3u) || (fc & 0x4300u) || ((fc & 0xfcu) != 0xd0u && (fc & 0x0cu) != 8u) ||
        !switch_a3_equal(frame + 16, switch_bssid))
        return 0;
    SWITCH_A3_DIAG_COUNT(RX_BSSID);
    if (!(frame[4] & 1u) && !switch_a3_equal(frame + 4, switch_a3_rx_state.address))
        return 0;
    SWITCH_A3_DIAG_COUNT(RX_DEST);
    uint16_t body_bytes = 0;
    if (original->flags & 0x2000u) {
        body_bytes = switch_a3_read16((const uint8_t *)original + 80);
        if (switch_a3_read16((const uint8_t *)original + 84))
            return 0;
    }
    const uint32_t frame_bytes = (uint32_t)prefix_bytes + body_bytes;
    const uint32_t headroom = SWITCH_A3_RX_BUS_HEADROOM + sizeof(struct switch_a3_raw_rx) + prefix_bytes;
    if (frame_bytes - 28u > SWITCH_A3_RX_BUFFER_BYTES - SWITCH_A3_RX_BUS_HEADROOM - sizeof(struct switch_a3_raw_rx) - 28u)
        return 0;
    struct switch_a3_raw_rx header;
    header.magic = SWITCH_A3_RX_MAGIC;
    header.version = SWITCH_A3_RX_VERSION;
    header.header_bytes = sizeof header;
    header.frame_bytes = frame_bytes;
    header.chanspec = switch_a3_read16(status + 22);
    header.rssi = (int8_t)status[28];
    header.flags = SWITCH_A3_RX_FCS_PRESENT;
    header.reserved = 0;
    header.session = switch_a3_rx_state.session;
    header.clock_epoch = switch_a3_rx_state.clock_epoch;
    header.config_generation = switch_a3_rx_state.config_generation;
    header.reserved2 = 0;
    if (!switch_a3_rx_timestamp(wlc, status, &header.tsf))
        return 0;
    SWITCH_A3_DIAG_COUNT(RX_MATCH);
    void *osh = switch_a3_pointer(wlc, 4);
    if (body_bytes) {
        SWITCH_A3_DIAG_COUNT(RX_SPLIT);
#if defined(SWITCH_A3_CANDIDATE) && SWITCH_A3_CANDIDATE
        if (switch_a3_rx_state.pending >= 3) {
            SWITCH_A3_DIAG_COUNT(RX_DROP);
            switch_a3_packet_free(osh, original, 0);
            return 1;
        }
#endif
        struct switch_a3_rx_pending *pending = switch_a3_malloc_zero(sizeof *pending);
        if (pending) {
            pending->fetch.original = original;
            pending->fetch.headroom = headroom;
            pending->fetch.callback = switch_a3_rx_fetched;
            pending->fetch.context = pending;
            switch_a3_copy(&pending->header, &header, sizeof header);
            pending->wlc = wlc;
            pending->generation = switch_a3_rx_state.generation;
            pending->prefix = frame;
            pending->prefix_bytes = prefix_bytes;
            pending->body_bytes = body_bytes;
            ++switch_a3_rx_state.pending;
            if (!switch_a3_packet_fetch(&pending->fetch))
                return 1;
            SWITCH_A3_DIAG_COUNT(RX_FETCH_FAILED);
            --switch_a3_rx_state.pending;
            switch_a3_free(pending);
        }
    } else {
        struct switch_a3_packet *packet = switch_a3_packet_get(osh, headroom);
        if (packet) {
            packet->data += SWITCH_A3_RX_BUS_HEADROOM;
            packet->length = sizeof header + prefix_bytes;
            switch_a3_copy(packet->data, &header, sizeof header);
            switch_a3_copy(packet->data + sizeof header, frame, prefix_bytes);
            switch_a3_packet_free(osh, original, 0);
            SWITCH_A3_DIAG_COUNT(RX_EXPORT);
            switch_a3_rx_deliver(wlc, packet);
            return 1;
        }
    }
    SWITCH_A3_DIAG_COUNT(RX_DROP);
    switch_a3_packet_free(osh, original, 0);
    return 1;
}

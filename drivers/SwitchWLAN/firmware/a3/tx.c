/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "tx.h"
#include "clock.h"
#include "tx_timing.h"
#include "timer.h"
#include "scheduler.h"
#include "control.h"
#include "diagnostic.h"
#include "rx.h"

#define SWITCH_A3_TX_HEADROOM 202u
#define SWITCH_A3_TX_CREDITS  8u

extern struct switch_a3_tx_packet **switch_a3_tx_packets;
extern void *switch_a3_tx_allocate(uint32_t bytes);
extern void switch_a3_tx_free(void *allocation);
extern struct switch_a3_tx_packet *switch_a3_tx_packet_get(void *osh, uint32_t bytes);
extern void switch_a3_tx_packet_free(void *osh, struct switch_a3_tx_packet *packet, int send);
extern int switch_a3_tx_fetch(struct switch_a3_tx_fetch *request);
extern int switch_a3_tx_register(void *pcb, void (*callback)(void *, uint32_t, void *), void *arg,
                                 struct switch_a3_tx_packet *packet);
extern int switch_a3_tx_unregister(void *pcb, void (*callback)(void *, uint32_t, void *), void *arg, int cancel);
extern int switch_a3_tx_sendctl(void *wlc, struct switch_a3_tx_packet *packet, void *queue, void *scb, void *bsscfg,
                                int short_preamble, void *key, uint32_t rate);
extern void switch_a3_tx_sendup(void *wl, void *interface, struct switch_a3_tx_packet *packet);
extern uint16_t switch_a3_tx_phy_chanspec(void *pi);
extern int switch_a3_tx_original(void *source, void *destination, struct switch_a3_tx_packet *packets);
extern void switch_a3_tx_status_original(void *pcb, struct switch_a3_tx_packet *packet, uint32_t status);

struct switch_a3_tx_pending {
    struct switch_a3_tx_fetch fetch;
    struct switch_a3_tx_header header;
    struct switch_a3_tx_pending *next;
    const uint8_t *prefix;
    void *wlc;
    struct switch_a3_tx_packet *radio_packet;
    uint16_t prefix_bytes, body_bytes;
    uint8_t cancelled, submitting, completed, radio, group, radio_status, waiting;
    uint32_t compact_status, hardware_status;
};

static struct {
    uint8_t enabled;
    void *wlc;
    struct switch_a3_tx_pending *pending;

    union {
        struct switch_a3_owner owner;

        struct {
            uint32_t session, epoch, schedule;
        };
    };

    uint32_t firmware, count;
    struct switch_a3_configuration configuration;
    struct switch_a3_ring_binding rings;
    struct switch_a3_timer timer;
} switch_a3_tx_state;

static void *switch_a3_tx_pointer(const void *base, size_t offset)
{
    return *(void *const *)((const uint8_t *)base + offset);
}

__attribute__((noinline)) static uint16_t switch_a3_tx_read16(const void *address)
{
    const uint8_t *bytes = address;
    return bytes[0] | ((uint16_t)bytes[1] << 8);
}

static __attribute__((noinline)) void switch_a3_tx_copy(void *destination, const void *source, size_t bytes)
{
    uint8_t *out = destination;
    const uint8_t *in = source;
    for (size_t index = 0; index < bytes; ++index)
        out[index] = in[index];
}

static int switch_a3_tx_equal(const uint8_t *left, const uint8_t *right)
{
    for (size_t index = 0; index < 6; ++index)
        if (left[index] != right[index])
            return 0;
    return 1;
}

static void switch_a3_tx_reply(void *wlc, const struct switch_a3_tx_header *header, int result, uint32_t flags)
{
    if (header->request_id) {
        struct switch_a3_tx_status reply = {
            .magic = SWITCH_A3_TX_STATUS_MAGIC,
            .version = SWITCH_A3_WIRE_VERSION,
            .header_bytes = sizeof reply,
            .total_bytes = sizeof reply,
            .session = header->session,
            .clock_epoch = header->clock_epoch,
            .schedule_generation = header->schedule_generation,
            .request_id = header->request_id,
            .status = result,
            .flags = flags,
            .credits = switch_a3_tx_credits(),
        };
        if (switch_a3_rx_event(wlc, &reply, sizeof reply))
            SWITCH_A3_DIAG_COUNT(TX_STATUS_EXPORT_DROP);
    }
}

static void switch_a3_tx_finish(struct switch_a3_tx_pending *pending, int result, uint32_t flags)
{
    struct switch_a3_tx_pending **link = &switch_a3_tx_state.pending;
    while (*link && *link != pending)
        link = &(*link)->next;
    if (*link) {
        *link = pending->next;
        --switch_a3_tx_state.count;
    }
    switch_a3_tx_packet_free(switch_a3_tx_pointer(pending->wlc, 4), pending->fetch.original, 1);
    switch_a3_tx_reply(pending->wlc, &pending->header, result, flags);
    switch_a3_tx_free(pending);
    switch_a3_control_retired();
}

static __attribute__((noinline, noclone)) void switch_a3_tx_discard(struct switch_a3_tx_pending *pending, int result,
                                                                 void *osh)
{
    switch_a3_tx_packet_free(osh, pending->radio_packet, 1);
    switch_a3_tx_finish(pending, result, 0);
}

static void switch_a3_tx_completed(void *wlc, uint32_t status, void *argument)
{
    struct switch_a3_tx_pending *pending = argument;
    if (wlc != pending->wlc)
        pending->cancelled = 1;
    pending->completed = 1;
    pending->compact_status = status;
    if (!pending->submitting) {
        const int acked = !pending->cancelled && (status & 2u);
        const int group_sent = pending->group && pending->radio_status && !(pending->hardware_status & 0x7cu);
        const int suppressed = !pending->group && pending->radio_status && (pending->hardware_status & 0x7cu);
        switch_a3_tx_finish(pending,
                            pending->cancelled    ? SWITCH_A3_TX_CANCELLED
                            : acked || group_sent ? SWITCH_A3_TX_SENT
                            : suppressed          ? SWITCH_A3_TX_EXPIRED
                                                  : SWITCH_A3_TX_FAILED,
                            acked ? SWITCH_A3_TX_ACK_VALID | SWITCH_A3_TX_ACKED : 0);
    }
}

void switch_a3_tx_radio_status(void *pcb, void *packet, uint32_t status)
{
    for (struct switch_a3_tx_pending *pending = switch_a3_tx_state.pending; pending; pending = pending->next)
        if (packet == pending->radio_packet && pcb == switch_a3_tx_pointer(pending->wlc, 0x498)) {
            SWITCH_A3_DIAG_COUNT(TX_STATUS);
            pending->radio_status = 1;
            pending->hardware_status = status;
            break;
        }
    switch_a3_tx_status_original(pcb, packet, status);
}

int switch_a3_tx_cancel(void *wlc)
{
    if (wlc != switch_a3_tx_state.wlc)
        return 0;
    switch_a3_tx_state.enabled = 0;
    const int timer_status = switch_a3_timer_cancel_owned(&switch_a3_tx_state.timer, wlc);
    /* Radio-down teardown can suppress PCB callbacks; retained requests then require a chip reset. */
    for (struct switch_a3_tx_pending *pending = switch_a3_tx_state.pending, *next; pending; pending = next) {
        next = pending->next;
        pending->cancelled = 1;
        if (pending->waiting) {
            switch_a3_tx_discard(pending, SWITCH_A3_TX_CANCELLED, switch_a3_tx_pointer(wlc, 4));
        }
    }
    return timer_status;
}

uint32_t switch_a3_tx_pending(void)
{
    return switch_a3_tx_state.count;
}

uint32_t switch_a3_tx_credits(void)
{
    return switch_a3_tx_state.enabled && switch_a3_tx_state.count < switch_a3_tx_state.rings.credits
               ? switch_a3_tx_state.rings.credits - switch_a3_tx_state.count
               : 0;
}

int switch_a3_tx_binding_valid(const struct switch_a3_ring_binding *rings)
{
    return rings && rings->ring_count && rings->first_ring >= 2 &&
           (uint32_t)rings->first_ring + rings->ring_count <= 65536u && rings->max_frame >= 24 &&
           rings->max_frame <= SWITCH_A3_MAX_FRAME && rings->credits && rings->credits <= SWITCH_A3_TX_CREDITS &&
           !rings->reserved && !rings->reserved2;
}

int switch_a3_tx_configure(void *wlc, const struct switch_a3_configuration *configuration,
                           const struct switch_a3_ring_binding *rings, uint32_t session, uint32_t clock_epoch,
                           uint32_t schedule_generation, uint32_t firmware_generation)
{
    if (!wlc || !configuration || !switch_a3_tx_binding_valid(rings) || !session || !clock_epoch ||
        !schedule_generation || !firmware_generation || !configuration->chanspec || (configuration->address[0] & 1u) ||
        (configuration->bssid[0] & 1u))
        return -2;
    uint8_t address_nonzero = 0, bssid_nonzero = 0;
    for (size_t index = 0; index < 6; ++index) {
        address_nonzero |= configuration->address[index];
        bssid_nonzero |= configuration->bssid[index];
    }
    if (!address_nonzero || !bssid_nonzero)
        return -2;
    if (switch_a3_tx_state.count)
        return -16;
    const int timer_status = switch_a3_timer_initialize_owned(&switch_a3_tx_state.timer, wlc);
    if (timer_status)
        return timer_status;
    switch_a3_tx_state.wlc = wlc;
    switch_a3_tx_copy(&switch_a3_tx_state.configuration, configuration, sizeof *configuration);
    switch_a3_tx_copy(&switch_a3_tx_state.rings, rings, sizeof *rings);
    switch_a3_tx_state.session = session;
    switch_a3_tx_state.epoch = clock_epoch;
    switch_a3_tx_state.schedule = schedule_generation;
    switch_a3_tx_state.firmware = firmware_generation;
    switch_a3_tx_state.enabled = 1;
    return 0;
}

static int switch_a3_tx_rate_valid(uint32_t rate)
{
    /* Bitmask of (rate >> 2) for valid OFDM rates: 0, 12, 24, 48, 72, 108 (500 kbps units). */
    return !(rate & 3u) && rate <= 108u && ((0x08041049u >> (rate >> 2)) & 1u);
}

static int switch_a3_tx_validate(struct switch_a3_tx_pending *pending, const uint8_t *bytes, uint32_t length)
{
    if (length < 14 + sizeof pending->header)
        return SWITCH_A3_TX_FAILED;
    struct switch_a3_tx_header decoded;
    switch_a3_tx_copy(&decoded, bytes + 14, sizeof decoded);
    const struct switch_a3_tx_header *header = &decoded;
    if (header->magic != SWITCH_A3_TX_MAGIC ||
        (header->version | ((uint32_t)header->header_bytes << 16)) !=
            (SWITCH_A3_WIRE_VERSION | ((uint32_t)sizeof *header << 16)) || header->total_bytes != length - 14 ||
        header->frame_bytes != length - 14 - sizeof *header || header->frame_bytes < 24 ||
        header->frame_bytes > switch_a3_tx_state.rings.max_frame || header->request_id != pending->header.request_id ||
        header->reserved || header->session != switch_a3_tx_state.session ||
        header->clock_epoch != switch_a3_tx_state.epoch || header->schedule_generation != switch_a3_tx_state.schedule ||
        header->firmware_generation != switch_a3_tx_state.firmware || !header->expiry_tsf ||
        header->earliest_tsf > header->expiry_tsf ||
        (header->flags & ~SWITCH_A3_TX_NO_ACK) != SWITCH_A3_TX_ON_CHANNEL_ONLY ||
        !switch_a3_tx_rate_valid(header->rate))
        return SWITCH_A3_TX_FAILED;
    const uint8_t *frame = bytes + 14 + sizeof *header;
    const uint16_t fc = switch_a3_tx_read16(frame);
    if ((fc & 0xc703u) || (switch_a3_tx_read16(frame + 22) & 15u) ||
        ((fc & 0xfcu) != 0xd0u && (fc & 0x7cu) != 8u) ||
        !switch_a3_tx_equal(frame + 10, switch_a3_tx_state.configuration.address) ||
        !switch_a3_tx_equal(frame + 16, switch_a3_tx_state.configuration.bssid) ||
        (!(frame[4] & 1u) && (header->flags & SWITCH_A3_TX_NO_ACK)) ||
        ((fc & 0xfcu) == 0x88u && (header->frame_bytes < 26 || (!(frame[4] & 1u) && (frame[24] & 0x60u)))))
        return SWITCH_A3_TX_FAILED;
    switch_a3_tx_copy(&pending->header, header, sizeof *header);
    return SWITCH_A3_TX_SENT;
}

static __attribute__((noinline, noclone)) void switch_a3_tx_air(struct switch_a3_tx_pending *pending)
{
    void *wlc = pending->wlc;
    void *osh = switch_a3_tx_pointer(wlc, 4);
    void *band = switch_a3_tx_pointer(wlc, 32);
    void *scb = band ? switch_a3_tx_pointer(band, 24) : NULL;
    void *bsscfg = scb ? switch_a3_tx_pointer(scb, 16) : NULL;
    void *pcb = switch_a3_tx_pointer(wlc, 0x498);
    void *queue = switch_a3_tx_pointer(wlc, 0x5cc);
    struct switch_a3_tx_packet *packet = pending->radio_packet;
    if (!scb || !bsscfg || !pcb || !queue) {
        switch_a3_tx_discard(pending, SWITCH_A3_TX_FAILED, osh);
        return;
    }
    if (switch_a3_tx_register(pcb, switch_a3_tx_completed, pending, packet)) {
        switch_a3_tx_discard(pending, SWITCH_A3_TX_FAILED, osh);
        return;
    }
    pending->radio = pending->submitting = 1;
    uint64_t now;
    uint32_t lifetime;
    const int cancelled = pending->cancelled || !switch_a3_tx_state.enabled;
    uint64_t until;
    int timing =
        cancelled || switch_a3_clock_read(wlc, &now) || pending->header.earliest_tsf > now ||
                switch_a3_scheduler_admit(wlc, pending->header.chanspec, &until)
            ? -1
            : switch_a3_tx_timing(packet->data, packet->length, now,
                                  until < pending->header.expiry_tsf ? until : pending->header.expiry_tsf, &lifetime);
    if (timing < 0) {
        switch_a3_tx_unregister(pcb, switch_a3_tx_completed, pending, 1);
        switch_a3_tx_discard(pending,
                            cancelled      ? SWITCH_A3_TX_CANCELLED
                            : timing == -2 ? SWITCH_A3_TX_EXPIRED
                                           : SWITCH_A3_TX_FAILED,
                            osh);
        return;
    }
    *(uint32_t *)((uint8_t *)packet + 24) |= 0x200u;
    *(uint16_t *)((uint8_t *)packet + 28) = 0x1000u | (switch_a3_tx_read16(packet->data + 22) >> 4);
    *(uint32_t *)((uint8_t *)packet + 36) = lifetime;
    if (timing)
        packet->flags = (packet->flags & ~7u) | 7u;
    SWITCH_A3_DIAG_COUNT(TX_NATIVE);
    const uint32_t rate = pending->header.rate ? pending->header.rate : 12u;
    const int accepted = switch_a3_tx_sendctl(wlc, packet, queue, scb, bsscfg, 0, NULL, rate);
    pending->submitting = 0;
    if (!accepted && !pending->completed) {
        switch_a3_tx_unregister(pcb, switch_a3_tx_completed, pending, 1);
        pending->completed = 1;
        pending->compact_status = 0;
    }
    if (pending->completed)
        switch_a3_tx_completed(wlc, pending->compact_status, pending);
}

static void switch_a3_tx_due(void *wlc, uint64_t requested, uint64_t actual, uint32_t session, uint32_t epoch)
{
    (void)requested;
    (void)actual;
    if (session != switch_a3_tx_state.session || epoch != switch_a3_tx_state.epoch)
        return;
    if (switch_a3_tx_state.timer.snapshot.status)
        switch_a3_control_timer_fault(switch_a3_tx_state.timer.snapshot.status, &switch_a3_tx_state.timer,
                                      &switch_a3_tx_state.owner);
    else
        switch_a3_tx_wake(wlc);
}

void switch_a3_tx_wake(void *wlc)
{
    if (wlc != switch_a3_tx_state.wlc)
        return;
    uint64_t now = 0, next_deadline = UINT64_MAX;
    /* Native submission can synchronously retire any queued packet; rescan the live list. */
    for (unsigned attempt = 0; attempt < SWITCH_A3_TX_CREDITS; ++attempt) {
        const int clock_status = switch_a3_clock_read(wlc, &now);
        struct switch_a3_tx_pending *pending = switch_a3_tx_state.pending;
        int expired = 0;
        for (; pending; pending = pending->next) {
            if (!pending->waiting)
                continue;
            expired = pending->cancelled || clock_status || !*((const uint8_t *)wlc + 25) ||
                      now >= pending->header.expiry_tsf;
            uint64_t until;
            if (expired || (pending->header.earliest_tsf <= now &&
                            !switch_a3_scheduler_admit(wlc, pending->header.chanspec, &until)))
                break;
        }
        if (!pending)
            break;
        if (expired) {
            switch_a3_tx_discard(pending,
                                pending->cancelled                              ? SWITCH_A3_TX_CANCELLED
                                : clock_status || !*((const uint8_t *)wlc + 25) ? SWITCH_A3_TX_FAILED
                                                                                : SWITCH_A3_TX_EXPIRED,
                                switch_a3_tx_pointer(wlc, 4));
        } else {
            pending->waiting = 0;
            switch_a3_tx_air(pending);
        }
    }
    for (const struct switch_a3_tx_pending *pending = switch_a3_tx_state.pending; pending; pending = pending->next)
        if (pending->waiting) {
            const uint64_t deadline =
                pending->header.earliest_tsf > now ? pending->header.earliest_tsf : pending->header.expiry_tsf;
            if (deadline < next_deadline)
                next_deadline = deadline;
        }
    int result;
    if (next_deadline == UINT64_MAX) {
        result = switch_a3_timer_cancel_owned(&switch_a3_tx_state.timer, wlc);
    } else {
        result = switch_a3_timer_arm_owned(&switch_a3_tx_state.timer, wlc, next_deadline, switch_a3_tx_state.session,
                                           switch_a3_tx_state.epoch, switch_a3_tx_due, wlc);
        if (result == SWITCH_A3_TIMER_ELAPSED)
            result = switch_a3_timer_defer_owned(&switch_a3_tx_state.timer, wlc, switch_a3_tx_state.session,
                                                 switch_a3_tx_state.epoch, switch_a3_tx_due, wlc);
    }
    if (result)
        switch_a3_control_timer_fault(result, &switch_a3_tx_state.timer, &switch_a3_tx_state.owner);
}

static void switch_a3_tx_submit(struct switch_a3_tx_pending *pending, const uint8_t *bytes, uint32_t length)
{
    const int validation = switch_a3_tx_validate(pending, bytes, length);
    const int cancelled = pending->cancelled || !switch_a3_tx_state.enabled;
    if (cancelled || validation != SWITCH_A3_TX_SENT) {
        switch_a3_tx_finish(pending, cancelled ? SWITCH_A3_TX_CANCELLED : validation, 0);
        return;
    }
    struct switch_a3_tx_packet *packet = switch_a3_tx_packet_get(switch_a3_tx_pointer(pending->wlc, 4),
                                                                 SWITCH_A3_TX_HEADROOM + pending->header.frame_bytes);
    if (!packet) {
        switch_a3_tx_finish(pending, SWITCH_A3_TX_FAILED, 0);
        return;
    }
    packet->data += SWITCH_A3_TX_HEADROOM;
    packet->length = pending->header.frame_bytes;
    switch_a3_tx_copy(packet->data, bytes + 14 + sizeof pending->header, packet->length);
    pending->radio_packet = packet;
    pending->group = packet->data[4] & 1u;
    pending->waiting = 1;
    switch_a3_tx_wake(pending->wlc);
}

static void switch_a3_tx_fetched(struct switch_a3_tx_packet *packet, struct switch_a3_tx_packet *original,
                                 void *argument, int cancelled)
{
    struct switch_a3_tx_pending *pending = argument;
    void *osh = switch_a3_tx_pointer(pending->wlc, 4);
    if (cancelled || !packet || original != pending->fetch.original || packet->length != pending->body_bytes) {
        if (packet)
            switch_a3_tx_packet_free(osh, packet, 0);
        switch_a3_tx_finish(pending, cancelled || pending->cancelled ? SWITCH_A3_TX_CANCELLED : SWITCH_A3_TX_FAILED, 0);
        return;
    }
    packet->data -= pending->prefix_bytes;
    packet->length += pending->prefix_bytes;
    switch_a3_tx_copy(packet->data, pending->prefix, pending->prefix_bytes);
    switch_a3_tx_submit(pending, packet->data, packet->length);
    switch_a3_tx_packet_free(osh, packet, 0);
}

static int switch_a3_tx_private(struct switch_a3_tx_packet *packet, uint16_t *skip)
{
    static const uint8_t marker[6] = SWITCH_A3_TX_MARKER_BYTES;
    if (!packet->data)
        return 0;
    *skip = *((uint8_t *)packet + 18) * 4u;
    if ((uint32_t)*skip + 14u > packet->length)
        return 0;
    const uint8_t *ethernet = packet->data + *skip;
    return switch_a3_tx_equal(ethernet, marker) && ethernet[12] == 0x88 && ethernet[13] == 0xb5;
}

static int switch_a3_tx_bound(struct switch_a3_tx_packet *packet, void *wlc)
{
    if (!wlc || wlc != switch_a3_tx_state.wlc || !(packet->flags & 0x4000u) || packet->next)
        return 0;
    const uint16_t ring = switch_a3_tx_read16((uint8_t *)packet + 62);
    const uint32_t first = switch_a3_tx_state.rings.first_ring;
    return (uint32_t)ring - first < switch_a3_tx_state.rings.ring_count;
}

static void switch_a3_tx_consume(struct switch_a3_tx_packet *packet, uint16_t skip)
{
    SWITCH_A3_DIAG_COUNT(TX_INGRESS);
    void *wlc = switch_a3_tx_state.wlc;
    const uint8_t *ethernet = packet->data + skip;
    const uint32_t request_id =
        (uint32_t)switch_a3_tx_read16(ethernet + 8) | ((uint32_t)switch_a3_tx_read16(ethernet + 10) << 16);
    if (ethernet[6] != 0x02 || ethernet[7] != 0x57 || !request_id) {
        switch_a3_tx_packet_free(switch_a3_tx_pointer(wlc, 4), packet, 1);
        return;
    }
    struct switch_a3_tx_header identity;
    for (size_t index = 0; index < sizeof identity; ++index)
        ((uint8_t *)&identity)[index] = 0;
    identity.session = switch_a3_tx_state.session;
    identity.clock_epoch = switch_a3_tx_state.epoch;
    identity.schedule_generation = switch_a3_tx_state.schedule;
    identity.request_id = request_id;
    if (!switch_a3_tx_state.enabled) {
        switch_a3_tx_packet_free(switch_a3_tx_pointer(wlc, 4), packet, 1);
        switch_a3_tx_reply(wlc, &identity, SWITCH_A3_TX_CANCELLED, 0);
        return;
    }
    struct switch_a3_tx_pending *pending = NULL;
    if (switch_a3_tx_state.count < switch_a3_tx_state.rings.credits)
        pending = switch_a3_tx_allocate(sizeof *pending);
    if (!pending) {
        switch_a3_tx_packet_free(switch_a3_tx_pointer(wlc, 4), packet, 1);
        switch_a3_tx_reply(wlc, &identity, SWITCH_A3_TX_FAILED, 0);
        return;
    }
    switch_a3_tx_copy(&pending->header, &identity, sizeof identity);
    pending->fetch.original = packet;
    pending->wlc = wlc;
    pending->prefix = packet->data + skip;
    pending->prefix_bytes = packet->length - skip;
    struct switch_a3_tx_pending **tail = &switch_a3_tx_state.pending;
    while (*tail)
        tail = &(*tail)->next;
    pending->next = NULL;
    *tail = pending;
    ++switch_a3_tx_state.count;
    if (packet->flags & 0x200u) {
        pending->body_bytes = switch_a3_tx_read16((uint8_t *)packet + 80);
        if (switch_a3_tx_read16((uint8_t *)packet + 84) ||
            (uint32_t)pending->prefix_bytes + pending->body_bytes > 1920u) {
            switch_a3_tx_finish(pending, SWITCH_A3_TX_FAILED, 0);
            return;
        }
    }
    if (pending->body_bytes) {
        pending->fetch.headroom = pending->prefix_bytes;
        pending->fetch.callback = switch_a3_tx_fetched;
        pending->fetch.context = pending;
        if (!switch_a3_tx_fetch(&pending->fetch))
            return;
        switch_a3_tx_finish(pending, SWITCH_A3_TX_FAILED, 0);
    } else {
        switch_a3_tx_submit(pending, pending->prefix, pending->prefix_bytes);
    }
}

int switch_a3_tx_ingress(void *source_device, void *destination_device, void *opaque_packets)
{
    void *wl = destination_device ? switch_a3_tx_pointer(destination_device, 24) : NULL;
    void *wlc = wl ? switch_a3_tx_pointer(wl, 8) : NULL;
    void *osh = wlc ? switch_a3_tx_pointer(wlc, 4) : NULL;
    if (!osh)
        return 1;
    struct switch_a3_tx_packet *packet = opaque_packets, *head = NULL, *tail = NULL;
    while (packet) {
        struct switch_a3_tx_packet *next = packet->link ? switch_a3_tx_packets[packet->link] : NULL;
        uint16_t skip;
        if (switch_a3_tx_private(packet, &skip)) {
            packet->link = 0;
            if (switch_a3_tx_bound(packet, wlc))
                switch_a3_tx_consume(packet, skip);
            else
                switch_a3_tx_packet_free(osh, packet, 1);
        } else {
            if (tail)
                tail->link = packet->id;
            else
                head = packet;
            tail = packet;
        }
        packet = next;
    }
    if (tail) {
        tail->link = 0;
        return switch_a3_tx_original(source_device, destination_device, head);
    }
    return 0;
}

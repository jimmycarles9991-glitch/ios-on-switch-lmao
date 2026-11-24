/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "tx.h"
#include "diagnostic.h"
#include "timer.h"

union switch_test_memory {
    uint64_t alignment;
    uint8_t bytes[2304];
};
static union switch_test_memory switch_wlc, switch_wl, switch_device, switch_band, switch_scb, switch_pi;
static union switch_test_memory switch_other_wlc;
static union switch_test_memory switch_input[6], switch_buffers[6], switch_heap[6], switch_fetched;

static struct {
    struct switch_a3_tx_packet packet;
    uint8_t extension[72];
} switch_original_storage[6], switch_radio_storage[6];
static struct switch_a3_tx_packet *switch_original[6], *switch_radio[6], switch_status_packet, switch_fetch_packet;
static struct switch_a3_tx_packet *switch_table[16];
struct switch_a3_tx_packet **switch_a3_tx_packets = switch_table;
static struct switch_a3_tx_fetch *switch_fetch_pending;

static struct {
    void (*fn)(void *, uint32_t, void *);
    void *arg;
} switch_callbacks[6];
static struct switch_a3_tx_status switch_reply;
static struct switch_a3_configuration switch_configuration;
static struct switch_a3_ring_binding switch_rings;
static uint8_t switch_envelope[1920];
static uint32_t switch_envelope_bytes, switch_prefix, switch_allocated, switch_freed, switch_radio_count;
static uint32_t switch_original_freed, switch_radio_freed, switch_fetch_freed, switch_statuses, switch_sends;
static uint32_t switch_normal_ids[6], switch_normal_count, switch_fault, switch_cancelled_callbacks;
static uint32_t switch_original_frees[6], switch_send_order[6];
static void *switch_expected_osh;
static int switch_oom, switch_fetch_failure, switch_register_failure, switch_send_failure, switch_immediate;
static int switch_fetch_immediate, switch_clock_error, switch_expire_on_register;
static int switch_elapsed_on_arm, switch_timer_fault;
static int switch_complete_other;
static uint64_t switch_complete_other_tsf;
static uint64_t switch_clock_tsf;
static uint32_t switch_expected_rate;
static uint16_t switch_chanspec;
static struct switch_a3_timer *switch_tx_timer;
volatile uint32_t switch_tx_test_results[86];
volatile uint32_t switch_tx_test_done;
static uintptr_t switch_pm_arguments[6], switch_pm_fallback[5];

#define SWITCH_REQUIRE(condition) \
    do {                          \
        if (!(condition))         \
            return __LINE__;      \
    } while (0)

__attribute__((used)) void *memset(void *output, int value, size_t bytes)
{
    uint8_t *out = output;
    for (size_t index = 0; index < bytes; ++index)
        out[index] = value;
    return output;
}

static void switch_zero(void *output, size_t bytes)
{
    uint8_t *out = output;
    for (size_t index = 0; index < bytes; ++index)
        out[index] = 0;
}

static void switch_copy(void *output, const void *input, size_t bytes)
{
    uint8_t *out = output;
    const uint8_t *in = input;
    for (size_t index = 0; index < bytes; ++index)
        out[index] = in[index];
}

static void switch_put16(void *address, uint16_t value)
{
    uint8_t *bytes = address;
    bytes[0] = value;
    bytes[1] = value >> 8;
}

static struct switch_a3_tx_header *switch_header(void)
{
    return (struct switch_a3_tx_header *)(switch_envelope + 14);
}

static void switch_reset_input(uint16_t frame_bytes, int split, unsigned metadata)
{
    static const uint8_t marker[6] = SWITCH_A3_TX_MARKER_BYTES;
    switch_a3_tx_cancel(switch_wlc.bytes);
    switch_zero(&switch_wlc, sizeof switch_wlc);
    switch_zero(&switch_wl, sizeof switch_wl);
    switch_zero(switch_original_storage, sizeof switch_original_storage);
    switch_zero(switch_radio_storage, sizeof switch_radio_storage);
    for (unsigned index = 0; index < 6; ++index) {
        switch_original[index] = &switch_original_storage[index].packet;
        switch_radio[index] = &switch_radio_storage[index].packet;
    }
    switch_zero(switch_callbacks, sizeof switch_callbacks);
    switch_zero(switch_original_frees, sizeof switch_original_frees);
    switch_zero(switch_send_order, sizeof switch_send_order);
    switch_zero(switch_table, sizeof switch_table);
    switch_zero(switch_envelope, sizeof switch_envelope);
    *(void **)(switch_wlc.bytes + 4) = switch_wlc.bytes + 64;
    switch_expected_osh = switch_wlc.bytes + 64;
    *(void **)(switch_wlc.bytes + 8) = switch_wl.bytes;
    *(void **)(switch_wlc.bytes + 32) = switch_band.bytes;
    *(void **)(switch_wlc.bytes + 0x498) = switch_wlc.bytes + 128;
    *(void **)(switch_wlc.bytes + 0x5cc) = switch_wlc.bytes + 160;
    *(void **)(switch_wl.bytes + 8) = switch_wlc.bytes;
    *(void **)(switch_device.bytes + 24) = switch_wl.bytes;
    *(void **)(switch_band.bytes + 16) = switch_pi.bytes;
    *(void **)(switch_band.bytes + 24) = switch_scb.bytes;
    *(void **)(switch_scb.bytes + 16) = switch_wlc.bytes + 192;
    switch_wlc.bytes[25] = 1;
    switch_configuration = (struct switch_a3_configuration){
        .address = {2, 3, 4, 5, 6, 7}, .bssid = {0, 0x25, 0, 0xff, 0x94, 0x73}, .chanspec = 0xd024};
    switch_rings = (struct switch_a3_ring_binding){.first_ring = 10, .ring_count = 2, .max_frame = 1800, .credits = 2};
    switch_copy(switch_envelope, marker, 6);
    switch_envelope[6] = 0x02;
    switch_envelope[7] = 0x57;
    switch_envelope[8] = 5;
    switch_envelope[12] = 0x88;
    switch_envelope[13] = 0xb5;
    struct switch_a3_tx_header header = {.magic = SWITCH_A3_TX_MAGIC,
                                         .version = SWITCH_A3_WIRE_VERSION,
                                         .header_bytes = 64,
                                         .total_bytes = 64 + frame_bytes,
                                         .session = 2,
                                         .clock_epoch = 3,
                                         .schedule_generation = 4,
                                         .request_id = 5,
                                         .frame_bytes = frame_bytes,
                                         .chanspec = 0xd024,
                                         .flags = SWITCH_A3_TX_ON_CHANNEL_ONLY,
                                         .earliest_tsf = 50,
                                         .expiry_tsf = 2000,
                                         .firmware_generation = 1};
    switch_copy(switch_envelope + 14, &header, 64);
    uint8_t *frame = switch_envelope + 78;
    switch_put16(frame, 0xd0);
    frame[4] = 2;
    frame[5] = 9;
    switch_copy(frame + 10, switch_configuration.address, 6);
    switch_copy(frame + 16, switch_configuration.bssid, 6);
    switch_envelope_bytes = 78 + frame_bytes;
    switch_prefix = split ? 14 : switch_envelope_bytes;
    for (unsigned index = 0; index < 6; ++index) {
        switch_table[index + 1] = switch_original[index];
        switch_original[index]->id = index + 1;
        switch_original[index]->flags = 0x4000 | (split ? 0x200 : 0);
        switch_original[index]->data = switch_input[index].bytes;
        switch_original[index]->length = metadata + switch_prefix;
        ((uint8_t *)switch_original[index])[18] = metadata / 4;
        switch_put16((uint8_t *)switch_original[index] + 62, 10);
        switch_put16((uint8_t *)switch_original[index] + 80, split ? switch_envelope_bytes - switch_prefix : 0);
        switch_copy(switch_input[index].bytes + metadata, switch_envelope, switch_prefix);
    }
    switch_fetch_pending = NULL;
    switch_allocated = switch_freed = switch_radio_count = switch_original_freed = switch_radio_freed = 0;
    switch_fetch_freed = switch_statuses = switch_sends = switch_normal_count = switch_fault =
        switch_cancelled_callbacks = 0;
    switch_oom = switch_fetch_failure = switch_register_failure = switch_send_failure = switch_immediate = 0;
    switch_fetch_immediate = switch_clock_error = 0;
    switch_elapsed_on_arm = switch_timer_fault = 0;
    switch_complete_other = 0;
    switch_complete_other_tsf = 0;
    switch_chanspec = 0xd024;
    switch_expire_on_register = 0;
    switch_expected_rate = 12;
    switch_clock_tsf = 1000;
}

static void switch_reset(uint16_t frame_bytes, int split, unsigned metadata)
{
    SWITCH_A3_DIAG(
        for (size_t index = 0; index < sizeof switch_a3_diagnostic; ++index)
            ((uint8_t *)&switch_a3_diagnostic)[index] = 0);
    switch_reset_input(frame_bytes, split, metadata);
    if (switch_a3_tx_configure(switch_wlc.bytes, &switch_configuration, &switch_rings, 2, 3, 4, 1))
        switch_fault = 1;
}

static void switch_ordinary(unsigned index)
{
    uint8_t *ethernet = switch_original[index]->data + ((uint8_t *)switch_original[index])[18] * 4u;
    static const uint8_t destination[6] = {2, 0, 0, 0, 0, 1};
    switch_copy(ethernet, destination, 6);
    ethernet[12] = 8;
    ethernet[13] = 0;
}

void *switch_a3_tx_allocate(uint32_t bytes)
{
    if (switch_oom)
        return NULL;
    if (bytes > sizeof switch_heap[0] || switch_allocated >= 6) {
        switch_fault = 2;
        return NULL;
    }
    void *result = switch_heap[switch_allocated++].bytes;
    switch_zero(result, bytes);
    return result;
}

void switch_a3_tx_free(void *allocation)
{
    ++switch_freed;
    for (unsigned index = 0; index < 6; ++index)
        if (allocation == switch_heap[index].bytes) {
            switch_zero(allocation, sizeof switch_heap[index]);
            return;
        }
    switch_fault = 3;
}

struct switch_a3_tx_packet *switch_a3_tx_packet_get(void *osh, uint32_t bytes)
{
    if (osh != switch_wlc.bytes + 64 || bytes > 202 + 1800)
        switch_fault = 4;
    if (switch_oom)
        return NULL;
    if (bytes == 64) {
        switch_status_packet.data = switch_buffers[5].bytes;
        switch_status_packet.flags = 0;
        switch_status_packet.length = bytes;
        return &switch_status_packet;
    }
    if (switch_radio_count >= 5) {
        switch_fault = 5;
        return NULL;
    }
    struct switch_a3_tx_packet *packet = switch_radio[switch_radio_count];
    packet->data = switch_buffers[switch_radio_count++].bytes;
    packet->length = bytes;
    return packet;
}

static void switch_radio_complete(unsigned index, uint32_t status)
{
    void (*fn)(void *, uint32_t, void *) = switch_callbacks[index].fn;
    void *arg = switch_callbacks[index].arg;
    switch_callbacks[index].fn = NULL;
    switch_callbacks[index].arg = NULL;
    if (fn)
        fn(switch_wlc.bytes, status, arg);
    ++switch_radio_freed;
}

void switch_a3_tx_status_original(void *pcb, struct switch_a3_tx_packet *packet, uint32_t status)
{
    if (pcb != switch_wlc.bytes + 128)
        switch_fault = 22;
    if ((status & 0x1cu) == 4)
        return;
    for (unsigned index = 0; index < 6; ++index)
        if (packet == switch_radio[index]) {
            switch_radio_complete(index, status);
            return;
        }
}

void switch_a3_tx_packet_free(void *osh, struct switch_a3_tx_packet *packet, int send)
{
    if (osh != switch_expected_osh)
        switch_fault = 6;
    for (unsigned index = 0; index < 6; ++index) {
        if (packet == switch_original[index]) {
            if (!send)
                switch_fault = 7;
            if (++switch_original_frees[index] != 1)
                switch_fault = 24;
            ++switch_original_freed;
            if (packet->next)
                switch_a3_tx_packet_free(osh, switch_table[packet->next], send);
            return;
        }
        if (packet == switch_radio[index]) {
            if (!send)
                switch_fault = 8;
            switch_radio_complete(index, 0);
            return;
        }
    }
    if (packet == &switch_fetch_packet && !send) {
        ++switch_fetch_freed;
        return;
    }
    switch_fault = 9;
}

int switch_a3_clock_read(void *wlc, uint64_t *tsf)
{
    if (wlc != switch_wlc.bytes)
        switch_fault = 10;
    *tsf = switch_clock_tsf;
    return switch_clock_error;
}

void switch_a3_control_retired(void)
{}

void switch_a3_control_fault(int error)
{
    switch_timer_fault = error;
    switch_a3_tx_cancel(switch_wlc.bytes);
}

void switch_a3_control_timer_fault(int error, const struct switch_a3_timer *timer, const struct switch_a3_owner *owner)
{
    (void)timer;
    (void)owner;
    switch_a3_control_fault(error);
}

int switch_a3_timer_initialize_owned(struct switch_a3_timer *timer, void *wlc)
{
    if (wlc != switch_wlc.bytes)
        return -2;
    switch_tx_timer = timer;
    timer->wlc = wlc;
    return 0;
}

int switch_a3_timer_cancel_owned(struct switch_a3_timer *timer, void *wlc)
{
    (void)wlc;
    timer->snapshot.armed = 0;
    return 0;
}

int switch_a3_timer_arm_owned(struct switch_a3_timer *timer, void *wlc, uint64_t deadline, uint32_t session,
                              uint32_t epoch, switch_a3_timer_callback callback, void *context)
{
    if (wlc != switch_wlc.bytes || deadline <= switch_clock_tsf)
        return -2;
    if (switch_elapsed_on_arm) {
        switch_elapsed_on_arm = 0;
        switch_clock_tsf = deadline;
        return SWITCH_A3_TIMER_ELAPSED;
    }
    timer->snapshot.requested_tsf = deadline;
    timer->snapshot.session_generation = session;
    timer->snapshot.clock_generation = epoch;
    timer->snapshot.armed = 1;
    timer->snapshot.status = 0;
    timer->callback = callback;
    timer->context = context;
    return 0;
}

int switch_a3_timer_defer_owned(struct switch_a3_timer *timer, void *wlc, uint32_t session, uint32_t epoch,
                                switch_a3_timer_callback callback, void *context)
{
    return switch_a3_timer_arm_owned(timer, wlc, switch_clock_tsf + 1, session, epoch, callback, context);
}

int switch_a3_scheduler_admit(void *wlc, uint16_t channel, uint64_t *until)
{
    if (wlc != switch_wlc.bytes || channel != switch_chanspec)
        return -35;
    *until = UINT64_MAX;
    return 0;
}

uint16_t switch_a3_tx_phy_chanspec(void *pi)
{
    if (pi != switch_pi.bytes)
        switch_fault = 11;
    return switch_chanspec;
}

int switch_a3_tx_register(void *pcb, void (*callback)(void *, uint32_t, void *), void *arg,
                          struct switch_a3_tx_packet *packet)
{
    if (pcb != switch_wlc.bytes + 128)
        switch_fault = 12;
    if (switch_register_failure)
        return -1;
    if (switch_expire_on_register)
        switch_clock_tsf = 2000;
    for (unsigned index = 0; index < 6; ++index)
        if (packet == switch_radio[index]) {
            switch_callbacks[index].fn = callback;
            switch_callbacks[index].arg = arg;
            return 0;
        }
    switch_fault = 13;
    return -1;
}

int switch_a3_tx_unregister(void *pcb, void (*callback)(void *, uint32_t, void *), void *arg, int cancel)
{
    if (pcb != switch_wlc.bytes + 128 || !cancel)
        switch_fault = 14;
    int count = 0;
    for (unsigned index = 0; index < 6; ++index)
        if (switch_callbacks[index].fn == callback && switch_callbacks[index].arg == arg) {
            switch_callbacks[index].fn = NULL;
            ++count;
            ++switch_cancelled_callbacks;
        }
    return count;
}

int switch_a3_tx_sendctl(void *wlc, struct switch_a3_tx_packet *packet, void *queue, void *scb, void *bsscfg,
                         int short_preamble, void *key, uint32_t rate)
{
    ++switch_sends;
    if (wlc != switch_wlc.bytes || queue != switch_wlc.bytes + 160 || scb != switch_scb.bytes ||
        bsscfg != switch_wlc.bytes + 192 || short_preamble || key || rate != switch_expected_rate)
        switch_fault = 15;
    unsigned index = 0;
    while (index < 6 && packet != switch_radio[index])
        ++index;
    if (switch_sends <= 6)
        switch_send_order[switch_sends - 1] = index;
    if (index >= 6 || packet->data != switch_buffers[index].bytes + 202 ||
        packet->length != switch_header()->frame_bytes || (*((uint8_t *)packet + 30) & 15))
        switch_fault = 16;
    for (unsigned byte = 0; byte < packet->length; ++byte)
        if (packet->data[byte] != switch_envelope[78 + byte])
            switch_fault = 17;
    if (!(*(uint32_t *)((uint8_t *)packet + 24) & 0x200u) || *(uint32_t *)((uint8_t *)packet + 36) != 1792)
        switch_fault = 23;
    const uint16_t sequence = (packet->data[22] | ((uint16_t)packet->data[23] << 8)) >> 4;
    if (*(uint16_t *)((uint8_t *)packet + 28) != (0x1000u | sequence))
        switch_fault = 24;
    if (switch_complete_other) {
        const unsigned other = (unsigned)switch_complete_other - 1;
        switch_complete_other = 0;
        switch_clock_tsf = switch_complete_other_tsf;
        switch_radio_complete(other, 0);
    }
    if (switch_send_failure || switch_immediate)
        switch_radio_complete(index, switch_send_failure ? 0 : 2);
    return !switch_send_failure;
}

void switch_a3_tx_sendup(void *wl, void *interface, struct switch_a3_tx_packet *packet)
{
    if (wl != switch_wl.bytes || interface || packet != &switch_status_packet || !(packet->flags & 0x80))
        switch_fault = 18;
    switch_copy(&switch_reply, packet->data, sizeof switch_reply);
    ++switch_statuses;
    if (switch_reply.tsf || (switch_reply.flags & SWITCH_A3_TX_TSF_VALID))
        switch_fault = 19;
}

struct switch_a3_packet;

struct switch_a3_packet *switch_a3_packet_get(void *osh, uint32_t bytes)
{
    return (struct switch_a3_packet *)switch_a3_tx_packet_get(osh, bytes);
}

void switch_a3_raw_sendup(void *wl, void *interface, struct switch_a3_packet *packet)
{
    switch_a3_tx_sendup(wl, interface, (struct switch_a3_tx_packet *)packet);
}

int switch_a3_tx_original(void *source, void *destination, struct switch_a3_tx_packet *packet)
{
    if (source != switch_device.bytes + 64 || destination != switch_device.bytes)
        switch_fault = 20;
    while (packet && switch_normal_count < 6) {
        switch_normal_ids[switch_normal_count++] = packet->id;
        packet = packet->link ? switch_table[packet->link] : NULL;
    }
    return 17;
}

static void switch_fetch_complete(int cancelled)
{
    struct switch_a3_tx_fetch *request = switch_fetch_pending;
    switch_fetch_pending = NULL;
    switch_fetch_packet.data = switch_fetched.bytes + request->headroom;
    switch_fetch_packet.length = switch_envelope_bytes - switch_prefix;
    switch_copy(switch_fetch_packet.data, switch_envelope + switch_prefix, switch_fetch_packet.length);
    request->callback(&switch_fetch_packet, request->original, request->context, cancelled);
}

int switch_a3_tx_fetch(struct switch_a3_tx_fetch *request)
{
    if (request->offset || request->next || request->headroom != switch_prefix ||
        request->headroom + switch_envelope_bytes - switch_prefix > 1920)
        switch_fault = 21;
    if (switch_fetch_failure)
        return -1;
    switch_fetch_pending = request;
    if (switch_fetch_immediate)
        switch_fetch_complete(0);
    return 0;
}

static void switch_enter(void)
{
    switch_a3_tx_entry(switch_device.bytes + 64, switch_device.bytes, switch_original[0]);
}

static void switch_fire_timer(uint64_t now)
{
    switch_clock_tsf = now;
    switch_tx_timer->snapshot.armed = 0;
    switch_tx_timer->callback(switch_tx_timer->context, switch_tx_timer->snapshot.requested_tsf, now,
                              switch_tx_timer->snapshot.session_generation, switch_tx_timer->snapshot.clock_generation);
}

static uint32_t switch_test_transfer(unsigned variant)
{
    switch_reset(variant == 4 ? 1800 : 32, variant != 0 && variant != 1, variant == 3 ? 8 : 0);
    switch_immediate = variant == 1;
    switch_fetch_immediate = variant == 5;
    switch_enter();
    if (switch_fetch_pending)
        switch_fetch_complete(0);
    if (!switch_immediate)
        switch_radio_complete(0, 2);
    SWITCH_REQUIRE(!switch_fault && switch_original_freed == 1 && switch_radio_freed == 1 && switch_freed == 1);
    SWITCH_REQUIRE(switch_statuses == 1 && switch_reply.status == SWITCH_A3_TX_SENT && switch_reply.request_id == 5);
    SWITCH_REQUIRE(switch_reply.flags == (SWITCH_A3_TX_ACK_VALID | SWITCH_A3_TX_ACKED) && switch_reply.credits == 2);
    return 0;
}

static uint32_t switch_test_failure(unsigned variant)
{
    switch_reset(32, variant == 2 || variant == 7, 0);
    switch_oom = variant == 0;
    switch_register_failure = variant == 1;
    switch_fetch_failure = variant == 2;
    switch_send_failure = variant == 3;
    switch_clock_error = variant == 4 ? -35 : 0;
    switch_chanspec = variant == 5 ? 0x1006 : 0xd024;
    if (variant == 6)
        switch_wlc.bytes[25] = 0;
    switch_enter();
    if (variant == 7)
        switch_fetch_complete(1);
    if (variant == 5)
        switch_fire_timer(2000);
    SWITCH_REQUIRE(!switch_fault && switch_original_freed == 1 && !switch_normal_count);
    if (variant)
        SWITCH_REQUIRE(switch_statuses == 1 && switch_reply.status != SWITCH_A3_TX_SENT && switch_freed == 1);
    else
        SWITCH_REQUIRE(!switch_statuses && !switch_freed);
    return 0;
}

static uint32_t switch_test_bad_header(unsigned variant)
{
    switch_reset(32, 0, 0);
    struct switch_a3_tx_header header;
    switch_copy(&header, switch_envelope + 14, 64);
    if (variant == 0)
        ++header.session;
    if (variant == 1)
        ++header.clock_epoch;
    if (variant == 2)
        ++header.schedule_generation;
    if (variant == 3)
        ++header.firmware_generation;
    if (variant == 4)
        --header.total_bytes;
    if (variant == 5)
        header.earliest_tsf = 2001;
    if (variant == 6)
        header.expiry_tsf = 100;
    if (variant == 7)
        header.flags |= SWITCH_A3_TX_NO_ACK;
    if (variant == 8)
        header.flags = 0;
    switch_copy(switch_input[0].bytes + 14, &header, 64);
    switch_enter();
    SWITCH_REQUIRE(!switch_fault && !switch_sends && switch_original_freed == 1 && switch_freed == 1);
    SWITCH_REQUIRE(switch_statuses == 1 &&
                   switch_reply.status == (variant == 6 ? SWITCH_A3_TX_EXPIRED : SWITCH_A3_TX_FAILED));
    return 0;
}

static uint32_t switch_test_decline(unsigned variant)
{
    switch_reset(32, 0, variant == 3 ? 8 : 0);
    switch_ordinary(0);
    if (variant == 0)
        switch_original[0]->flags = 0;
    if (variant == 1)
        switch_put16((uint8_t *)switch_original[0] + 62, 9);
    switch_enter();
    SWITCH_REQUIRE(!switch_fault && switch_normal_count == 1 && switch_normal_ids[0] == 1);
    SWITCH_REQUIRE(!switch_allocated && !switch_original_freed && !switch_sends && !switch_statuses);
    return 0;
}

static uint32_t switch_test_cancel(int dma)
{
    switch_reset(32, dma, 0);
    switch_enter();
    switch_a3_tx_cancel(switch_wlc.bytes);
    SWITCH_REQUIRE(switch_a3_tx_pending() == 1 && !switch_a3_tx_credits() && !switch_original_freed && !switch_freed &&
                   !switch_statuses && !switch_cancelled_callbacks);
    SWITCH_REQUIRE(switch_a3_tx_configure(switch_wlc.bytes, &switch_configuration, &switch_rings, 2, 3, 4, 1) == -16);
    if (dma) {
        SWITCH_REQUIRE(!switch_original_freed);
        switch_fetch_complete(0);
    } else
        switch_radio_complete(0, 2);
    SWITCH_REQUIRE(!switch_fault && switch_original_freed == 1 && switch_freed == 1 && switch_statuses == 1);
    SWITCH_REQUIRE(switch_reply.status == SWITCH_A3_TX_CANCELLED);
    if (dma)
        SWITCH_REQUIRE(!switch_sends);
    SWITCH_REQUIRE(!switch_a3_tx_credits() && !switch_reply.credits);
    SWITCH_REQUIRE(!switch_a3_tx_pending());
    return 0;
}

static uint32_t switch_test_mixed(void)
{
    switch_reset(32, 0, 0);
    switch_original[0]->link = 2;
    switch_original[1]->link = 3;
    switch_original[2]->link = 4;
    switch_ordinary(1);
    switch_ordinary(3);
    switch_enter();
    switch_radio_complete(0, 2);
    switch_radio_complete(1, 2);
    SWITCH_REQUIRE(!switch_fault && switch_normal_count == 2 && switch_normal_ids[0] == 2 && switch_normal_ids[1] == 4);
    SWITCH_REQUIRE(switch_sends == 2 && switch_original_freed == 2 && switch_statuses == 2 && switch_freed == 2);
    return 0;
}

static uint32_t switch_test_credits(void)
{
    switch_reset(32, 0, 0);
    SWITCH_REQUIRE(switch_a3_tx_credits() == 2);
    switch_original[0]->link = 2;
    switch_original[1]->link = 3;
    switch_enter();
    SWITCH_REQUIRE(switch_sends == 2 && switch_original_freed == 1 && switch_allocated == 2);
    SWITCH_REQUIRE(!switch_a3_tx_credits());
    SWITCH_REQUIRE(switch_a3_tx_configure(switch_wlc.bytes, &switch_configuration, &switch_rings, 2, 3, 4, 1) == -16);
    switch_radio_complete(0, 2);
    SWITCH_REQUIRE(switch_a3_tx_credits() == 1);
    switch_radio_complete(1, 2);
    SWITCH_REQUIRE(!switch_fault && switch_original_freed == 3 && switch_freed == 2);
    SWITCH_REQUIRE(switch_a3_tx_credits() == 2);
    return 0;
}

static uint32_t switch_test_group_without_evidence(void)
{
    switch_reset(32, 0, 0);
    switch_input[0].bytes[78 + 4] = 1;
    switch_envelope[78 + 4] = 1;
    switch_enter();
    switch_radio_complete(0, 0);
    SWITCH_REQUIRE(!switch_fault && switch_statuses == 1 && switch_reply.status == SWITCH_A3_TX_FAILED &&
                   !switch_reply.flags);
    return 0;
}

static uint32_t switch_test_extra_fragment(void)
{
    switch_reset(32, 1, 0);
    switch_put16((uint8_t *)switch_original[0] + 84, 1);
    switch_enter();
    SWITCH_REQUIRE(!switch_fault && !switch_fetch_pending && !switch_sends && switch_original_freed == 1);
    return 0;
}

static uint32_t switch_test_inline(unsigned variant)
{
    switch_reset(32, variant == 0, 0);
    if (variant == 0)
        switch_fetch_failure = 1;
    if (variant == 1)
        switch_input[0].bytes[14 + 24] = 6;
    if (variant == 2)
        switch_input[0].bytes[6] = 0;
    switch_enter();
    SWITCH_REQUIRE(!switch_fault && !switch_sends && switch_original_freed == 1 && !switch_normal_count);
    if (variant == 2)
        SWITCH_REQUIRE(!switch_allocated && !switch_statuses);
    else
        SWITCH_REQUIRE(switch_statuses == 1 && switch_reply.request_id == 5 && switch_reply.session == 2 &&
                       switch_reply.clock_epoch == 3 && switch_reply.schedule_generation == 4 &&
                       switch_reply.status == SWITCH_A3_TX_FAILED);
    return 0;
}

static uint32_t switch_test_hardware_status(unsigned variant)
{
    switch_reset(32, 0, 0);
    if (variant != 1 && variant != 6 && variant != 7)
        switch_input[0].bytes[82] = switch_envelope[82] = 1;
    switch_enter();
    if (variant == 5)
        switch_a3_tx_cancel(switch_wlc.bytes);
    uint32_t status = (variant == 2 || variant == 6) ? 4 : (variant == 3 || variant == 7) ? 0x40 : 0;
    switch_a3_tx_radio_status(switch_wlc.bytes + 128,
                              variant == 4 ? (void *)switch_original[0] : (void *)switch_radio[0], status);
    if (variant == 2 || variant == 4 || variant == 6)
        switch_radio_complete(0, 0);
    SWITCH_REQUIRE(!switch_fault && switch_original_freed == 1 && switch_freed == 1 && switch_statuses == 1);
    SWITCH_REQUIRE(switch_reply.status == (variant == 0   ? SWITCH_A3_TX_SENT
                                           : variant == 5 ? SWITCH_A3_TX_CANCELLED
                                           : variant >= 6 ? SWITCH_A3_TX_EXPIRED
                                                          : SWITCH_A3_TX_FAILED) &&
                   !switch_reply.flags);
    SWITCH_A3_DIAG(
        SWITCH_REQUIRE(switch_a3_diagnostic.counters[SWITCH_A3_DIAG_TX_INGRESS] == 1 &&
                       switch_a3_diagnostic.counters[SWITCH_A3_DIAG_TX_NATIVE] == 1 &&
                       switch_a3_diagnostic.counters[SWITCH_A3_DIAG_TX_STATUS] == (variant != 4)));
    return 0;
}

void switch_a3_channel_pm_status(void *wlc, void *bsscfg, void *packet, uint32_t fc, uint32_t status,
                                 const uint8_t *header)
{
    switch_pm_arguments[0] = (uintptr_t)wlc;
    switch_pm_arguments[1] = (uintptr_t)bsscfg;
    switch_pm_arguments[2] = (uintptr_t)packet;
    switch_pm_arguments[3] = fc;
    switch_pm_arguments[4] = status;
    switch_pm_arguments[5] = (uintptr_t)header;
}

uint32_t switch_a3_pm_status_record(void *bsscfg, uint32_t status, uint32_t r2, uint32_t r3, uint32_t r12)
{
    switch_pm_fallback[0] = (uintptr_t)bsscfg;
    switch_pm_fallback[1] = status;
    switch_pm_fallback[2] = r2;
    switch_pm_fallback[3] = r3;
    switch_pm_fallback[4] = r12;
    return 0x79a5;
}

extern uint32_t switch_pm_test_entry(void *, void *, const void *);

static uint32_t switch_test_pm_veneer(void)
{
    *(void **)switch_band.bytes = switch_wlc.bytes;
    SWITCH_REQUIRE(switch_pm_test_entry(switch_band.bytes, switch_original[0], switch_envelope + 78) == 0x79a5);
    SWITCH_REQUIRE(switch_pm_arguments[0] == (uintptr_t)switch_wlc.bytes &&
                   switch_pm_arguments[1] == (uintptr_t)switch_band.bytes &&
                   switch_pm_arguments[2] == (uintptr_t)switch_original[0] && switch_pm_arguments[3] == 0x1148 &&
                   switch_pm_arguments[4] == 0x1202 && switch_pm_arguments[5] == (uintptr_t)(switch_envelope + 78));
    SWITCH_REQUIRE(switch_pm_fallback[0] == (uintptr_t)switch_band.bytes && switch_pm_fallback[1] == 0x1202 &&
                   switch_pm_fallback[2] == 0x2233 && switch_pm_fallback[3] == 0x4455 &&
                   switch_pm_fallback[4] == 0x6677);
    return 0;
}

static uint32_t switch_test_closed(unsigned ordinary)
{
    switch_reset(32, 1, 0);
    switch_a3_tx_cancel(switch_wlc.bytes);
    if (ordinary)
        switch_ordinary(0);
    if (ordinary)
        switch_original[0]->flags = 0;
    switch_enter();
    SWITCH_REQUIRE(!switch_fault && !switch_allocated && !switch_sends && !switch_fetch_pending);
    if (ordinary)
        SWITCH_REQUIRE(switch_normal_count == 1 && !switch_original_freed && !switch_statuses);
    else
        SWITCH_REQUIRE(!switch_normal_count && switch_original_freed == 1 && switch_statuses == 1 &&
                       switch_reply.status == SWITCH_A3_TX_CANCELLED && switch_reply.request_id == 5);
    return 0;
}

static uint32_t switch_test_final_expiry(void)
{
    switch_reset(32, 0, 0);
    switch_expire_on_register = 1;
    switch_enter();
    SWITCH_REQUIRE(!switch_fault && !switch_sends && switch_original_freed == 1 && switch_radio_freed == 1 &&
                   switch_freed == 1 && switch_cancelled_callbacks == 1 && switch_statuses == 1 &&
                   switch_reply.status == SWITCH_A3_TX_EXPIRED);
    SWITCH_A3_DIAG(
        SWITCH_REQUIRE(!switch_a3_diagnostic.counters[SWITCH_A3_DIAG_TX_NATIVE]));
    return 0;
}

static uint32_t switch_test_binding(unsigned variant)
{
    struct switch_a3_ring_binding rings = {.first_ring = 2, .ring_count = 1, .max_frame = 1800, .credits = 8};
    if (variant == 2)
        rings.first_ring = 1;
    if (variant == 3)
        rings.ring_count = 0;
    if (variant == 4) {
        rings.first_ring = 65535;
        rings.ring_count = 2;
    }
    if (variant == 5)
        rings.max_frame = 23;
    if (variant == 6)
        rings.max_frame = 1801;
    if (variant == 7)
        rings.credits = 0;
    if (variant == 8)
        rings.credits = 9;
    if (variant == 9)
        rings.reserved = 1;
    if (variant == 10)
        rings.reserved2 = 1;
    if (variant == 11) {
        rings.first_ring = 65535;
        rings.max_frame = 24;
        rings.credits = 1;
    }
    SWITCH_REQUIRE(switch_a3_tx_binding_valid(variant == 1 ? NULL : &rings) == (variant == 0 || variant == 11));
    return 0;
}

static uint32_t switch_test_unobserved_retirement(void)
{
    switch_reset(32, 0, 0);
    switch_enter();
    switch_a3_tx_cancel(switch_wlc.bytes);
    switch_callbacks[0].fn = NULL;
    SWITCH_REQUIRE(!switch_fault && switch_a3_tx_pending() == 1 && !switch_a3_tx_credits() && !switch_original_freed &&
                   !switch_freed && !switch_statuses && !switch_cancelled_callbacks);
    SWITCH_REQUIRE(switch_a3_tx_configure(switch_wlc.bytes, &switch_configuration, &switch_rings, 2, 3, 5, 1) == -16);
    return 0;
}

static uint32_t switch_test_unconfigured(void)
{
    switch_reset_input(32, 1, 0);
    switch_original[0]->link = 2;
    switch_ordinary(1);
    switch_enter();
    SWITCH_REQUIRE(!switch_fault && switch_original_freed == 1 && switch_original_frees[0] == 1 &&
                   switch_normal_count == 1 && switch_normal_ids[0] == 2);
    SWITCH_REQUIRE(!switch_sends && !switch_allocated && !switch_statuses && !switch_fetch_pending);
    return 0;
}

static uint32_t switch_test_private_rejection(unsigned variant)
{
    switch_reset(32, 1, variant == 4 ? 8 : 0);
    if (variant == 0) {
        switch_original[0]->flags = 0;
        switch_put16((uint8_t *)switch_original[0] + 62, 65535);
    }
    if (variant == 1)
        switch_put16((uint8_t *)switch_original[0] + 62, 9);
    if (variant == 2)
        switch_put16((uint8_t *)switch_original[0] + 62, 12);
    if (variant == 3)
        switch_original[0]->next = 2;
    if (variant == 4) {
        *(void **)(switch_other_wlc.bytes + 4) = switch_other_wlc.bytes + 64;
        *(void **)(switch_wl.bytes + 8) = switch_other_wlc.bytes;
        switch_expected_osh = switch_other_wlc.bytes + 64;
        switch_original[0]->link = 2;
        switch_ordinary(1);
    }
    switch_enter();
    SWITCH_REQUIRE(!switch_fault && !switch_sends && !switch_allocated && !switch_statuses && !switch_fetch_pending);
    SWITCH_REQUIRE(switch_original_freed == (variant == 3 ? 2u : 1u) && switch_original_frees[0] == 1);
    SWITCH_REQUIRE(switch_original_frees[1] == (variant == 3 ? 1u : 0u));
    SWITCH_REQUIRE(switch_normal_count == (variant == 4 ? 1u : 0u));
    if (variant == 4)
        SWITCH_REQUIRE(switch_normal_ids[0] == 2);
    return 0;
}

static uint32_t switch_test_mixed_rejection(void)
{
    switch_reset(32, 0, 0);
    for (unsigned index = 0; index < 4; ++index)
        switch_original[index]->link = index + 2;
    switch_ordinary(1);
    switch_ordinary(3);
    switch_put16((uint8_t *)switch_original[2] + 62, 9);
    switch_original[4]->next = 6;
    switch_enter();
    switch_radio_complete(0, 2);
    SWITCH_REQUIRE(!switch_fault && switch_sends == 1 && switch_allocated == 1 && switch_freed == 1);
    SWITCH_REQUIRE(switch_statuses == 1 && switch_normal_count == 2 && switch_normal_ids[0] == 2 &&
                   switch_normal_ids[1] == 4);
    SWITCH_REQUIRE(switch_original_freed == 4 && switch_original_frees[0] == 1 && !switch_original_frees[1] &&
                   switch_original_frees[2] == 1 && !switch_original_frees[3] && switch_original_frees[4] == 1 &&
                   switch_original_frees[5] == 1);
    return 0;
}

static uint32_t switch_test_unresolved_context(unsigned variant)
{
    switch_reset(32, 1, 0);
    switch_original[0]->link = 2;
    switch_original[0]->next = 3;
    if (variant == 0)
        *(void **)(switch_device.bytes + 24) = NULL;
    if (variant == 1)
        *(void **)(switch_wl.bytes + 8) = NULL;
    SWITCH_REQUIRE(switch_a3_tx_entry(switch_device.bytes + 64, variant == 2 ? NULL : switch_device.bytes,
                                      switch_original[0]) == 1);
    SWITCH_REQUIRE(switch_original[0]->link == 2 && switch_original[0]->next == 3);
    SWITCH_REQUIRE(!switch_fault && !switch_normal_count && !switch_sends && !switch_allocated && !switch_statuses &&
                   !switch_original_freed && !switch_fetch_pending);
    return 0;
}

static uint32_t switch_test_deferred(unsigned variant)
{
    switch_reset(32, 0, 0);
    struct switch_a3_tx_header header;
    switch_copy(&header, switch_input[0].bytes + 14, sizeof header);
    if (!variant)
        header.earliest_tsf = 1500;
    else
        switch_chanspec = 0x1006;
    switch_elapsed_on_arm = variant == 4;
    switch_copy(switch_input[0].bytes + 14, &header, sizeof header);
    switch_enter();
    SWITCH_REQUIRE(!switch_fault && !switch_sends && switch_a3_tx_pending() == 1 && !switch_statuses);
    SWITCH_REQUIRE(switch_tx_timer->snapshot.armed);
    if (!variant) {
        SWITCH_REQUIRE(switch_tx_timer->snapshot.requested_tsf == 1500);
        switch_fire_timer(1499);
        SWITCH_REQUIRE(!switch_sends && switch_a3_tx_pending() == 1);
        switch_fire_timer(1500);
    } else if (variant == 1) {
        switch_clock_tsf = 1200;
        switch_chanspec = 0xd024;
        switch_a3_tx_wake(switch_wlc.bytes);
    } else if (variant == 2) {
        switch_fire_timer(2000);
    } else if (variant == 4) {
        SWITCH_REQUIRE(!switch_timer_fault && switch_tx_timer->snapshot.requested_tsf == 2001);
        switch_chanspec = 0xd024;
        switch_fire_timer(2001);
    } else if (variant == 5) {
        switch_tx_timer->snapshot.status = -1;
        switch_chanspec = 0xd024;
        switch_fire_timer(1500);
        SWITCH_REQUIRE(switch_timer_fault == -1);
    } else {
        switch_a3_tx_cancel(switch_wlc.bytes);
    }
    if (variant < 2) {
        SWITCH_REQUIRE(switch_sends == 1 && switch_a3_tx_pending() == 1 && !switch_statuses);
        switch_radio_complete(0, 2);
        SWITCH_REQUIRE(switch_reply.status == SWITCH_A3_TX_SENT);
    } else {
        SWITCH_REQUIRE(!switch_sends && switch_reply.status == (variant == 2 || variant == 4 ? SWITCH_A3_TX_EXPIRED
                                                                                             : SWITCH_A3_TX_CANCELLED));
        SWITCH_REQUIRE(!switch_cancelled_callbacks);
    }
    SWITCH_REQUIRE(!switch_fault && !switch_a3_tx_pending() && switch_original_freed == 1 && switch_freed == 1 &&
                   switch_statuses == 1 && !switch_tx_timer->snapshot.armed);
    return 0;
}

static uint32_t switch_test_unicast_data(unsigned variant)
{
    switch_reset(32, 0, 0);
    uint8_t *frame = switch_envelope + 78;
    switch_put16(frame, variant == 1 || variant == 4 ? 0x88 : variant == 2 ? 0x0408 : 8);
    switch_put16(frame + 22, variant == 3 ? 0xabc1 : 0xabc0);
    if (variant == 4)
        frame[24] = 0x20;
    switch_copy(switch_input[0].bytes, switch_envelope, switch_envelope_bytes);
    switch_enter();
    if (variant < 2) {
        SWITCH_REQUIRE(switch_sends == 1 && !switch_statuses && switch_a3_tx_pending() == 1);
        switch_radio_complete(0, 2);
        SWITCH_REQUIRE(switch_reply.status == SWITCH_A3_TX_SENT &&
                       switch_reply.flags == (SWITCH_A3_TX_ACK_VALID | SWITCH_A3_TX_ACKED));
    } else {
        SWITCH_REQUIRE(!switch_sends && switch_reply.status == SWITCH_A3_TX_FAILED);
    }
    SWITCH_REQUIRE(!switch_fault && switch_statuses == 1 && !switch_a3_tx_pending() && switch_original_freed == 1);
    return 0;
}

static uint32_t switch_test_other_completion(unsigned expire)
{
    switch_reset(32, 0, 0);
    switch_rings.credits = 4;
    SWITCH_REQUIRE(!switch_a3_tx_configure(switch_wlc.bytes, &switch_configuration, &switch_rings, 2, 3, 4, 1));
    for (unsigned index = 0; index < 3; ++index) {
        struct switch_a3_tx_header header;
        switch_copy(&header, switch_input[index].bytes + 14, sizeof header);
        header.earliest_tsf = index == 1 ? 50 : 1500;
        header.request_id += index;
        switch_copy(switch_input[index].bytes + 14, &header, sizeof header);
        switch_input[index].bytes[8] += index;
        switch_a3_tx_entry(switch_device.bytes + 64, switch_device.bytes, switch_original[index]);
    }
    SWITCH_REQUIRE(!switch_fault && switch_sends == 1 && switch_a3_tx_pending() == 3);
    switch_complete_other = 2;
    switch_complete_other_tsf = expire ? 2000 : 1600;
    switch_clock_tsf = 1500;
    switch_a3_tx_wake(switch_wlc.bytes);
    const int progressed = !switch_fault && switch_sends == (expire ? 2u : 3u) &&
                           switch_a3_tx_pending() == (expire ? 1u : 2u) && switch_original_frees[1] == 1 &&
                           (expire ? switch_original_frees[2] == 1 && switch_reply.request_id == 7 &&
                                         switch_reply.status == SWITCH_A3_TX_EXPIRED
                                   : !switch_original_frees[2]) &&
                           !switch_tx_timer->snapshot.armed;
    for (unsigned index = 0; index < 3; ++index)
        if (switch_callbacks[index].fn)
            switch_radio_complete(index, 2);
    SWITCH_REQUIRE(!switch_a3_tx_cancel(switch_wlc.bytes));
    SWITCH_REQUIRE(progressed);
    SWITCH_REQUIRE(!switch_fault && !switch_a3_tx_pending() && switch_original_freed == 3 && switch_radio_freed == 3 &&
                   switch_freed == 3 && switch_statuses == 3 && !switch_tx_timer->snapshot.armed);
    return 0;
}

static uint32_t switch_test_deferred_fifo(void)
{
    switch_reset(32, 0, 0);
    switch_chanspec = 0x1006;
    for (unsigned index = 0; index < 2; ++index) {
        struct switch_a3_tx_header header;
        switch_copy(&header, switch_input[index].bytes + 14, sizeof header);
        header.request_id += index;
        switch_copy(switch_input[index].bytes + 14, &header, sizeof header);
        switch_input[index].bytes[8] += index;
        switch_a3_tx_entry(switch_device.bytes + 64, switch_device.bytes, switch_original[index]);
    }
    SWITCH_REQUIRE(!switch_fault && !switch_sends && switch_a3_tx_pending() == 2);
    switch_chanspec = 0xd024;
    switch_a3_tx_wake(switch_wlc.bytes);
    SWITCH_REQUIRE(!switch_fault && switch_sends == 2 && switch_send_order[0] == 0 && switch_send_order[1] == 1);
    for (unsigned index = 0; index < 2; ++index)
        switch_radio_complete(index, 2);
    SWITCH_REQUIRE(!switch_fault && !switch_a3_tx_pending() && switch_original_freed == 2 && switch_statuses == 2);
    return 0;
}

static uint32_t switch_test_rates(void)
{
    static const uint32_t valid[] = {0, 12, 24, 48, 72, 108};
    for (unsigned index = 0; index < sizeof valid / sizeof valid[0]; ++index) {
        switch_reset(32, 0, 0);
        struct switch_a3_tx_header header;
        switch_copy(&header, switch_envelope + 14, sizeof header);
        header.rate = valid[index];
        switch_expected_rate = valid[index] ? valid[index] : 12;
        switch_copy(switch_input[0].bytes + 14, &header, sizeof header);
        switch_enter();
        switch_radio_complete(0, 2);
        SWITCH_REQUIRE(!switch_fault && switch_sends == 1 && switch_statuses == 1 &&
                       switch_reply.status == SWITCH_A3_TX_SENT);
    }
    static const uint32_t invalid[] = {11, 18, 109};
    for (unsigned index = 0; index < sizeof invalid / sizeof invalid[0]; ++index) {
        switch_reset(32, 0, 0);
        struct switch_a3_tx_header header;
        switch_copy(&header, switch_envelope + 14, sizeof header);
        header.rate = invalid[index];
        switch_copy(switch_input[0].bytes + 14, &header, sizeof header);
        switch_enter();
        SWITCH_REQUIRE(!switch_fault && !switch_sends && switch_statuses == 1 &&
                       switch_reply.status == SWITCH_A3_TX_FAILED);
    }
    return 0;
}

void switch_tx_test_run(void)
{
    extern void switch_timing_test_run(void);
    switch_tx_test_results[58] = switch_test_unconfigured();
    for (unsigned index = 0; index < 6; ++index)
        switch_tx_test_results[index] = switch_test_transfer(index);
    for (unsigned index = 0; index < 8; ++index)
        switch_tx_test_results[6 + index] = switch_test_failure(index);
    for (unsigned index = 0; index < 9; ++index)
        switch_tx_test_results[14 + index] = switch_test_bad_header(index);
    for (unsigned index = 0; index < 4; ++index)
        switch_tx_test_results[23 + index] = switch_test_decline(index);
    switch_tx_test_results[27] = switch_test_cancel(0);
    switch_tx_test_results[28] = switch_test_cancel(1);
    switch_tx_test_results[29] = switch_test_mixed();
    switch_tx_test_results[30] = switch_test_credits();
    switch_tx_test_results[31] = switch_test_group_without_evidence();
    switch_tx_test_results[32] = switch_test_extra_fragment();
    for (unsigned index = 0; index < 3; ++index)
        switch_tx_test_results[33 + index] = switch_test_inline(index);
    for (unsigned index = 0; index < 6; ++index)
        switch_tx_test_results[36 + index] = switch_test_hardware_status(index);
    switch_tx_test_results[42] = switch_test_pm_veneer();
    switch_tx_test_results[43] = switch_test_closed(0);
    switch_tx_test_results[44] = switch_test_closed(1);
    switch_tx_test_results[45] = switch_test_final_expiry();
    for (unsigned index = 0; index < 12; ++index)
        switch_tx_test_results[46 + index] = switch_test_binding(index);
    for (unsigned index = 0; index < 5; ++index)
        switch_tx_test_results[59 + index] = switch_test_private_rejection(index);
    switch_tx_test_results[64] = switch_test_mixed_rejection();
    for (unsigned index = 0; index < 3; ++index)
        switch_tx_test_results[65 + index] = switch_test_unresolved_context(index);
    switch_timing_test_run();
    for (unsigned index = 0; index < 4; ++index)
        switch_tx_test_results[69 + index] = switch_test_deferred(index);
    for (unsigned index = 0; index < 5; ++index)
        switch_tx_test_results[73 + index] = switch_test_unicast_data(index);
    switch_tx_test_results[78] = switch_test_deferred(4);
    switch_tx_test_results[79] = switch_test_deferred(5);
    switch_tx_test_results[80] = switch_test_other_completion(0);
    switch_tx_test_results[81] = switch_test_other_completion(1);
    switch_tx_test_results[82] = switch_test_deferred_fifo();
    switch_tx_test_results[83] = switch_test_rates();
    switch_tx_test_results[84] = switch_test_hardware_status(6);
    switch_tx_test_results[85] = switch_test_hardware_status(7);
    /* Unobserved retirement deliberately retains a native TX owner until reset. */
    switch_tx_test_results[68] = switch_test_unobserved_retirement();
    switch_tx_test_done = 0x53574133;
}

/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "rx.h"
#include "diagnostic.h"

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
    void *context, *next;
};

struct switch_limit_packet {
    struct switch_a3_packet packet;
    uint8_t reserved[72];
};
static struct switch_limit_packet switch_packets[5];
static uint8_t switch_frames[5][128];
static uint32_t switch_wlc[512], switch_band[8], switch_osh, switch_wl;
static const uint8_t switch_address[6] = {2, 3, 4, 5, 6, 7};
static struct switch_a3_fetch *switch_requests[5];
static uint64_t switch_heap[5][32];
static uint32_t switch_allocated, switch_queued, switch_retired, switch_freed, switch_original_freed[5], switch_fault;
static uint32_t switch_clock_reads, switch_timestamp_reads, switch_beacons, switch_scheduler_wakes;
volatile uint32_t switch_rx_limit_test_results[3], switch_rx_limit_test_done;
static struct switch_a3_packet switch_linear_packet;
static uint8_t switch_linear_bytes[128];
static uint32_t switch_linear_enabled, switch_exports;

#define SWITCH_REQUIRE(condition) \
    do {                          \
        if (!(condition))         \
            return __LINE__;      \
    } while (0)

int switch_a3_clock_read(void *wlc, uint64_t *tsf)
{
    ++switch_clock_reads;
    if (wlc != switch_wlc)
        switch_fault = 1;
    *tsf = 1000;
    return 0;
}

int switch_a3_beacon_observe(void *wlc, const void *status, uint32_t status_bytes, const uint8_t *frame,
                             uint32_t frame_bytes, uint64_t received_tsf)
{
    (void)wlc;
    (void)status;
    (void)status_bytes;
    (void)frame;
    (void)frame_bytes;
    (void)received_tsf;
    ++switch_beacons;
    return 1;
}

void switch_a3_scheduler_observed(void *wlc)
{
    (void)wlc;
    ++switch_scheduler_wakes;
}

void switch_a3_tsf_receive(void *wlc, const void *status, uint32_t *high, uint32_t *low)
{
    (void)wlc;
    (void)status;
    ++switch_timestamp_reads;
    *high = 0;
    *low = 1000;
}

void *switch_a3_malloc_zero(uint32_t length)
{
    if (switch_allocated >= 5 || length > sizeof *switch_heap) {
        switch_fault = 2;
        return NULL;
    }
    return switch_heap[switch_allocated++];
}

void switch_a3_free(void *allocation)
{
    int found = 0;
    for (uint32_t index = 0; index < switch_allocated; ++index)
        found |= allocation == switch_heap[index];
    if (!found)
        switch_fault = 3;
    ++switch_freed;
}

struct switch_a3_packet *switch_a3_packet_get(void *osh, uint32_t bytes)
{
    if (switch_linear_enabled && osh == &switch_osh && bytes <= sizeof switch_linear_bytes) {
        switch_linear_packet.data = switch_linear_bytes;
        return &switch_linear_packet;
    }
    switch_fault = 4;
    return NULL;
}

void switch_a3_packet_free(void *osh, struct switch_a3_packet *packet, int send)
{
    if (osh != &switch_osh || send)
        switch_fault = 5;
    for (uint32_t index = 0; index < 5; ++index)
        if (packet == &switch_packets[index].packet) {
            ++switch_original_freed[index];
            return;
        }
    switch_fault = 6;
}

int switch_a3_packet_fetch(struct switch_a3_fetch *request)
{
    if (switch_queued >= 5 || request->next || request->host_offset || request->headroom != 88) {
        switch_fault = 7;
        return -1;
    }
    switch_requests[switch_queued++] = request;
    if (switch_queued - switch_retired > switch_rx_limit_test_results[1])
        switch_rx_limit_test_results[1] = switch_queued - switch_retired;
    return 0;
}

void switch_a3_raw_sendup(void *wl, void *interface, struct switch_a3_packet *packet)
{
    if (switch_linear_enabled && wl == &switch_wl && !interface && packet == &switch_linear_packet) {
        ++switch_exports;
        return;
    }
    switch_fault = 8;
}

static void switch_complete(uint32_t index)
{
    struct switch_a3_fetch *request = switch_requests[index];
    ++switch_retired;
    request->callback(NULL, request->original, request->context, 1);
}

static uint32_t switch_test_limit(void)
{
    static const uint8_t bssid[6] = {0, 0x25, 0, 0xff, 0x94, 0x73};
    switch_wlc[1] = (uint32_t)&switch_osh;
    switch_wlc[2] = (uint32_t)&switch_wl;
    switch_wlc[8] = (uint32_t)switch_band;
    switch_wlc[0x5f0 / 4] = 32;
    for (uint32_t index = 0; index < 5; ++index) {
        struct switch_a3_packet *packet = &switch_packets[index].packet;
        packet->data = switch_frames[index];
        packet->length = 70;
        packet->flags = 0x2000;
        *(uint16_t *)((uint8_t *)packet + 80) = 64;
        switch_frames[index][38] = 0xd0;
        for (uint32_t byte = 0; byte < 6; ++byte) {
            switch_frames[index][42 + byte] = switch_address[byte];
            switch_frames[index][54 + byte] = bssid[byte];
        }
    }
    switch_frames[4][38] = 0x80;
    SWITCH_REQUIRE(!switch_a3_rx_consume(switch_wlc, &switch_packets[4].packet) && !switch_clock_reads &&
                   !switch_timestamp_reads && !switch_beacons && !switch_scheduler_wakes);
    SWITCH_REQUIRE(!switch_a3_rx_configure(switch_wlc, switch_address, 1, 7, 8, 9));
    SWITCH_REQUIRE(!switch_a3_rx_consume(switch_wlc, &switch_packets[4].packet));
#if defined(SWITCH_A3_CANDIDATE) && SWITCH_A3_CANDIDATE
    SWITCH_REQUIRE(switch_clock_reads == 1 && switch_timestamp_reads == 1 && switch_beacons == 1 &&
                   switch_scheduler_wakes == 1);
#else
    SWITCH_REQUIRE(!switch_clock_reads && !switch_timestamp_reads && !switch_beacons && !switch_scheduler_wakes);
#endif
    switch_frames[4][38] = 0xd0;
    for (uint32_t index = 0; index < 4; ++index)
        SWITCH_REQUIRE(switch_a3_rx_consume(switch_wlc, &switch_packets[index].packet) == 1);
    switch_frames[4][54] ^= 1;
    static const uint8_t vendor[7] = {0x7f, 0, 0x17, 0xf2, 8, 0x10, 0};
    volatile uint8_t *body = &switch_frames[4][62];
    for (unsigned i = 0; i < sizeof vendor; ++i)
        body[i] = vendor[i];
    SWITCH_REQUIRE(!switch_a3_rx_consume(switch_wlc, &switch_packets[4].packet) && !switch_original_freed[4]);
#if defined(SWITCH_A3_CANDIDATE) && SWITCH_A3_CANDIDATE
    SWITCH_REQUIRE(switch_queued == 3 && switch_allocated == 3 && switch_a3_rx_pending() == 3 &&
                   switch_original_freed[3] == 1);
#else
    SWITCH_REQUIRE(switch_queued == 4 && switch_allocated == 4 && switch_a3_rx_pending() == 4 &&
                   !switch_original_freed[3]);
#endif
    SWITCH_REQUIRE(!switch_a3_rx_configure(switch_wlc, NULL, 0, 0, 0, 0));
    SWITCH_REQUIRE(!switch_a3_rx_configure(switch_wlc, switch_address, 1, 7, 8, 10));
#if defined(SWITCH_A3_CANDIDATE) && SWITCH_A3_CANDIDATE
    SWITCH_REQUIRE(switch_a3_rx_consume(switch_wlc, &switch_packets[3].packet) == 1);
    SWITCH_REQUIRE(switch_queued == 3 && switch_a3_rx_pending() == 3 && switch_original_freed[3] == 2);
#endif
    switch_complete(0);
#if (DEVELOPMENT || DEBUG) && defined(SWITCH_A3_CANDIDATE) && SWITCH_A3_CANDIDATE
    SWITCH_REQUIRE(!switch_a3_diagnostic.counters[SWITCH_A3_DIAG_RX_FETCH_FAILED]);
#endif
#if defined(SWITCH_A3_CANDIDATE) && SWITCH_A3_CANDIDATE
    SWITCH_REQUIRE(switch_a3_rx_pending() == 2);
    SWITCH_REQUIRE(switch_a3_rx_consume(switch_wlc, &switch_packets[3].packet) == 1);
    SWITCH_REQUIRE(switch_queued == 4 && switch_a3_rx_pending() == 3);
#endif
    for (uint32_t index = 1; index < switch_queued; ++index)
        switch_complete(index);
    SWITCH_REQUIRE(!switch_fault && !switch_a3_rx_pending() && switch_freed == 4 && switch_original_freed[0] == 1 &&
                   switch_original_freed[1] == 1 && switch_original_freed[2] == 1);
    switch_rx_limit_test_results[2] = switch_freed;
    switch_frames[4][54] ^= 1;
    switch_frames[4][68] = 3;
    switch_packets[4].packet.flags = 0;
    switch_linear_enabled = 1;
    SWITCH_REQUIRE(switch_a3_rx_consume(switch_wlc, &switch_packets[4].packet) == 1 &&
                   switch_exports == 1 && !switch_fault);
#if (DEVELOPMENT || DEBUG) && defined(SWITCH_A3_CANDIDATE) && SWITCH_A3_CANDIDATE
    SWITCH_REQUIRE(switch_a3_diagnostic.counters[SWITCH_A3_DIAG_RX_HOOK] == 9 &&
                   switch_a3_diagnostic.counters[SWITCH_A3_DIAG_RX_MATCH] == 7 &&
                   switch_a3_diagnostic.counters[SWITCH_A3_DIAG_RX_HEADER] == 9 &&
                   switch_a3_diagnostic.counters[SWITCH_A3_DIAG_RX_PHY] == 9 &&
                   switch_a3_diagnostic.counters[SWITCH_A3_DIAG_RX_ACTION] == 8 &&
                   switch_a3_diagnostic.counters[SWITCH_A3_DIAG_RX_BSSID] == 7 &&
                   switch_a3_diagnostic.counters[SWITCH_A3_DIAG_RX_DEST] == 7 &&
                   switch_a3_diagnostic.counters[SWITCH_A3_DIAG_RX_PSF] == 1 &&
                   switch_a3_diagnostic.counters[SWITCH_A3_DIAG_RX_MIF] == 1 &&
                   switch_a3_diagnostic.counters[SWITCH_A3_DIAG_RX_SPLIT] == 6 &&
                   switch_a3_diagnostic.counters[SWITCH_A3_DIAG_RX_DROP] == 2 &&
                   switch_a3_diagnostic.counters[SWITCH_A3_DIAG_RX_FETCH_FAILED] == 1 &&
                   switch_a3_diagnostic.counters[SWITCH_A3_DIAG_RX_EXPORT] == 1);
#endif
    return 0;
}

void switch_rx_limit_test_run(void)
{
    switch_rx_limit_test_results[0] = switch_test_limit();
    switch_rx_limit_test_done = 0x53574133;
}

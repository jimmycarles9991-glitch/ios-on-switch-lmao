/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "rx.h"
#include "../../awdl/test/switch_apple_frames.h"

struct switch_test_packet {
    uint32_t words[2];
    uint8_t *data;
    uint16_t length, flags;
    uint32_t word;
    uint16_t next, link;
    uint8_t extension[72];
};

struct switch_test_fetch {
    struct switch_test_packet *original;
    uint16_t headroom;
    int16_t host_offset;
    void (*callback)(struct switch_test_packet *, struct switch_test_packet *, void *, int);
    void *context, *next;
};

union switch_test_memory {
    uint64_t alignment;
    uint8_t bytes[2048];
};

static union switch_test_memory switch_wlc, switch_input, switch_output, switch_heap;
static struct switch_test_packet switch_original, switch_new;
static uint32_t switch_band[4], switch_osh[4], switch_wl[4];
static const uint8_t switch_address[6] = {2, 3, 4, 5, 6, 7};
static const uint8_t switch_bssid[6] = {0, 0x25, 0, 0xff, 0x94, 0x73};
static struct switch_test_fetch *switch_pending;
static const uint8_t *switch_fetched_bytes;
static uint32_t switch_free_original, switch_free_new, switch_heap_free, switch_allocs, switch_delivered;
static uint32_t switch_fault, switch_oom, switch_queue_full, switch_immediate, switch_tsf_calls;
static uint32_t switch_original_calls;
uint32_t switch_rx_test_results[31];
volatile uint32_t switch_rx_test_done;
extern uint32_t switch_rx_test_registers(void *wlc, void *packet);

static void switch_copy(void *output, const void *input, size_t count)
{
    uint8_t *out = output;
    const uint8_t *in = input;
    for (size_t index = 0; index < count; ++index)
        out[index] = in[index];
}

static void switch_zero(void *output, size_t count)
{
    uint8_t *out = output;
    for (size_t index = 0; index < count; ++index)
        out[index] = 0;
}

static int switch_equal(const void *left, const void *right, size_t count)
{
    const uint8_t *a = left, *b = right;
    for (size_t index = 0; index < count; ++index)
        if (a[index] != b[index])
            return 0;
    return 1;
}

#define SWITCH_REQUIRE(condition) \
    do {                          \
        if (!(condition))         \
            return __LINE__;      \
    } while (0)

static void switch_put16(void *address, uint16_t value)
{
    uint8_t *bytes = address;
    bytes[0] = value;
    bytes[1] = value >> 8;
}

static void switch_reset(uint16_t prefix, uint16_t body)
{
    switch_a3_rx_configure(0, 0, 0, 0, 0, 0);
    switch_zero(&switch_wlc, sizeof switch_wlc);
    switch_zero(&switch_input, sizeof switch_input);
    switch_zero(&switch_output, sizeof switch_output);
    switch_zero(&switch_original, sizeof switch_original);
    switch_zero(&switch_new, sizeof switch_new);
    *(void **)(switch_wlc.bytes + 4) = switch_osh;
    *(void **)(switch_wlc.bytes + 8) = switch_wl;
    *(void **)(switch_wlc.bytes + 32) = switch_band;
    *(uint32_t *)(switch_wlc.bytes + 0x5f0) = 32;
    switch_band[2] = 0;
    switch_original.data = switch_input.bytes;
    switch_original.length = 38 + prefix;
    switch_original.flags = body ? 0x2000 : 0;
    switch_put16((uint8_t *)&switch_original + 80, body);
    switch_put16(switch_input.bytes + 22, 0xd024);
    switch_input.bytes[28] = (uint8_t)-42;
    uint8_t *frame = switch_input.bytes + 38;
    for (uint16_t index = 0; index < prefix; ++index)
        frame[index] = (uint8_t)(index + 17);
    switch_put16(frame, 0xd0);
    switch_copy(frame + 4, switch_address, 6);
    switch_copy(frame + 16, switch_bssid, 6);
    switch_pending = 0;
    switch_fetched_bytes = 0;
    switch_free_original = switch_free_new = switch_heap_free = switch_allocs = switch_delivered = 0;
    switch_fault = switch_oom = switch_queue_full = switch_immediate = switch_tsf_calls = 0;
    switch_original_calls = 0;
    switch_a3_rx_configure(switch_wlc.bytes, switch_address, 1, 7, 8, 9);
}

void *switch_a3_packet_get(void *osh, uint32_t length)
{
    ++switch_allocs;
    if (osh != switch_osh || length > SWITCH_A3_RX_BUFFER_BYTES)
        switch_fault = 1;
    if (switch_oom)
        return 0;
    switch_new.data = switch_output.bytes + 8;
    switch_new.length = length;
    switch_output.bytes[7] = 0xa5;
    switch_output.bytes[8 + SWITCH_A3_RX_BUFFER_BYTES] = 0x5a;
    return &switch_new;
}

void switch_a3_packet_free(void *osh, void *packet, int send)
{
    if (osh != switch_osh || send)
        switch_fault = 2;
    if (packet == &switch_original)
        ++switch_free_original;
    else if (packet == &switch_new)
        ++switch_free_new;
    else
        switch_fault = 3;
}

void *switch_a3_malloc_zero(uint32_t length)
{
    ++switch_allocs;
    if (length > sizeof switch_heap)
        switch_fault = 4;
    if (switch_oom)
        return 0;
    switch_zero(&switch_heap, sizeof switch_heap);
    return switch_heap.bytes;
}

void switch_a3_free(void *pointer)
{
    if (pointer != switch_heap.bytes)
        switch_fault = 5;
    ++switch_heap_free;
    switch_zero(&switch_heap, sizeof switch_heap);
}

static void switch_complete(int cancelled)
{
    struct switch_test_fetch *request = switch_pending;
    const uint16_t body = *(uint16_t *)((uint8_t *)request->original + 80);
    switch_new.data = switch_output.bytes + 8 + request->headroom;
    switch_new.length = body;
    switch_output.bytes[7] = 0xa5;
    switch_output.bytes[8 + SWITCH_A3_RX_BUFFER_BYTES] = 0x5a;
    for (uint16_t index = 0; index < body; ++index)
        switch_new.data[index] = switch_fetched_bytes ? switch_fetched_bytes[index] : (uint8_t)(0xf0u + index);
    switch_pending = 0;
    request->callback(&switch_new, request->original, request->context, cancelled);
}

int switch_a3_packet_fetch(struct switch_test_fetch *request)
{
    if (request->original != &switch_original || request->host_offset || request->next ||
        request->context != switch_heap.bytes || request->headroom < 56)
        switch_fault = 6;
    if (switch_queue_full)
        return -1;
    switch_pending = request;
    if (switch_immediate)
        switch_complete(0);
    return 0;
}

void switch_a3_raw_sendup(void *wl, void *interface, struct switch_test_packet *packet)
{
    if (wl != switch_wl || interface || packet != &switch_new || !(packet->flags & 0x80u))
        switch_fault = 7;
    ++switch_delivered;
}

void switch_a3_tsf_read(void *wlc, uint32_t *low, uint32_t *high)
{
    if (wlc != switch_wlc.bytes)
        switch_fault = 8;
    *low = 2;
    *high = 4;
    switch_tsf_calls |= 1;
}

void switch_a3_tsf_receive(void *wlc, const void *status, uint32_t *high, uint32_t *low)
{
    if (wlc != switch_wlc.bytes || status != switch_input.bytes || *low != 2 || *high != 4)
        switch_fault = 9;
    *low = 0xfffffff0u;
    *high = 3;
    switch_tsf_calls |= 2;
}

void switch_a3_rx_original(void *wlc, void *packet)
{
    if (wlc != switch_wlc.bytes || packet != &switch_original)
        switch_fault = 10;
    ++switch_original_calls;
}

static uint32_t switch_check_delivery(uint16_t prefix, uint16_t body)
{
    struct switch_a3_raw_rx header;
    switch_copy(&header, switch_new.data, sizeof header);
    SWITCH_REQUIRE(!switch_fault && switch_free_original == 1 && !switch_free_new && switch_delivered == 1);
    SWITCH_REQUIRE(switch_new.data == switch_output.bytes + 8 + SWITCH_A3_RX_BUS_HEADROOM);
    SWITCH_REQUIRE(switch_new.length == sizeof header + prefix + body);
    SWITCH_REQUIRE(header.magic == SWITCH_A3_RX_MAGIC && header.version == SWITCH_A3_RX_VERSION);
    SWITCH_REQUIRE(header.header_bytes == 40 && header.frame_bytes == prefix + body && !header.reserved);
    SWITCH_REQUIRE(header.chanspec == 0xd024 && header.rssi == -42 && header.flags == SWITCH_A3_RX_FCS_PRESENT);
    SWITCH_REQUIRE(header.session == 7 && header.clock_epoch == 8 && header.config_generation == 9 &&
                   !header.reserved2);
    SWITCH_REQUIRE(header.tsf == UINT64_C(0x3fffffff0) && switch_tsf_calls == 3);
    const uint16_t skip = (switch_input.bytes[16] & 4u) ? 40 : 38;
    SWITCH_REQUIRE(switch_equal(switch_new.data + sizeof header, switch_input.bytes + skip, prefix));
    for (uint16_t index = 0; index < body; ++index)
        SWITCH_REQUIRE(switch_new.data[sizeof header + prefix + index] == (uint8_t)(0xf0u + index));
    SWITCH_REQUIRE(switch_output.bytes[7] == 0xa5 && switch_output.bytes[8 + SWITCH_A3_RX_BUFFER_BYTES] == 0x5a);
    return 0;
}

static uint32_t switch_test_linear(void)
{
    switch_reset(64, 0);
    SWITCH_REQUIRE(switch_a3_rx_consume(switch_wlc.bytes, &switch_original) == 1);
    SWITCH_REQUIRE(!switch_heap_free);
    return switch_check_delivery(64, 0);
}

static uint32_t switch_test_split(void)
{
    switch_reset(28, 100);
    SWITCH_REQUIRE(switch_a3_rx_consume(switch_wlc.bytes, &switch_original) == 1);
    SWITCH_REQUIRE(switch_a3_rx_pending() == 1);
    SWITCH_REQUIRE(switch_pending && !switch_free_original && !switch_delivered && !switch_heap_free);
    switch_complete(0);
    SWITCH_REQUIRE(switch_heap_free == 1 && !switch_a3_rx_pending());
    return switch_check_delivery(28, 100);
}

static uint32_t switch_test_immediate(void)
{
    switch_reset(28, 100);
    switch_immediate = 1;
    SWITCH_REQUIRE(switch_a3_rx_consume(switch_wlc.bytes, &switch_original) == 1);
    SWITCH_REQUIRE(!switch_pending && switch_heap_free == 1);
    return switch_check_delivery(28, 100);
}

static uint32_t switch_test_failed_fetch(void)
{
    switch_reset(28, 100);
    switch_queue_full = 1;
    SWITCH_REQUIRE(switch_a3_rx_consume(switch_wlc.bytes, &switch_original) == 1);
    SWITCH_REQUIRE(!switch_fault && switch_free_original == 1 && !switch_free_new && switch_heap_free == 1);
    SWITCH_REQUIRE(!switch_pending && !switch_delivered);
    return 0;
}

static uint32_t switch_test_cancel(void)
{
    switch_reset(28, 100);
    SWITCH_REQUIRE(switch_a3_rx_consume(switch_wlc.bytes, &switch_original) == 1);
    switch_complete(1);
    SWITCH_REQUIRE(!switch_fault && switch_free_original == 1 && switch_free_new == 1 && switch_heap_free == 1);
    SWITCH_REQUIRE(!switch_delivered);
    return 0;
}

static uint32_t switch_test_generation(void)
{
    switch_reset(28, 100);
    SWITCH_REQUIRE(switch_a3_rx_consume(switch_wlc.bytes, &switch_original) == 1);
    SWITCH_REQUIRE(!switch_a3_rx_configure(switch_wlc.bytes, switch_address, 1, 7, 8, 9));
    switch_complete(0);
    SWITCH_REQUIRE(!switch_fault && switch_free_original == 1 && switch_free_new == 1 && switch_heap_free == 1);
    SWITCH_REQUIRE(!switch_delivered);
    return 0;
}

static uint32_t switch_test_bound(void)
{
    switch_reset(28, 1836);
    SWITCH_REQUIRE(switch_a3_rx_consume(switch_wlc.bytes, &switch_original) == 1);
    switch_complete(0);
    return switch_check_delivery(28, 1836);
}

static uint32_t switch_test_oom(void)
{
    for (uint16_t split = 0; split < 2; ++split) {
        switch_reset(28, split ? 100 : 0);
        switch_oom = 1;
        SWITCH_REQUIRE(switch_a3_rx_consume(switch_wlc.bytes, &switch_original) == 1);
        SWITCH_REQUIRE(!switch_fault && switch_free_original == 1 && !switch_free_new && !switch_delivered);
        SWITCH_REQUIRE(!switch_heap_free && !switch_pending);
    }
    return 0;
}

static uint32_t switch_test_declines(uint32_t variant)
{
    uint8_t packet_before[sizeof switch_original], data_before[128];
    switch_reset(28, 0);
    switch (variant) {
    case 0:
        switch_a3_rx_configure(0, 0, 0, 0, 0, 0);
        break;
    case 1:
        switch_original.length = 16;
        break;
    case 2:
        *(uint32_t *)(switch_wlc.bytes + 0x5f0) = 0xffffffffu;
        break;
    case 3:
        switch_original.next = 1;
        break;
    case 4:
        switch_input.bytes[38 + 16] ^= 1;
        break;
    case 5:
        switch_input.bytes[38 + 4] ^= 2;
        break;
    case 6:
        switch_input.bytes[16] = 1;
        break;
    case 7:
        switch_input.bytes[4] = 0x10;
        break;
    case 8:
        switch_put16(switch_input.bytes + 38, 0x1d0);
        break;
    case 9:
        switch_put16(switch_input.bytes + 38, 0x80);
        break;
    case 10:
        switch_original.flags = 0x2000;
        switch_put16((uint8_t *)&switch_original + 80, 1837);
        break;
    case 11:
        switch_original.flags = 0x2000;
        switch_put16((uint8_t *)&switch_original + 84, 1);
        break;
    case 12:
        switch_original.link = 1;
        break;
    }
    switch_copy(packet_before, &switch_original, sizeof packet_before);
    switch_copy(data_before, switch_input.bytes, sizeof data_before);
    switch_a3_rx_entry(switch_wlc.bytes, &switch_original);
    SWITCH_REQUIRE(!switch_fault && switch_original_calls == 1);
    SWITCH_REQUIRE(!switch_allocs && !switch_tsf_calls && !switch_free_original && !switch_delivered);
    SWITCH_REQUIRE(switch_equal(packet_before, &switch_original, sizeof packet_before));
    SWITCH_REQUIRE(switch_equal(data_before, switch_input.bytes, sizeof data_before));
    return 0;
}

static uint32_t switch_test_phy_quirk(void)
{
    switch_reset(28, 0);
    switch_put16(switch_input.bytes + 4, 0x100);
    switch_band[2] = 10;
    switch_a3_rx_entry(switch_wlc.bytes, &switch_original);
    SWITCH_REQUIRE(!switch_original_calls && switch_input.bytes[5] == 1);
    return switch_check_delivery(28, 0);
}

static uint32_t switch_test_address(void)
{
    static const uint8_t zero[6] = {0}, multicast[6] = {1, 2, 3, 4, 5, 6};
    switch_reset(28, 0);
    SWITCH_REQUIRE(switch_a3_rx_configure(0, switch_address, 1, 7, 8, 9) == -2);
    SWITCH_REQUIRE(switch_a3_rx_configure(switch_wlc.bytes, zero, 1, 7, 8, 9) == -2);
    SWITCH_REQUIRE(switch_a3_rx_configure(switch_wlc.bytes, multicast, 1, 7, 8, 9) == -2);
    SWITCH_REQUIRE(switch_a3_rx_configure(switch_wlc.bytes, 0, 1, 7, 8, 9) == -2);
    switch_input.bytes[42] = 0xff;
    SWITCH_REQUIRE(switch_a3_rx_consume(switch_wlc.bytes, &switch_original) == 1);
    return switch_check_delivery(28, 0);
}

static uint32_t switch_test_short_descriptor(void)
{
    switch_reset(28, 0);
    switch_put16((uint8_t *)&switch_original + 80, 0xffff);
    switch_put16((uint8_t *)&switch_original + 84, 0xffff);
    SWITCH_REQUIRE(switch_rx_test_registers(switch_wlc.bytes, &switch_original));
    return switch_check_delivery(28, 0);
}

static uint32_t switch_test_padding(void)
{
    switch_reset(28, 0);
    for (uint16_t index = 28; index; --index)
        switch_input.bytes[40 + index - 1] = switch_input.bytes[38 + index - 1];
    switch_original.length += 2;
    switch_input.bytes[16] = 4;
    switch_put16(switch_input.bytes + 40, 0x88);
    SWITCH_REQUIRE(switch_a3_rx_consume(switch_wlc.bytes, &switch_original) == 1);
    return switch_check_delivery(28, 0);
}

static uint32_t switch_test_disabled_callback(void)
{
    switch_reset(28, 100);
    SWITCH_REQUIRE(switch_a3_rx_consume(switch_wlc.bytes, &switch_original) == 1);
    switch_a3_rx_configure(0, 0, 0, 0, 0, 0);
    switch_complete(0);
    SWITCH_REQUIRE(!switch_fault && switch_free_original == 1 && switch_free_new == 1 && switch_heap_free == 1);
    SWITCH_REQUIRE(!switch_delivered);
    return 0;
}

static uint32_t switch_test_fallback_registers(void)
{
    switch_reset(28, 0);
    switch_a3_rx_configure(0, 0, 0, 0, 0, 0);
    SWITCH_REQUIRE(switch_rx_test_registers(switch_wlc.bytes, &switch_original));
    SWITCH_REQUIRE(!switch_fault && switch_original_calls == 1 && !switch_free_original && !switch_allocs);
    return 0;
}

static uint32_t switch_test_apple_action(const uint8_t *frame, uint16_t length, uint16_t prefix)
{
    switch_reset(prefix, length - prefix);
    switch_put16(switch_input.bytes + 22, 0xd02c);
    switch_copy(switch_input.bytes + 38, frame, prefix);
    switch_fetched_bytes = frame + prefix;
    SWITCH_REQUIRE(switch_a3_rx_consume(switch_wlc.bytes, &switch_original) == 1);
    if (prefix < length) {
        SWITCH_REQUIRE(switch_pending && !switch_delivered && !switch_free_original);
        switch_complete(0);
    }
    struct switch_a3_raw_rx header;
    switch_copy(&header, switch_new.data, sizeof header);
    SWITCH_REQUIRE(!switch_fault && switch_delivered == 1 && switch_free_original == 1 && !switch_free_new);
    SWITCH_REQUIRE(header.chanspec == 0xd02c && header.flags == SWITCH_A3_RX_FCS_PRESENT);
    SWITCH_REQUIRE(header.session == 7 && header.clock_epoch == 8 && header.config_generation == 9);
    SWITCH_REQUIRE(header.tsf == UINT64_C(0x3fffffff0) && header.frame_bytes == length);
    SWITCH_REQUIRE(switch_new.length == sizeof header + length);
    SWITCH_REQUIRE(switch_equal(switch_new.data + sizeof header, frame, length));
    return 0;
}

void switch_rx_test_run(void)
{
    switch_rx_test_results[0] = switch_test_linear();
    switch_rx_test_results[1] = switch_test_split();
    switch_rx_test_results[2] = switch_test_immediate();
    switch_rx_test_results[3] = switch_test_failed_fetch();
    switch_rx_test_results[4] = switch_test_cancel();
    switch_rx_test_results[5] = switch_test_generation();
    switch_rx_test_results[6] = switch_test_bound();
    switch_rx_test_results[7] = switch_test_oom();
    for (uint32_t index = 0; index < 13; ++index)
        switch_rx_test_results[8 + index] = switch_test_declines(index);
    switch_rx_test_results[21] = switch_test_phy_quirk();
    switch_rx_test_results[22] = switch_test_address();
    switch_rx_test_results[23] = switch_test_short_descriptor();
    switch_rx_test_results[24] = switch_test_padding();
    switch_rx_test_results[25] = switch_test_disabled_callback();
    switch_rx_test_results[26] = switch_test_fallback_registers();
    switch_rx_test_results[27] = switch_test_apple_action(switch_apple_psf, sizeof switch_apple_psf, sizeof switch_apple_psf);
    switch_rx_test_results[28] = switch_test_apple_action(switch_apple_mif, sizeof switch_apple_mif, sizeof switch_apple_mif);
    switch_rx_test_results[29] = switch_test_apple_action(switch_apple_psf, sizeof switch_apple_psf, 29);
    switch_rx_test_results[30] = switch_test_apple_action(switch_apple_mif, sizeof switch_apple_mif, 29);
    switch_rx_test_done = 0x53574133;
}

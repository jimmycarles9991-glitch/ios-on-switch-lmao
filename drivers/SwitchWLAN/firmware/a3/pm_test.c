/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "channel.h"
#include "diagnostic.h"
#include <stddef.h>

static uint32_t switch_wlc[512], switch_cfg[256], switch_pub[64], switch_scb[128], switch_private[160];
static uint32_t switch_pm[40], switch_scan[8], switch_queue[8], switch_info[8], switch_stats[64], switch_table[8];
static uint32_t switch_packet[24], switch_status[16];
static uint8_t switch_storage[512];
const uint8_t *switch_pm_test_header = switch_storage + 202;
static struct switch_a3_channel_binding switch_binding;
static uint32_t switch_allocations, switch_enqueues, switch_frees, switch_timers, switch_pm2, switch_suspend;
static uint32_t switch_variant, switch_fault;
static uint64_t switch_now;
volatile uint32_t switch_pm_test_results[35], switch_pm_test_done, switch_pm_test_original;

extern int switch_pm_test_prepare(void *context);
extern void switch_pm_test_status(void *wlc, void *cfg, void *packet, void *status);

#define SWITCH_REQUIRE(condition) do { if (!(condition)) return __LINE__; } while (0)

static void switch_zero(void *memory, size_t bytes)
{
    uint8_t *out = memory;
    for (size_t index = 0; index < bytes; ++index)
        out[index] = 0;
}

static void switch_pointer(void *memory, size_t offset, const void *value)
{
    *(const void **)((uint8_t *)memory + offset) = value;
}

static void switch_byte(void *memory, size_t offset, uint8_t value)
{
    *((uint8_t *)memory + offset) = value;
}

static void switch_short(void *memory, size_t offset, uint16_t value)
{
    *(uint16_t *)((uint8_t *)memory + offset) = value;
}

static uint32_t switch_word(const void *memory, size_t offset)
{
    return *(const uint32_t *)((const uint8_t *)memory + offset);
}

void *switch_pm_test_copy(void *destination, const void *source, size_t bytes)
{
    uint8_t *out = destination;
    const uint8_t *in = source;
    for (size_t index = 0; index < bytes; ++index)
        out[index] = in[index];
    return destination;
}

uint32_t switch_pm_test_zero(void) { return 0; }
uint32_t switch_pm_test_one(void) { return 1; }
void switch_pm_test_noop(void) {}

void *switch_pm_test_scb(void *wlc, void *cfg, const uint8_t *address)
{
    if (wlc != switch_wlc || cfg != switch_cfg || address != (uint8_t *)switch_cfg + 216)
        switch_fault = 1;
    return switch_scb;
}

void *switch_pm_test_allocate(void *osh, uint32_t bytes)
{
    if (osh != switch_wlc || bytes > sizeof switch_storage)
        switch_fault = 2;
    ++switch_allocations;
    if (switch_variant == 4)
        return NULL;
    switch_zero(switch_packet, sizeof switch_packet);
    if (switch_variant == 31)
        switch_packet[6] = 0x400;
    switch_pointer(switch_packet, 8, switch_storage);
    return switch_packet;
}

static void switch_complete(uint32_t status, void *packet)
{
    switch_zero(switch_status, sizeof switch_status);
    switch_byte(switch_status, 21, (status & 0x40) != 0);
    switch_byte(switch_status, 22, (status & 0x80) != 0);
    switch_byte(switch_status, 24, (status >> 2) & 7);
    switch_byte(switch_status, 25, (status & 2) != 0);
    switch_pm_test_status(switch_wlc, switch_cfg, packet, switch_status);
}

int switch_pm_test_enqueue(void *wlc, void *queue, void *packet, uint32_t priority)
{
    (void)priority;
    if (wlc != switch_wlc || queue != (uint8_t *)switch_queue + 4 || packet != switch_packet ||
        switch_word(switch_packet, 40) != (uintptr_t)switch_scb)
        switch_fault = 3;
    const uint8_t *software = (const uint8_t *)(uintptr_t)switch_word(packet, 8);
    for (unsigned index = 0; index < 16; ++index)
        if (software[index] != (index == 4))
            switch_fault = 4;
    ++switch_enqueues;
    if (switch_variant == 28 || switch_variant == 33)
        switch_complete(0x82, switch_packet);
    return switch_variant != 5 && switch_variant != 28 && switch_variant != 33;
}

void switch_pm_test_drain(void *wlc, void *queue)
{
    if (wlc != switch_wlc || queue != switch_queue)
        switch_fault = 5;
    if (switch_variant == 3)
        switch_complete(0x82, switch_packet);
    if (switch_variant == 25)
        switch_a3_channel_complete();
}

void switch_pm_test_free(void *osh, void *packet, uint32_t send)
{
    if (osh != switch_wlc || packet != switch_packet || send != 1)
        switch_fault = 6;
    ++switch_frees;
}

void switch_pm_test_timer(void *osh, void *timer, uint32_t delay, uint32_t periodic)
{
    if (osh != switch_wlc || timer != switch_private || periodic || (delay && delay != 3))
        switch_fault = 7;
    ++switch_timers;
}

void switch_pm_test_suspend(void *wlc)
{
    if (wlc != switch_wlc)
        switch_fault = 8;
    ++switch_suspend;
}

void switch_pm_test_enable(void *wlc)
{
    if (wlc != switch_wlc || !switch_suspend)
        switch_fault = 9;
    --switch_suspend;
}

void switch_pm_test_home(void *wlc, uint32_t channel)
{
    if (wlc != switch_wlc || channel != 0x1006)
        switch_fault = 10;
    switch_short(wlc, 0x468, channel);
}

uint32_t switch_pm_test_pm2_eligible(void)
{
    return switch_variant == 24;
}

void switch_pm_test_pm2(void *cfg, uint32_t enabled)
{
    if (cfg != switch_cfg || enabled != 1)
        switch_fault = 11;
    ++switch_pm2;
}

int switch_a3_clock_read(void *wlc, uint64_t *tsf)
{
    if (wlc != switch_wlc)
        switch_fault = 12;
    *tsf = switch_now;
    return 0;
}

int switch_a3_channel_valid(void *manager, uint32_t channel)
{
    return manager == switch_cfg && channel == 0xd02c;
}

int switch_a3_channel_restricted(void *manager, uint32_t channel)
{
    (void)manager;
    (void)channel;
    return 0;
}

void switch_a3_scan_abort(void *scan, uint32_t status)
{
    if (scan != switch_scan || status != 4)
        switch_fault = 13;
    switch_a3_channel_complete();
}

static int switch_reset(uint32_t variant)
{
    switch_a3_channel_complete();
    switch_zero(switch_wlc, sizeof switch_wlc);
    switch_zero(switch_cfg, sizeof switch_cfg);
    switch_zero(switch_pub, sizeof switch_pub);
    switch_zero(switch_scb, sizeof switch_scb);
    switch_zero(switch_private, sizeof switch_private);
    switch_zero(switch_pm, sizeof switch_pm);
    switch_zero(switch_scan, sizeof switch_scan);
    switch_zero(switch_storage, sizeof switch_storage);
    switch_zero(switch_table, sizeof switch_table);
    switch_zero(&switch_a3_diagnostic, sizeof switch_a3_diagnostic);
    switch_variant = variant;
    switch_allocations = switch_enqueues = switch_frees = switch_timers = switch_pm2 = switch_suspend = switch_fault = 0;
    switch_now = 1000;
    switch_pointer(switch_wlc, 0, switch_pub);
    switch_pointer(switch_wlc, 4, switch_wlc);
    switch_pointer(switch_wlc, 8, switch_wlc);
    switch_pointer(switch_wlc, 0x168, switch_scan);
    switch_pointer(switch_wlc, 0x15c, switch_cfg);
    switch_pointer(switch_wlc, 0x268, switch_table);
    switch_pointer(switch_wlc, 0x26c, switch_cfg);
    switch_pointer(switch_table, 0, switch_cfg);
    switch_byte(switch_wlc, 25, 1);
    switch_short(switch_wlc, 0x466, 0x1006);
    switch_short(switch_wlc, 0x468, 0x1006);
    switch_pointer(switch_pub, 0x88, switch_stats);
    switch_byte(switch_pub, 24, 1);
    switch_pointer(switch_cfg, 0, switch_wlc);
    switch_pointer(switch_cfg, 12, switch_info);
    switch_pointer(switch_info, 12, switch_queue);
    switch_pointer(switch_cfg, 0x120, switch_pm);
    switch_pointer(switch_cfg, 0x360, switch_info);
    switch_byte(switch_cfg, 8, 1);
    switch_byte(switch_cfg, 16, 1);
    switch_byte(switch_cfg, 17, 1);
    switch_byte(switch_cfg, 68, 1);
    for (unsigned index = 0; index < 6; ++index) {
        switch_byte(switch_cfg, 216 + index, 2 + index);
        switch_byte(switch_cfg, 222 + index, 8 + index);
    }
    switch_pointer(switch_scb, 16, switch_cfg);
    switch_pointer(switch_scan, 0, switch_private);
    switch_pointer(switch_private, 0, switch_scan);
    switch_pointer(switch_private, 8, switch_wlc);
    switch_pointer(switch_private, 0x1f8, switch_private);
    switch_byte(switch_private, 0x240, 3);
    switch_binding = (struct switch_a3_channel_binding){
        .wlc = switch_wlc, .bsscfg = switch_cfg,
        .completion = 0x10001, .entered = 0x10021,
        .completion_context = (void *)(uintptr_t)(variant + 1),
        .entered_context = (void *)(uintptr_t)(variant + 1),
        .expiry_tsf = 5000, .session = 7, .clock_epoch = 8, .generation = 9,
        .home_chanspec = 0x1006, .target_chanspec = 0xd02c,
    };
    if (switch_a3_channel_claim(&switch_binding))
        return __LINE__;
    switch_pointer(switch_private, 0x1fc, (void *)switch_binding.completion);
    switch_pointer(switch_private, 0x200, switch_binding.completion_context);
    switch_pointer(switch_private, 0x22c, (void *)switch_binding.entered);
    switch_pointer(switch_private, 0x230, switch_binding.entered_context);
    switch_byte(switch_scan, 6, 1);
    return 0;
}

static uint32_t switch_case(uint32_t variant)
{
    SWITCH_REQUIRE(!switch_reset(variant));
    if (variant == 1 || variant == 15 || variant == 33)
        switch_byte(switch_pm, 8, 1);
    if (variant == 2)
        switch_scb[1] = 0x10040;
    if (variant == 6)
        switch_byte(switch_wlc, 0x5d8, 1);
    if (variant == 14 || variant == 15)
        switch_pointer(switch_private, 0x200, NULL);
    if (variant == 16)
        switch_wlc[0x548 / 4] = 1;
    if (variant == 17)
        switch_byte(switch_cfg, 8, 0);
    if (variant == 18)
        switch_byte(switch_cfg, 216, 3);
    switch_short(switch_scan, 4, 0x800);
    const int waiting = switch_pm_test_prepare(switch_private);
    switch_short(switch_scan, 4, *(uint16_t *)((uint8_t *)switch_scan + 4) & ~0x800u);
    SWITCH_REQUIRE(waiting >= 0 && !switch_fault);
    if (variant == 3)
        SWITCH_REQUIRE(!waiting && !switch_timers);
    if (variant == 15 || variant == 16 || variant == 17) {
        SWITCH_REQUIRE(!switch_allocations && !switch_enqueues && !switch_a3_diagnostic.pm_requests);
        return 0;
    }
    if (variant == 28 || variant == 33) {
        SWITCH_REQUIRE(switch_allocations == 1 && switch_enqueues == 1 && switch_frees == 1);
        SWITCH_REQUIRE(switch_a3_diagnostic.pm_requests == 1 && switch_a3_diagnostic.pm_packets == 1);
        SWITCH_REQUIRE(switch_a3_diagnostic.pm_acks == 1 && !switch_a3_diagnostic.pm_failed);
        SWITCH_REQUIRE(switch_a3_diagnostic.pm_reason == SWITCH_A3_PM_ACKED && switch_a3_diagnostic.pm_status == 0x82);
        SWITCH_REQUIRE(!switch_a3_channel_pm_capture(switch_wlc, switch_packet, switch_scb));
        switch_a3_channel_pm_submitted((uintptr_t)switch_binding.completion_context, 0);
        SWITCH_REQUIRE(switch_a3_channel_may_leave(switch_wlc, 0xd02c));
        SWITCH_REQUIRE(switch_a3_diagnostic.pm_acks == 1 && !switch_a3_diagnostic.pm_failed);
        return 0;
    }
    if (variant == 4 || variant == 5 || variant == 6 || variant == 31) {
        SWITCH_REQUIRE(!switch_a3_channel_may_leave(switch_wlc, 0xd02c));
        SWITCH_REQUIRE(switch_a3_diagnostic.pm_failed == 1);
        SWITCH_REQUIRE(switch_a3_diagnostic.pm_reason == (variant == 5 ?
                           SWITCH_A3_PM_SUBMIT_REFUSED : SWITCH_A3_PM_NO_PACKET));
        SWITCH_REQUIRE(switch_a3_diagnostic.pm_status == SWITCH_A3_PM_STATUS_UNAVAILABLE);
        SWITCH_REQUIRE(!switch_a3_channel_pm_capture(switch_wlc, switch_packet, switch_scb));
        if (variant != 31)
            SWITCH_REQUIRE(!((uint8_t *)switch_pm)[10]);
        SWITCH_REQUIRE(switch_frees == (uint32_t)(variant == 5));
        return 0;
    }
    SWITCH_REQUIRE(switch_allocations == 1 && switch_enqueues == 1 && !switch_frees);
    const uint8_t *header = switch_storage + 202;
    SWITCH_REQUIRE(*(const uint16_t *)header == (variant == 2 ? 0x01c8 : 0x0148));
    SWITCH_REQUIRE(!(switch_word(switch_packet, 24) & 0x400));
    if (variant == 14 || variant == 18) {
        SWITCH_REQUIRE(!switch_a3_diagnostic.pm_requests && !switch_a3_diagnostic.pm_packets);
        switch_complete(0x82, switch_packet);
        SWITCH_REQUIRE(!switch_a3_diagnostic.pm_acks && !((uint8_t *)switch_pm)[10]);
        return 0;
    }
    if (variant == 25) {
        switch_complete(0x82, switch_packet);
        SWITCH_REQUIRE(!switch_a3_diagnostic.pm_acks && !switch_a3_channel_owned());
        return 0;
    }
    SWITCH_REQUIRE(switch_a3_diagnostic.pm_requests == 1 && switch_a3_diagnostic.pm_packets == 1);
    if (variant == 19) {
        switch_pm_test_prepare(switch_private);
        SWITCH_REQUIRE(switch_allocations == 1 && switch_a3_diagnostic.pm_requests == 1);
    }
    if (variant == 20) {
        switch_a3_channel_abort();
        switch_complete(0x82, switch_packet);
        SWITCH_REQUIRE(!switch_a3_diagnostic.pm_acks && !switch_a3_channel_owned());
        SWITCH_REQUIRE(switch_a3_diagnostic.pm_reason == SWITCH_A3_PM_CANCELLED);
        return 0;
    }
    if (variant == 11) {
        switch_complete(0xc2, switch_packet);
        SWITCH_REQUIRE(!switch_a3_diagnostic.pm_acks);
        SWITCH_REQUIRE(switch_a3_diagnostic.pm_reason == SWITCH_A3_PM_PENDING && switch_a3_diagnostic.pm_status == 0xc2);
    }
    if (variant == 12) {
        switch_complete(0x82, switch_scb);
        SWITCH_REQUIRE(!switch_a3_diagnostic.pm_acks);
    }
    if (variant != 3)
        switch_complete(variant == 7 ? 2 : variant == 8 ? 0x80 : variant == 9 ? 0 : variant == 10 ? 0x14 : 0x82,
                         switch_packet);
    if (variant >= 7 && variant <= 10) {
        SWITCH_REQUIRE(!switch_a3_channel_may_leave(switch_wlc, 0xd02c));
        SWITCH_REQUIRE(!switch_a3_diagnostic.pm_acks && switch_a3_diagnostic.pm_failed == 1);
        SWITCH_REQUIRE(switch_a3_diagnostic.pm_reason == SWITCH_A3_PM_STATUS_REJECTED);
        if (variant != 7)
            SWITCH_REQUIRE(!((uint8_t *)switch_pm)[10]);
        return 0;
    }
    SWITCH_REQUIRE(switch_a3_diagnostic.pm_acks == 1 && !((uint8_t *)switch_pm)[10]);
    SWITCH_REQUIRE(switch_a3_diagnostic.pm_reason == SWITCH_A3_PM_ACKED && switch_a3_diagnostic.pm_status == 0x82);
    if (variant == 34) {
        switch_a3_channel_abort();
        SWITCH_REQUIRE(!switch_a3_channel_owned());
        SWITCH_REQUIRE(switch_a3_diagnostic.pm_reason == SWITCH_A3_PM_CANCELLED);
        SWITCH_REQUIRE(switch_a3_diagnostic.pm_status == SWITCH_A3_PM_STATUS_UNAVAILABLE);
        return 0;
    }
    if (variant == 13 || variant == 27) {
        ((void (*)(void *, uint32_t))0x193663)(switch_cfg, 0);
        if (variant == 13)
            ((void (*)(void *, uint32_t))0x193663)(switch_cfg, 1);
        SWITCH_REQUIRE(!switch_a3_channel_may_leave(switch_wlc, 0xd02c));
        SWITCH_REQUIRE(switch_a3_diagnostic.pm_reason == SWITCH_A3_PM_INVALIDATED);
        return 0;
    }
    if (variant == 21) {
        switch_now = 5000;
        SWITCH_REQUIRE(!switch_a3_channel_may_leave(switch_wlc, 0xd02c));
        SWITCH_REQUIRE(switch_a3_channel_complete() == SWITCH_A3_CHANNEL_EXPIRED);
        return 0;
    }
    if (variant == 26)
        switch_a3_channel_pm_set(switch_scb, 0);
    if (variant == 32) {
        switch_byte(switch_cfg, 17, 0);
        SWITCH_REQUIRE(!switch_a3_channel_may_leave(switch_wlc, 0xd02c));
        return 0;
    }
    if (variant == 29 || variant == 30) {
        uint8_t other[24];
        switch_pm_test_copy(other, header, sizeof other);
        if (variant == 29) {
            other[1] = 0;
            other[4] ^= 2;
        }
        switch_a3_channel_pm_status(switch_wlc, switch_cfg, switch_scb, *(uint16_t *)other, 0, other);
        if (variant == 30) {
            SWITCH_REQUIRE(!switch_a3_channel_may_leave(switch_wlc, 0xd02c));
            return 0;
        }
    }
    SWITCH_REQUIRE(switch_a3_channel_may_leave(switch_wlc, 0xd02c));
    if (variant >= 22 && variant <= 24) {
        switch_byte(switch_pm, 0, variant == 22 ? 1 : variant == 24 ? 2 : 0);
        ((void (*)(void *))0x1cf7c7)(switch_private);
        SWITCH_REQUIRE(!switch_fault && !switch_suspend && !(switch_word(switch_pm, 0x44) & 1));
        SWITCH_REQUIRE(((uint8_t *)switch_pm)[8] == (variant != 23));
        SWITCH_REQUIRE(switch_pm2 == (variant == 24));
        SWITCH_REQUIRE(switch_allocations == (variant == 23 ? 2u : 1u));
    }
    return switch_fault ? __LINE__ : 0;
}

static uint32_t switch_original_case(uint32_t asleep)
{
    SWITCH_REQUIRE(!switch_reset(0));
    switch_byte(switch_pm, 8, asleep);
    switch_short(switch_scan, 4, 0x800);
    switch_pm_test_prepare(switch_private);
    switch_short(switch_scan, 4, *(uint16_t *)((uint8_t *)switch_scan + 4) & ~0x800u);
    SWITCH_REQUIRE(!switch_fault && switch_allocations == !asleep && switch_enqueues == !asleep);
    SWITCH_REQUIRE(!switch_a3_diagnostic.pm_requests && !switch_a3_diagnostic.pm_packets);
    if (!asleep) {
        SWITCH_REQUIRE(*(const uint16_t *)(switch_storage + 202) == 0x0148);
        switch_complete(0x82, switch_packet);
        SWITCH_REQUIRE(!switch_a3_diagnostic.pm_acks && !((uint8_t *)switch_pm)[10]);
    }
    return 0;
}

void switch_pm_test_run(void)
{
    if (switch_pm_test_original) {
        switch_pm_test_results[0] = switch_original_case(0);
        switch_pm_test_results[1] = switch_original_case(1);
    } else {
        for (uint32_t variant = 0; variant < 35; ++variant)
            switch_pm_test_results[variant] = switch_case(variant);
    }
    switch_pm_test_done = 0x53574133;
}

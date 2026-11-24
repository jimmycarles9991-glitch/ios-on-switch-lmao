/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "channel.h"
#include "diagnostic.h"
#include "wire.h"
#include <stddef.h>

static uint64_t switch_wlc[192], switch_cfg[40], switch_scan[8], switch_private[96];
static struct switch_a3_channel_binding switch_binding;
static uint64_t switch_now;
static int switch_clock_failed, switch_abort_async;
static uint16_t switch_restricted, switch_invalid;
static uint32_t switch_original_calls, switch_cleanup_calls, switch_abort_calls, switch_fault, switch_completion;
static uint32_t switch_suspend_depth, switch_resume_calls;
static const uint8_t switch_ap[6] = {2, 1, 2, 3, 4, 5};
static uint8_t switch_header[24];
static uint32_t switch_pm[32], switch_packet[16], switch_scb[8];
volatile uint32_t switch_channel_test_results[26];
volatile uint32_t switch_channel_test_done;
extern void switch_channel_test_invoke(void *wlc, uint32_t chanspec);

#define SWITCH_REQUIRE(condition) \
    do {                          \
        if (!(condition))         \
            return __LINE__;      \
    } while (0)

static void switch_zero(void *memory, size_t count)
{
    uint8_t *bytes = memory;
    for (size_t index = 0; index < count; ++index)
        bytes[index] = 0;
}

static void switch_pointer(void *base, size_t offset, const void *value)
{
    *(const void **)((uint8_t *)base + offset) = value;
}

static void switch_word16(void *base, size_t offset, uint16_t value)
{
    uint8_t *bytes = (uint8_t *)base + offset;
    bytes[0] = value;
    bytes[1] = value >> 8;
}

int switch_a3_clock_read(void *wlc, uint64_t *tsf)
{
    if (wlc != switch_wlc)
        switch_fault = 1;
    *tsf = switch_now;
    return switch_clock_failed ? -35 : 0;
}

int switch_a3_channel_valid(void *manager, uint32_t chanspec)
{
    return manager == switch_cfg && chanspec != switch_invalid;
}

int switch_a3_channel_restricted(void *manager, uint32_t chanspec)
{
    return manager == switch_cfg && chanspec == switch_restricted;
}

void switch_a3_scan_abort(void *scan, uint32_t status)
{
    if (scan != switch_scan || status != 4)
        switch_fault = 2;
    ++switch_abort_calls;
    if (!switch_abort_async)
        switch_completion = switch_a3_channel_complete();
}

void switch_a3_pm_request(void *bsscfg, uint32_t enabled)
{
    if (bsscfg != switch_cfg || enabled != 1)
        switch_fault = 6;
    ((uint8_t *)switch_pm)[8] = 1;
    switch_a3_channel_pm_capture(switch_wlc, switch_packet, switch_scb);
}

int switch_a3_pm_effective(void *bsscfg)
{
    return bsscfg == switch_cfg && ((uint8_t *)switch_pm)[8];
}

void switch_a3_channel_original(void *wlc, uint32_t chanspec)
{
    if (wlc != switch_wlc || chanspec != switch_binding.target_chanspec || switch_resume_calls)
        switch_fault = 3;
    ++switch_original_calls;
}

void switch_a3_channel_mac_enable(void *wlc)
{
    if (wlc != switch_wlc || !switch_suspend_depth) {
        switch_fault = 5;
        return;
    }
    --switch_suspend_depth;
    ++switch_resume_calls;
}

void switch_channel_cleanup_record(void *wlc, uint32_t chanspec, void *restored_wlc)
{
    if (wlc != switch_wlc || chanspec != switch_binding.target_chanspec || restored_wlc != wlc ||
        switch_resume_calls != 1)
        switch_fault = 4;
    ++switch_cleanup_calls;
}

static int switch_invoke_suspended(uint32_t initial_depth)
{
    switch_suspend_depth = initial_depth + 1;
    switch_channel_test_invoke(switch_wlc, switch_binding.target_chanspec);
    if (switch_original_calls)
        switch_a3_channel_mac_enable(switch_wlc);
    return !switch_fault && switch_suspend_depth == initial_depth && switch_resume_calls == 1;
}

static void switch_reset(void)
{
    SWITCH_A3_DIAG(
        for (size_t index = 0; index < sizeof switch_a3_diagnostic; ++index)
            ((uint8_t *)&switch_a3_diagnostic)[index] = 0);
    switch_a3_channel_complete();
    switch_zero(switch_wlc, sizeof switch_wlc);
    switch_zero(switch_cfg, sizeof switch_cfg);
    switch_zero(switch_scan, sizeof switch_scan);
    switch_zero(switch_private, sizeof switch_private);
    switch_zero(switch_pm, sizeof switch_pm);
    switch_zero(switch_packet, sizeof switch_packet);
    switch_zero(switch_scb, sizeof switch_scb);
    switch_pointer(switch_wlc, 0x168, switch_scan);
    switch_pointer(switch_wlc, 0x15c, switch_cfg);
    switch_word16(switch_wlc, 0x466, 0x1006);
    switch_word16(switch_wlc, 0x468, 0x1006);
    switch_pointer(switch_scan, 0, switch_private);
    switch_pointer(switch_private, 8, switch_wlc);
    switch_pointer(switch_cfg, 0, switch_wlc);
    switch_pointer(switch_cfg, 0x120, switch_pm);
    switch_pointer(switch_packet, 8, switch_header);
    switch_word16(switch_packet, 12, sizeof switch_header);
    switch_pointer(switch_scb, 16, switch_cfg);
    *((uint8_t *)switch_cfg + 8) = 1;
    *((uint8_t *)switch_cfg + 16) = 1;
    for (size_t index = 0; index < 6; ++index) {
        *((uint8_t *)switch_cfg + 216 + index) = switch_ap[index];
        switch_header[4 + index] = switch_header[16 + index] = switch_ap[index];
    }
    switch_word16(switch_header, 0, 0x0148);
    switch_binding.wlc = switch_wlc;
    switch_binding.bsscfg = switch_cfg;
    switch_binding.completion = 0x20001;
    switch_binding.entered = 0x20021;
    switch_binding.completion_context = switch_cfg;
    switch_binding.entered_context = switch_wlc;
    switch_binding.expiry_tsf = 5000;
    switch_binding.session = 7;
    switch_binding.clock_epoch = 8;
    switch_binding.generation = 9;
    switch_binding.home_chanspec = 0x1006;
    switch_binding.target_chanspec = 0xd024;
    switch_now = 1000;
    switch_clock_failed = switch_abort_async = 0;
    switch_restricted = switch_invalid = 0;
    switch_original_calls = switch_cleanup_calls = switch_abort_calls = switch_fault = switch_completion = 0;
    switch_suspend_depth = switch_resume_calls = 0;
}

static int switch_claim(void)
{
    const int result = switch_a3_channel_claim(&switch_binding);
    switch_pointer(switch_private, 0x1fc, (void *)switch_binding.completion);
    switch_pointer(switch_private, 0x200, switch_binding.completion_context);
    switch_pointer(switch_private, 0x22c, (void *)switch_binding.entered);
    switch_pointer(switch_private, 0x230, switch_binding.entered_context);
    *((uint8_t *)switch_scan + 6) = 1;
    return result;
}

static void switch_acknowledge(void)
{
    switch_a3_channel_pm_prepare(switch_cfg);
    switch_a3_channel_pm_status(switch_wlc, switch_cfg, switch_packet, 0x0148, 0x82, switch_header);
}

static uint32_t switch_test_gate(uint32_t variant, uint32_t initial_depth)
{
    switch_reset();
    if (variant == 13)
        *((uint8_t *)switch_cfg + 8) = 0;
    if (variant == 14)
        switch_binding.target_chanspec = switch_binding.home_chanspec;
    SWITCH_REQUIRE(!switch_claim());
    if (variant == 1 || (variant >= 6 && variant <= 9))
        switch_acknowledge();
    if (variant == 2)
        switch_a3_channel_pm_status(switch_wlc, switch_cfg, switch_header, 0x0148, 2, switch_header);
    if (variant == 3)
        switch_a3_channel_pm_status(switch_wlc, switch_cfg, switch_header, 0x1148, 0, switch_header);
    if (variant == 4)
        switch_a3_channel_pm_status(switch_wlc, switch_cfg, switch_header, 0x1148, 6, switch_header);
    if (variant == 5) {
        switch_header[4] ^= 2;
        switch_acknowledge();
    }
    if (variant == 6)
        *((uint8_t *)switch_cfg + 216) ^= 2;
    if (variant == 7)
        *((uint8_t *)switch_cfg + 8) = 0;
    if (variant == 8)
        switch_word16(switch_wlc, 0x466, 0xd095);
    if (variant == 9)
        switch_binding.target_chanspec ^= 1;
    if (variant == 10)
        switch_now = 5000;
    if (variant == 11)
        switch_clock_failed = 1;
    if (variant == 12)
        switch_pointer(switch_private, 0x200, switch_scan);
    SWITCH_REQUIRE(switch_invoke_suspended(initial_depth));
    const int allowed = variant == 1 || variant >= 12;
    SWITCH_REQUIRE(switch_original_calls == (uint32_t)allowed && switch_cleanup_calls == (uint32_t)!allowed);
    const uint32_t result = switch_a3_channel_complete();
    if (variant == 0 || (variant >= 2 && variant <= 5))
        SWITCH_REQUIRE(result == SWITCH_A3_CHANNEL_NO_PM_ACK);
    if (variant >= 6 && variant <= 9)
        SWITCH_REQUIRE(result == SWITCH_A3_CHANNEL_STATION_CHANGED);
    if (variant == 10)
        SWITCH_REQUIRE(result == SWITCH_A3_CHANNEL_EXPIRED);
    if (variant == 11)
        SWITCH_REQUIRE(result == SWITCH_A3_CHANNEL_CLOCK_FAILED);
    if (variant == 12)
        SWITCH_REQUIRE(result == SWITCH_A3_CHANNEL_OWNERSHIP_LOST && !switch_abort_calls);
    SWITCH_A3_DIAG(
        if (variant == 0)
            SWITCH_REQUIRE(switch_a3_diagnostic.counters[SWITCH_A3_DIAG_GATE_DENY] == 1 &&
                           !switch_a3_diagnostic.pm_acks);
        if (variant == 1)
            SWITCH_REQUIRE(switch_a3_diagnostic.pm_acks == 1 &&
                           switch_a3_diagnostic.counters[SWITCH_A3_DIAG_GATE_ALLOW] == 1 &&
                           !switch_a3_diagnostic.counters[SWITCH_A3_DIAG_GATE_DENY]));
    return 0;
}

static uint32_t switch_test_foreign_status(void)
{
    switch_reset();
    SWITCH_REQUIRE(!switch_claim());
    switch_a3_channel_pm_status(switch_scan, switch_cfg, switch_header, 0x1148, 2, switch_header);
    switch_a3_channel_pm_status(switch_wlc, switch_scan, switch_header, 0x1148, 2, switch_header);
    switch_a3_channel_pm_status(switch_wlc, switch_cfg, NULL, 0x1148, 2, switch_header);
    switch_a3_channel_pm_status(switch_wlc, switch_cfg, switch_header, 0x1148, 2, NULL);
    switch_header[16] ^= 2;
    switch_acknowledge();
    SWITCH_REQUIRE(!switch_a3_channel_may_leave(switch_wlc, switch_binding.target_chanspec));
    SWITCH_REQUIRE(switch_a3_channel_complete() == SWITCH_A3_CHANNEL_NO_PM_ACK);
    return 0;
}

static uint32_t switch_test_claim_bounds(void)
{
    switch_reset();
    SWITCH_REQUIRE(switch_a3_channel_claim(NULL) == -2);
    switch_binding.session = 0;
    SWITCH_REQUIRE(switch_a3_channel_claim(&switch_binding) == -2);
    switch_binding.session = 7;
    switch_binding.expiry_tsf = 1000;
    SWITCH_REQUIRE(switch_a3_channel_claim(&switch_binding) == -29);
    switch_binding.expiry_tsf = 5000;
    *((uint8_t *)switch_scan + 6) = 1;
    SWITCH_REQUIRE(switch_a3_channel_claim(&switch_binding) == -16);
    *((uint8_t *)switch_scan + 6) = 0;
    SWITCH_REQUIRE(!switch_claim());
    SWITCH_REQUIRE(switch_a3_channel_claim(&switch_binding) == -16);
    return 0;
}

static uint32_t switch_test_abort(uint32_t variant)
{
    switch_reset();
    SWITCH_REQUIRE(!switch_claim());
    if (variant >= 2)
        switch_pointer(switch_private, 0x22c, switch_scan);
    if (variant == 3)
        SWITCH_REQUIRE(switch_a3_channel_may_leave(switch_wlc, switch_binding.target_chanspec));
    switch_abort_async = variant == 1;
    const int result = switch_a3_channel_abort();
    if (variant >= 2) {
        SWITCH_REQUIRE(result == -1 && !switch_abort_calls);
        SWITCH_REQUIRE(switch_a3_channel_complete() == SWITCH_A3_CHANNEL_OWNERSHIP_LOST);
    } else {
        SWITCH_REQUIRE(!result && switch_abort_calls == 1 && !switch_fault);
        if (variant == 1) {
            SWITCH_REQUIRE(switch_a3_channel_claim(&switch_binding) == -16);
            SWITCH_REQUIRE(!switch_a3_channel_may_leave(switch_wlc, switch_binding.target_chanspec));
            SWITCH_REQUIRE(switch_a3_channel_complete() == SWITCH_A3_CHANNEL_CANCELLED);
        } else
            SWITCH_REQUIRE(switch_completion == SWITCH_A3_CHANNEL_CANCELLED);
    }
    return 0;
}

static uint32_t switch_test_social_channels(void)
{
    switch_reset();
    switch_pointer(switch_wlc, 0x15c, NULL);
    SWITCH_REQUIRE(!switch_a3_channel_socials(switch_wlc) && !switch_a3_channel_home(switch_wlc));
    switch_pointer(switch_wlc, 0x15c, switch_cfg);
    *((uint8_t *)switch_wlc + 25) = 1;
    switch_word16(switch_wlc, 0x466, 0xe02a);
    SWITCH_REQUIRE(switch_a3_channel_home(switch_wlc) == 0xe02a);
    SWITCH_REQUIRE(switch_a3_channel_socials(switch_wlc) ==
                   (SWITCH_A3_SOCIAL_6 | SWITCH_A3_SOCIAL_44 | SWITCH_A3_SOCIAL_149));
    switch_restricted = 0xd095;
    switch_invalid = 0x1006;
    SWITCH_REQUIRE(switch_a3_channel_socials(switch_wlc) == SWITCH_A3_SOCIAL_44);
    return 0;
}

static uint32_t switch_test_regulatory_change(void)
{
    switch_reset();
    SWITCH_REQUIRE(!switch_claim());
    switch_acknowledge();
    switch_restricted = switch_binding.target_chanspec;
    SWITCH_REQUIRE(switch_invoke_suspended(0));
    SWITCH_REQUIRE(!switch_original_calls && switch_cleanup_calls == 1);
    SWITCH_REQUIRE(switch_a3_channel_complete() == SWITCH_A3_CHANNEL_RESTRICTED);
    return 0;
}

void switch_channel_test_run(void)
{
    for (uint32_t index = 0; index < 15; ++index)
        switch_channel_test_results[index] = switch_test_gate(index, 0);
    switch_channel_test_results[15] = switch_test_foreign_status();
    switch_channel_test_results[16] = switch_test_claim_bounds();
    for (uint32_t index = 0; index < 3; ++index)
        switch_channel_test_results[17 + index] = switch_test_abort(index);
    switch_reset();
    switch_channel_test_results[20] =
        switch_invoke_suspended(0) && switch_original_calls == 1 && !switch_cleanup_calls ? 0 : __LINE__;
    switch_channel_test_results[21] = switch_test_abort(3);
    switch_channel_test_results[22] = switch_test_social_channels();
    switch_channel_test_results[23] = switch_test_regulatory_change();
    switch_channel_test_results[24] = switch_test_gate(0, 2);
    switch_channel_test_results[25] = switch_test_gate(1, 2);
    switch_channel_test_done = 0x53574133;
}

/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "channel.h"
#include "clock.h"
#include "wire.h"
#include "diagnostic.h"
#include <stddef.h>

extern void switch_a3_scan_abort(void *scan, uint32_t status);
extern int switch_a3_channel_valid(void *manager, uint32_t chanspec);
extern int switch_a3_channel_restricted(void *manager, uint32_t chanspec);
extern void switch_a3_pm_request(void *bsscfg, uint32_t enabled);
extern int switch_a3_pm_effective(void *bsscfg);

static struct {
    uint8_t active, associated, pm_ack, pm_attempted, pm_requesting, pm_captured;
    uint8_t ap[6];
    void *scan, *private_context, *pm_packet;
    uintptr_t pm_token;
    uint32_t result;
    struct switch_a3_channel_binding binding;
} switch_a3_channel;

const struct switch_a3_channel_binding *switch_a3_channel_current(void)
{
    return &switch_a3_channel.binding;
}

static void *switch_a3_channel_pointer(const void *base, size_t offset)
{
    return *(void *const *)((const uint8_t *)base + offset);
}

__attribute__((noinline)) static uint16_t switch_a3_channel_read16(const void *address)
{
    const uint8_t *bytes = address;
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

void *switch_a3_station_bsscfg(void *wlc)
{
    if (!wlc)
        return NULL;
    void *bsscfg = switch_a3_channel_pointer(wlc, 0x26c);
    return bsscfg && switch_a3_channel_pointer(bsscfg, 0) == wlc && !*((const uint8_t *)bsscfg + 6) ? bsscfg : NULL;
}

int switch_a3_channel_usable(void *wlc, uint16_t chanspec)
{
    void *manager = wlc ? switch_a3_channel_pointer(wlc, 0x15c) : NULL;
    return manager && (chanspec & 0x3800u) == 0x1000u && switch_a3_channel_valid(manager, chanspec) &&
           !switch_a3_channel_restricted(manager, chanspec);
}

uint16_t switch_a3_channel_socials(void *wlc)
{
    uint16_t channels = 0;
    if (switch_a3_channel_usable(wlc, 0x1006))
        channels |= SWITCH_A3_SOCIAL_6;
    if (switch_a3_channel_usable(wlc, 0xd02c))
        channels |= SWITCH_A3_SOCIAL_44;
    if (switch_a3_channel_usable(wlc, 0xd095))
        channels |= SWITCH_A3_SOCIAL_149;
    return channels;
}

uint16_t switch_a3_channel_home(void *wlc)
{
    return wlc && *((const uint8_t *)wlc + 25) ? switch_a3_channel_read16((const uint8_t *)wlc + 0x466) : 0;
}

static int switch_a3_channel_address(const uint8_t *address)
{
    for (size_t index = 0; index < 6; ++index)
        if (address[index] != switch_a3_channel.ap[index])
            return 0;
    return 1;
}

static void switch_a3_channel_pm_clear(void)
{
    switch_a3_channel.pm_packet = NULL;
    switch_a3_channel.pm_token = 0;
    switch_a3_channel.pm_ack = switch_a3_channel.pm_requesting = 0;
}

static int switch_a3_channel_pm_frame(const uint8_t *header, uint32_t fc)
{
    const uint32_t kind = fc & 0x03ffu;
    return header && (kind == 0x0148u || kind == 0x01c8u) &&
           switch_a3_channel_read16(header) == (uint16_t)fc && switch_a3_channel_address(header + 4) &&
           switch_a3_channel_address(header + 16);
}

int switch_a3_channel_owned(void)
{
    const struct switch_a3_channel_binding *binding = &switch_a3_channel.binding;
    const void *scan = switch_a3_channel.scan, *private_context = switch_a3_channel.private_context;
    return switch_a3_channel.active && switch_a3_channel_pointer(binding->wlc, 0x168) == scan &&
           switch_a3_channel_pointer(scan, 0) == private_context &&
           switch_a3_channel_pointer(private_context, 8) == binding->wlc && *((const uint8_t *)scan + 6) &&
           (uintptr_t)switch_a3_channel_pointer(private_context, 0x1fc) == binding->completion &&
           switch_a3_channel_pointer(private_context, 0x200) == binding->completion_context &&
           (uintptr_t)switch_a3_channel_pointer(private_context, 0x22c) == binding->entered &&
           switch_a3_channel_pointer(private_context, 0x230) == binding->entered_context;
}

int switch_a3_channel_claim(const struct switch_a3_channel_binding *binding)
{
    if (!binding || !binding->wlc || !binding->bsscfg || !(binding->completion & 1u) || !(binding->entered & 1u) ||
        !binding->completion_context || !binding->entered_context || !binding->session || !binding->clock_epoch ||
        !binding->generation || !binding->home_chanspec || !binding->target_chanspec)
        return -2;
    if (switch_a3_channel.active)
        return -16;
    void *scan = switch_a3_channel_pointer(binding->wlc, 0x168);
    if (!scan || !switch_a3_channel_pointer(scan, 0))
        return -25;
    void *private_context = switch_a3_channel_pointer(scan, 0);
    if (switch_a3_channel_pointer(private_context, 8) != binding->wlc ||
        switch_a3_channel_pointer(binding->bsscfg, 0) != binding->wlc || *((uint8_t *)binding->bsscfg + 6))
        return -2;
    if (*((uint8_t *)scan + 6))
        return -16;
    uint64_t now;
    if (switch_a3_clock_read(binding->wlc, &now) || binding->expiry_tsf <= now)
        return -29;
    switch_a3_channel.binding = *binding;
    switch_a3_channel.scan = scan;
    switch_a3_channel.private_context = private_context;
    switch_a3_channel.associated = *((uint8_t *)binding->bsscfg + 8) != 0;
    for (size_t index = 0; index < 6; ++index)
        switch_a3_channel.ap[index] = *((uint8_t *)binding->bsscfg + 216 + index);
    switch_a3_channel_pm_clear();
    switch_a3_channel.pm_attempted = switch_a3_channel.pm_captured = 0;
    switch_a3_channel.result = SWITCH_A3_CHANNEL_OK;
    switch_a3_channel.active = 1;
    return 0;
}

int switch_a3_channel_pm_prepare(void *bsscfg)
{
    const struct switch_a3_channel_binding *binding = &switch_a3_channel.binding;
    if (!switch_a3_channel_owned() || bsscfg != binding->bsscfg || !switch_a3_channel.associated ||
        binding->home_chanspec == binding->target_chanspec ||
        !switch_a3_channel_address((const uint8_t *)bsscfg + 216))
        return 0;
    if (!switch_a3_channel.pm_attempted) {
        const uintptr_t token = (uintptr_t)binding->completion_context;
        switch_a3_channel.pm_attempted = switch_a3_channel.pm_requesting = 1;
        switch_a3_channel.pm_token = token;
        SWITCH_A3_DIAG(SWITCH_A3_DIAG_FIELD(pm_requests));
        SWITCH_A3_DIAG_PM(PENDING, SWITCH_A3_PM_STATUS_UNAVAILABLE);
        switch_a3_pm_request(bsscfg, 1);
        if (switch_a3_channel.pm_token == token) {
            switch_a3_channel.pm_requesting = 0;
            if (!switch_a3_channel.pm_captured) {
                SWITCH_A3_DIAG(SWITCH_A3_DIAG_FIELD(pm_failed));
                SWITCH_A3_DIAG_PM(NO_PACKET, SWITCH_A3_PM_STATUS_UNAVAILABLE);
                switch_a3_channel_pm_clear();
            }
        }
    }
    return 1;
}

uintptr_t switch_a3_channel_pm_capture(void *wlc, void *packet, void *scb)
{
    if (!switch_a3_channel.pm_requesting || switch_a3_channel.pm_captured || !packet || !scb ||
        !switch_a3_channel_owned() || wlc != switch_a3_channel.binding.wlc ||
        switch_a3_channel_pointer(scb, 16) != switch_a3_channel.binding.bsscfg ||
        (*(const uint32_t *)((const uint8_t *)packet + 24) & 0x400u) ||
        switch_a3_channel_read16((const uint8_t *)packet + 12) < 24)
        return 0;
    const uint8_t *header = switch_a3_channel_pointer(packet, 8);
    if (!header || !switch_a3_channel_pm_frame(header, switch_a3_channel_read16(header)))
        return 0;
    switch_a3_channel.pm_captured = 1;
    switch_a3_channel.pm_packet = packet;
    SWITCH_A3_DIAG(SWITCH_A3_DIAG_FIELD(pm_packets));
    return switch_a3_channel.pm_token;
}

void switch_a3_channel_pm_submitted(uintptr_t token, int accepted)
{
    if (token && token == switch_a3_channel.pm_token && !accepted) {
        SWITCH_A3_DIAG(SWITCH_A3_DIAG_FIELD(pm_failed));
        SWITCH_A3_DIAG_PM(SUBMIT_REFUSED, SWITCH_A3_PM_STATUS_UNAVAILABLE);
        switch_a3_channel_pm_clear();
    }
}

void switch_a3_channel_pm_set(void *bsscfg, uint32_t enabled)
{
    if (!enabled && bsscfg == switch_a3_channel.binding.bsscfg) {
        if (switch_a3_channel.pm_token || switch_a3_channel.pm_ack)
            SWITCH_A3_DIAG_PM(INVALIDATED, SWITCH_A3_PM_STATUS_UNAVAILABLE);
        switch_a3_channel_pm_clear();
    }
}

void switch_a3_channel_pm_status(void *wlc, void *bsscfg, void *packet, uint32_t original_fc, uint32_t compact_status,
                                 const uint8_t *header)
{
    if (!switch_a3_channel_owned() || wlc != switch_a3_channel.binding.wlc ||
        bsscfg != switch_a3_channel.binding.bsscfg)
        return;
    if (switch_a3_channel.pm_ack && !(compact_status & 0xdcu) && header && (original_fc & 0x030fu) == 0x0108u &&
        switch_a3_channel_address(header + 4)) {
        switch_a3_channel.pm_ack = 0;
        SWITCH_A3_DIAG_PM(INVALIDATED, compact_status);
    }
    if (!packet || packet != switch_a3_channel.pm_packet || !switch_a3_channel.pm_token)
        return;
    SWITCH_A3_DIAG_PM(PENDING, compact_status);
    if (compact_status & 0x40u)
        return;
    switch_a3_channel.pm_packet = NULL;
    switch_a3_channel.pm_token = 0;
    switch_a3_channel.pm_requesting = 0;
    const uint8_t *pm = switch_a3_channel_pointer(bsscfg, 0x120);
    if ((compact_status & 0xfeu) != 0x82u || !pm[8] || !switch_a3_channel.associated ||
        !switch_a3_channel_pm_frame(header, original_fc) ||
        !switch_a3_channel_address((const uint8_t *)bsscfg + 216)) {
        SWITCH_A3_DIAG(SWITCH_A3_DIAG_FIELD(pm_failed));
        SWITCH_A3_DIAG_PM(STATUS_REJECTED, compact_status);
        switch_a3_channel_pm_clear();
        return;
    }
    switch_a3_channel.pm_ack = 1;
    SWITCH_A3_DIAG(SWITCH_A3_DIAG_FIELD(pm_acks));
    SWITCH_A3_DIAG_PM(ACKED, compact_status);
}

int switch_a3_channel_may_leave(void *wlc, uint32_t chanspec)
{
    if (!switch_a3_channel.active || wlc != switch_a3_channel.binding.wlc)
        return 1;
    if (!switch_a3_channel_owned()) {
        if (switch_a3_channel.pm_token || switch_a3_channel.pm_ack)
            SWITCH_A3_DIAG_PM(OWNERSHIP_LOST, SWITCH_A3_PM_STATUS_UNAVAILABLE);
        switch_a3_channel_pm_clear();
        switch_a3_channel.result = SWITCH_A3_CHANNEL_OWNERSHIP_LOST;
        switch_a3_channel.active = 0;
        return 1;
    }
    if (switch_a3_channel.result != SWITCH_A3_CHANNEL_OK) {
        SWITCH_A3_DIAG_COUNT(GATE_DENY);
        return 0;
    }
    const struct switch_a3_channel_binding *binding = &switch_a3_channel.binding;
    uint64_t now;
    if (switch_a3_clock_read(wlc, &now))
        switch_a3_channel.result = SWITCH_A3_CHANNEL_CLOCK_FAILED;
    else if (now >= binding->expiry_tsf)
        switch_a3_channel.result = SWITCH_A3_CHANNEL_EXPIRED;
    else if (chanspec != binding->target_chanspec ||
             switch_a3_channel_read16((uint8_t *)wlc + 0x466) != binding->home_chanspec ||
             ((*((uint8_t *)binding->bsscfg + 8) != 0) != switch_a3_channel.associated) ||
             !switch_a3_channel_address((uint8_t *)binding->bsscfg + 216))
        switch_a3_channel.result = SWITCH_A3_CHANNEL_STATION_CHANGED;
    else if (chanspec != binding->home_chanspec && !switch_a3_channel_usable(wlc, (uint16_t)chanspec))
        switch_a3_channel.result = SWITCH_A3_CHANNEL_RESTRICTED;
    else if (switch_a3_channel.associated && chanspec != binding->home_chanspec) {
        const uint8_t *pm = switch_a3_channel_pointer(binding->bsscfg, 0x120);
        if (!switch_a3_channel.pm_ack || !pm[8] || pm[10] || !switch_a3_pm_effective(binding->bsscfg))
            switch_a3_channel.result = SWITCH_A3_CHANNEL_NO_PM_ACK;
    }
    SWITCH_A3_DIAG(
        SWITCH_A3_DIAG_INDEX(switch_a3_channel.result == SWITCH_A3_CHANNEL_OK ?
            SWITCH_A3_DIAG_GATE_ALLOW : SWITCH_A3_DIAG_GATE_DENY));
    return switch_a3_channel.result == SWITCH_A3_CHANNEL_OK;
}

int switch_a3_channel_abort(void)
{
    const int pm_active = switch_a3_channel.pm_token || switch_a3_channel.pm_ack;
    switch_a3_channel_pm_clear();
    if (!switch_a3_channel.active)
        return switch_a3_channel.result == SWITCH_A3_CHANNEL_OWNERSHIP_LOST ? -1 : 0;
    if (!switch_a3_channel_owned()) {
        if (pm_active)
            SWITCH_A3_DIAG_PM(OWNERSHIP_LOST, SWITCH_A3_PM_STATUS_UNAVAILABLE);
        switch_a3_channel.result = SWITCH_A3_CHANNEL_OWNERSHIP_LOST;
        switch_a3_channel.active = 0;
        return -1;
    }
    if (pm_active)
        SWITCH_A3_DIAG_PM(CANCELLED, SWITCH_A3_PM_STATUS_UNAVAILABLE);
    switch_a3_channel.result = SWITCH_A3_CHANNEL_CANCELLED;
    switch_a3_scan_abort(switch_a3_channel.scan, 4);
    return 0;
}

uint32_t switch_a3_channel_complete(void)
{
    if (switch_a3_channel.pm_packet)
        SWITCH_A3_DIAG_PM(CANCELLED, SWITCH_A3_PM_STATUS_UNAVAILABLE);
    switch_a3_channel_pm_clear();
    switch_a3_channel.active = 0;
    return switch_a3_channel.result;
}

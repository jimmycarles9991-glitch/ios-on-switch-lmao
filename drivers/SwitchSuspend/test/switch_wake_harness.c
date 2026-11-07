// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host harness for drivers/SwitchSuspend/SwitchWakeSources.h: its static inline functions and the state they keep as
// exported C functions (C11, -Wconversion -Wsign-conversion), driven by switch_wake_test.cpp.
#include <stdint.h>
#include <string.h>
#include "../SwitchWakeSources.h"
#include "switch_wake_harness.h"

static struct switch_bt_buttons buttons;
static struct switch_wake_guard guard;
static struct switch_power_ring ring;

int wake_bt_buttons(const uint8_t *acl, size_t length, uint16_t *handle, uint8_t *report)
{
    return switch_wake_bt_buttons(acl, length, handle, report);
}

void bt_buttons_reset(void)
{
    memset(&buttons, 0, sizeof(buttons));
}

// One report's step packed: click | pressed << 4 | fresh << 8.
unsigned bt_report(uint16_t handle, unsigned held, uint64_t now, int dark)
{
    const struct switch_bt_buttons_step step =
        switch_bt_buttons_report(&buttons, handle, (uint8_t)held, now, dark != 0);
    return (unsigned)step.click | (unsigned)step.pressed << 4 | (step.fresh ? 1u << 8 : 0u);
}

void bt_gone(uint16_t handle)
{
    switch_bt_buttons_gone(&buttons, handle);
}

unsigned bt_down(void)
{
    return buttons.down;
}

uint64_t bt_click_limit(void)
{
    return SWITCH_BT_CLICK_LIMIT_NS;
}

void wake_guard_reset(void)
{
    memset(&guard, 0, sizeof(guard));
}

int wake_guard_arm(unsigned source, uint64_t now, uint64_t timeout, int panelOn)
{
    return switch_wake_guard_arm(&guard, source, now, timeout, panelOn != 0);
}

void wake_guard_display_on(void)
{
    switch_wake_guard_display_on(&guard);
}

int wake_guard_check(uint64_t now, int panelOn, int onRequested)
{
    return switch_wake_guard_check(&guard, now, panelOn != 0, onRequested != 0);
}

unsigned wake_guard_source(void)
{
    return guard.source;
}

uint64_t wake_guard_deadline(void)
{
    return guard.deadline;
}

int wake_guard_armed(void)
{
    return guard.armed;
}

void ring_reset(void)
{
    memset(&ring, 0, sizeof(ring));
}

void ring_append(const char *text)
{
    switch_power_ring_append(&ring, text);
}

size_t ring_render(char *out, size_t capacity)
{
    return switch_power_ring_render(&ring, out, capacity);
}

unsigned ring_lines(void)
{
    return SWITCH_POWER_LINES;
}

unsigned ring_line_bytes(void)
{
    return SWITCH_POWER_LINE;
}

// mask[2], highSet[2], lowClear[2]
void pmc_registers(uint64_t events, uint32_t out[6])
{
    switch_wake_pmc_registers(events, out, out + 2, out + 4);
}

uint64_t pmc_armed(int usbHigh, int gaugeHigh)
{
    return switch_wake_pmc_armed(usbHigh != 0, gaugeHigh != 0, false, false, false);
}

uint64_t pmc_armed_radios(int usbHigh, int gaugeHigh, int btWake, int wifiWake)
{
    return switch_wake_pmc_armed(usbHigh != 0, gaugeHigh != 0, btWake != 0, wifiWake != 0, false);
}

uint64_t pmc_armed_usb_device(int usbHigh, int usbDevice)
{
    return switch_wake_pmc_armed(usbHigh != 0, false, false, false, usbDevice != 0);
}

const char *pmc_reason(uint32_t status0, uint32_t status1, uint64_t armed)
{
    const uint32_t status[2] = {status0, status1};
    return switch_wake_pmc_reason(status, armed);
}

const char *source_name(unsigned source)
{
    return switch_wake_source_name(source);
}

unsigned bt_peek(void)
{
    return SWITCH_WAKE_BT_PEEK;
}

unsigned guard_ms(void)
{
    return SWITCH_WAKE_GUARD_MS;
}

uint64_t sleep_rtc_ms(uint32_t seconds, uint32_t millis)
{
    return switch_sleep_rtc_ms(seconds, millis);
}

uint64_t sleep_elapsed_ms(uint64_t before, uint64_t after)
{
    return switch_sleep_elapsed_ms(before, after);
}

int32_t sleep_average_ma(uint16_t before, uint16_t after, uint32_t lsb_uah, uint64_t elapsed_ms)
{
    return switch_sleep_average_ma(before, after, lsb_uah, elapsed_ms);
}

void bt_sleep_params(uint8_t out[12], int on)
{
    switch_bt_sleep_mode_params(out, on != 0);
}

static uint64_t reconnectUntil;

void bt_reconnect_open(uint64_t now)
{
    switch_bt_reconnect_open(&reconnectUntil, now);
}

int bt_reconnect_take(uint64_t now, int fresh, int dark)
{
    return switch_bt_reconnect_take(&reconnectUntil, now, fresh != 0, dark != 0);
}

int bt_reconnect_open_now(void)
{
    return reconnectUntil != 0;
}

// switch_wake_harness.c's exported wrappers of drivers/SwitchSuspend/SwitchWakeSources.h (with the button table, wake
// guard and power journal ring it keeps), for switch_wake_test.cpp.
#ifndef SWITCH_SWITCH_WAKE_HARNESS_H
#define SWITCH_SWITCH_WAKE_HARNESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int wake_bt_buttons(const uint8_t *acl, size_t length, uint16_t *handle, uint8_t *report);
void bt_buttons_reset(void);
// One report's step packed: click | pressed << 4 | fresh << 8.
unsigned bt_report(uint16_t handle, unsigned held, uint64_t now, int dark);
void bt_gone(uint16_t handle);
unsigned bt_down(void);
uint64_t bt_click_limit(void);
void wake_guard_reset(void);
int wake_guard_arm(unsigned source, uint64_t now, uint64_t timeout, int panelOn);
void wake_guard_display_on(void);
int wake_guard_check(uint64_t now, int panelOn, int onRequested);
unsigned wake_guard_source(void);
uint64_t wake_guard_deadline(void);
int wake_guard_armed(void);
void ring_reset(void);
void ring_append(const char *text);
size_t ring_render(char *out, size_t capacity);
unsigned ring_lines(void);
unsigned ring_line_bytes(void);
// mask[2], highSet[2], lowClear[2]
void pmc_registers(uint64_t events, uint32_t out[6]);
uint64_t pmc_armed(int usbHigh, int gaugeHigh);
uint64_t pmc_armed_radios(int usbHigh, int gaugeHigh, int btWake, int wifiWake);
uint64_t pmc_armed_usb_device(int usbHigh, int usbDevice);
const char *pmc_reason(uint32_t status0, uint32_t status1, uint64_t armed);
const char *source_name(unsigned source);
unsigned bt_peek(void);
unsigned guard_ms(void);
uint64_t sleep_rtc_ms(uint32_t seconds, uint32_t millis);
uint64_t sleep_elapsed_ms(uint64_t before, uint64_t after);
int32_t sleep_average_ma(uint16_t before, uint16_t after, uint32_t lsb_uah, uint64_t elapsed_ms);
void bt_sleep_params(uint8_t out[12], int on);
void bt_reconnect_open(uint64_t now);
int bt_reconnect_take(uint64_t now, int fresh, int dark);
int bt_reconnect_open_now(void);

#ifdef __cplusplus
}
#endif

#endif  // SWITCH_SWITCH_WAKE_HARNESS_H

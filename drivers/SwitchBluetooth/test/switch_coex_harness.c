// Host harness for Wi-Fi/Bluetooth coexistence (switch_coex_test.cpp): drivers/SwitchBluetooth/SwitchCoex.h (the
// Bluetooth controller's scans and ACL priority) and drivers/SwitchWLAN/SwitchWLANCoex.h (wifid's Bluetooth flags, the
// access point's sta_info), their static inline functions compiled as C and exported (switch_coex_harness.h).
#include <stdio.h>

#include "../../SwitchWLAN/SwitchWLANCoex.h"
#include "../SwitchCoex.h"
#include "switch_coex_harness.h"

static struct switch_coex_bt state;

void coex_reset(void)
{
    switch_coex_bt_reset(&state);
}

unsigned coex_command(uint16_t opcode, const uint8_t *params, uint32_t length)
{
    return switch_coex_bt_command(&state, opcode, params, length);
}

int coex_le_scan_shape(int awdl_active, uint16_t opcode, uint8_t *params, uint32_t length)
{
    return switch_coex_le_scan_shape(awdl_active != 0, opcode, params, length) ? 1 : 0;
}

int coex_describe(char *out, size_t size)
{
    return switch_coex_describe(&state, out, size);
}

// le_interval, le_window, le_active, le_on, scan_enable, priority_high, priority_handle, changes
void coex_state(uint32_t *out)
{
    out[0] = state.le_interval;
    out[1] = state.le_window;
    out[2] = state.le_active;
    out[3] = state.le_on;
    out[4] = state.scan_enable;
    out[5] = state.priority_high;
    out[6] = state.priority_handle;
    out[7] = state.changes;
}

int bt_flags_describe(uint32_t flags, char *out, size_t size)
{
    return swlan_bt_flags_describe(flags, out, size);
}

static struct swlan_sta was, now;

// Parses into the "now" reading after moving the last one to "was". Returns 1 when parsed.
int sta_parse(const uint8_t *buffer, size_t length)
{
    was = now;
    return swlan_sta_parse(buffer, length, &now);
}

// ver, len, have, tx_pkts, tx_failures, rx_ucast, rx_mcast, tx_rate_kbps, rx_rate_kbps, rx_decrypt_failed, idle,
// tx_retried, tx_retry_exhausted
void sta_fields(uint32_t *out)
{
    out[0] = now.ver;
    out[1] = now.len;
    out[2] = now.have;
    out[3] = now.tx_pkts;
    out[4] = now.tx_failures;
    out[5] = now.rx_ucast;
    out[6] = now.rx_mcast;
    out[7] = now.tx_rate_kbps;
    out[8] = now.rx_rate_kbps;
    out[9] = now.rx_decrypt_failed;
    out[10] = now.idle;
    out[11] = now.tx_retried;
    out[12] = now.tx_retry_exhausted;
}

int sta_describe(char *out, size_t size)
{
    return swlan_sta_describe(&was, &now, out, size);
}

void sta_reset(void)
{
    const struct swlan_sta empty = {0};
    was = now = empty;
}

// ---- AFH, power save, the firmware's coexistence state, NVRAM ----

unsigned afh_map(uint32_t mhz, uint8_t *map, unsigned *range)
{
    return switch_coex_afh_map(mhz, map, &range[0], &range[1]);
}

unsigned afh_used(const uint8_t *map, unsigned first, unsigned last, unsigned *inside)
{
    return switch_coex_afh_used(map, first, last, inside);
}

void pm2_sleep_ret_ext(uint8_t *out)
{
    swlan_pm2_sleep_ret_ext(out);
}

unsigned coex_pm(int bt_power, int sleep_return_set)
{
    return swlan_coex_pm(bt_power != 0, sleep_return_set != 0);
}

int btc_status_describe(const uint8_t *buffer, size_t length, char *out, size_t size)
{
    struct swlan_btc_status s;
    if (!swlan_btc_status_parse(buffer, length, &s))
        return -1;
    return swlan_btc_status_describe(&s, out, size);
}

int btc_profile_describe(const uint8_t *buffer, size_t length, char *out, size_t size)
{
    return swlan_btc_profile_describe(buffer, length, out, size);
}

int nvram_value(const char *text, size_t length, const char *key, uint32_t *out)
{
    return swlan_nvram_value(text, length, key, out);
}

int btc_profile_static(uint8_t *profile, size_t length)
{
    return swlan_btc_profile_static(profile, length);
}

int btc_profile_for(const uint8_t *nvram, size_t length, unsigned links, uint8_t *out)
{
    return swlan_btc_profile_for(nvram, length, links, out);
}

unsigned coex_mode(unsigned links, unsigned idle_mode)
{
    return swlan_coex_mode(links, idle_mode);
}

// switch_coex_harness.c's exports: SwitchCoex.h's Bluetooth state and AFH map, and SwitchWLANCoex.h's wifid flags,
// sta_info, power save, btc_dynctl and NVRAM parsing, compiled as C.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void coex_reset(void);
unsigned coex_command(uint16_t opcode, const uint8_t *params, uint32_t length);
int coex_le_scan_shape(int awdl_active, uint16_t opcode, uint8_t *params, uint32_t length);
int coex_describe(char *out, size_t size);
void coex_state(uint32_t *out);
int bt_flags_describe(uint32_t flags, char *out, size_t size);
int sta_parse(const uint8_t *buffer, size_t length);
void sta_fields(uint32_t *out);
int sta_describe(char *out, size_t size);
void sta_reset(void);
unsigned afh_map(uint32_t mhz, uint8_t *map, unsigned *range);
unsigned afh_used(const uint8_t *map, unsigned first, unsigned last, unsigned *inside);
void pm2_sleep_ret_ext(uint8_t *out);
unsigned coex_pm(int bt_power, int sleep_return_set);
int btc_status_describe(const uint8_t *buffer, size_t length, char *out, size_t size);
int btc_profile_describe(const uint8_t *buffer, size_t length, char *out, size_t size);
int nvram_value(const char *text, size_t length, const char *key, uint32_t *out);
int btc_profile_static(uint8_t *profile, size_t length);
int btc_profile_for(const uint8_t *nvram, size_t length, unsigned links, uint8_t *out);
unsigned coex_mode(unsigned links, unsigned idle_mode);

#ifdef __cplusplus
}
#endif

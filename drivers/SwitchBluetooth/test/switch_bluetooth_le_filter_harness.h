// switch_bluetooth_le_filter_harness.c's exports: SwitchBluetoothLeFilter.h compiled as C.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "../SwitchBluetoothLeFilter.h"

#ifdef __cplusplus
extern "C" {
#endif

bool h_le_capabilities(const uint8_t *ret, uint8_t length);
bool h_le_take(switch_le_filter *f, uint16_t opcode, const uint8_t *params, uint8_t length, uint8_t *sub);
unsigned h_le_program(const switch_le_filter *f, switch_le_command *out, unsigned max);
bool h_le_scan_enable(const switch_le_filter *f, uint8_t *params, uint8_t length);

#ifdef __cplusplus
}
#endif

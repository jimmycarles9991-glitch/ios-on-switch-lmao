// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host harness for switch_bluetooth_le_filter_test: SwitchBluetoothLeFilter.h compiled as C, as the kernel does not
// (it is C++ there), with the plain-C headers' flags.
#include "switch_bluetooth_le_filter_harness.h"

bool h_le_capabilities(const uint8_t *ret, uint8_t length)
{
    return switch_le_filter_capabilities(ret, length);
}

bool h_le_take(switch_le_filter *f, uint16_t opcode, const uint8_t *params, uint8_t length, uint8_t *sub)
{
    return switch_le_filter_take(f, opcode, params, length, sub);
}

unsigned h_le_program(const switch_le_filter *f, switch_le_command *out, unsigned max)
{
    return switch_le_filter_program(f, out, max);
}

bool h_le_scan_enable(const switch_le_filter *f, uint8_t *params, uint8_t length)
{
    return switch_le_filter_scan_enable(f, params, length);
}

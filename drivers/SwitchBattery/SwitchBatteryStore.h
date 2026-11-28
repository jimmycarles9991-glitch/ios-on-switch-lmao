// SPDX-License-Identifier: BSD-2-Clause-Patent
// What iPadOS 27's powerd writes into the battery service and J181 keeps in its gauge: DateOfFirstUse and
// ShipChargeLimitData (IORegistryEntrySetCFProperty on AppleSmartBattery; 24B119 powerd 0x10004cd5c and 0x1000542d8).
// Nintendo's gauge is never written: SwitchBattery keeps both in two NVRAM variables of its own, which SwitchNVRAM
// saves to the Data volume and restores before launchd (switch_battery_data_mounted). powerd writes DateOfFirstUse
// only while a pack's BatteryData lacks it (0x10004cc4c-0x10004cc80), so the restored value on the pack stops the
// write at the next boot; it reads ShipChargeLimitData back from the battery's own properties (0x100055108) and limits
// charging only when that says ShipChargeLimitSupported and ShipChargeLimitEnabled.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <libkern/c++/OSDictionary.h>

#define SWITCH_BATTERY_NVRAM_DATE_OF_FIRST_USE "switch-battery-date-of-first-use"
#define SWITCH_BATTERY_NVRAM_SHIP_CHARGE_LIMIT "switch-battery-ship-charge-limit"
#define SWITCH_BATTERY_SHIP_LIMIT_MAX          256u

// The NVRAM behind the store: IODTNVRAM in the kernel (SwitchBatteryNVRAM.cpp), a stand-in in the host test.
// read: false when the variable is missing or longer than *length; *length becomes its length.
bool switch_battery_nvram_read(const char *key, uint8_t *bytes, size_t *length);
bool switch_battery_nvram_write(const char *key, const uint8_t *bytes, size_t length);

// ShipChargeLimitData's entries, flat (booleans, numbers and strings: the types powerd's dictionary carries; any other
// entry is left out): [count] then per entry [key length][key][type][value]. 0 when it does not fit `capacity`.
size_t switch_battery_store_encode(const OSDictionary *dict, uint8_t *out, size_t capacity);
// A new dictionary from an encoding, or null for one that does not parse.
OSDictionary *switch_battery_store_decode(const uint8_t *in, size_t length);
// Two encodings hold the same entries, in whatever order each dictionary iterated them.
bool switch_battery_store_same(const uint8_t *a, size_t aLength, const uint8_t *b, size_t bLength);

// The stored values, read back from NVRAM: 0 and null for what was never written.
void switch_battery_store_load(uint64_t *dateOfFirstUse, OSDictionary **shipChargeLimit);
// Writes the values given (0 and null are skipped); false when an NVRAM write failed.
bool switch_battery_store_save(uint64_t dateOfFirstUse, const OSDictionary *shipChargeLimit);

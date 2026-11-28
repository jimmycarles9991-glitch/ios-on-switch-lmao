// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SWITCH_NVRAM_H
#define SWITCH_NVRAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Registers the NVRAM controller with IODTNVRAM. Call once after the platform expert has configured (IODTNVRAM
// exists), before launchd. Returns 0 on success; later calls return the first result.
int switch_nvram_publish(void);

// kern_exec.c load_init_program, once Data is mounted and before launchd: restores the NVRAM saved in
// /private/var/Switch/nvram.bin and starts saving it there. Returns how many variables were restored. Then hands the
// same moment to SwitchBluetooth (switch_bluetooth_data_mounted, a no-op without it), which publishes the BD address
// staged on Data before bluetoothd can read the address, and to SwitchBattery (switch_battery_data_mounted, 24B119),
// which publishes what powerd stored in NVRAM before powerd reads it.
uint32_t switch_nvram_data_mounted(void);

// Writes the NVRAM to the SD card now and returns once it is there (0, or the errno; 0 with nothing to write or before
// Data is mounted). Thread context, no spinlock held, SD card still up (before switch_block_suspend); one 12 KiB
// write and an fsync. The driver already calls it itself on system sleep and before the unmount at shutdown.
int switch_nvram_flush(void);

#ifdef __cplusplus
}
#endif

#endif

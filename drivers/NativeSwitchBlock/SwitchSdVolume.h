// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*switch_sd_read_sector_fn)(void *, uint64_t, uint8_t[512]);

struct switch_sd_files_slice {
    uint64_t first_sector, sector_count;
    uint8_t unique_guid[16], cid[16];
    bool exfat;
};

bool switch_sd_boot_card_cid(uint8_t cid[16]);

int switch_sd_files_volume(switch_sd_read_sector_fn, void *, uint64_t root_first, uint64_t root_sectors,
                           struct switch_sd_files_slice *);

#ifdef __cplusplus
}
#endif

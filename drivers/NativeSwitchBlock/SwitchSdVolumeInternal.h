// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "SwitchSdVolume.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "../../diagnostics/switch/storage/partition.h"
#include "../../diagnostics/switch/storage/sd_reference.h"

bool switch_sd_handoff_cid(const volatile uint8_t *handoff, uint8_t cid[16]);

int switch_sd_files_select(switch_sd_read_sector_fn, void *, const volatile uint8_t *handoff, uint64_t root_first,
                           uint64_t root_sectors, struct switch_gpt_workspace *, struct switch_sd_files_slice *);

#ifdef __cplusplus
}
#endif

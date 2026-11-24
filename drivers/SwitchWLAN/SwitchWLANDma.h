// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stddef.h>
#include <stdint.h>

bool switch_wlan_dma_start(char *why, size_t size);
bool switch_wlan_dma_map(uint64_t address, size_t bytes, char *why, size_t size);

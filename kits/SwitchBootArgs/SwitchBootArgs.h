// SPDX-License-Identifier: BSD-2-Clause-Patent
// Boot-args as drivers read them (kernel only). A bare flag (`switch-no-bt`) reads as 1, `switch-no-bt=0` as 0.
#ifndef SWITCH_BOOT_ARGS_H
#define SWITCH_BOOT_ARGS_H

#include <stdbool.h>
#include <stdint.h>
#include <pexpert/pexpert.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline bool switch_boot_flag(const char *name)
{
    uint32_t value = 0;
    return PE_parse_boot_argn(name, &value, sizeof(value)) != 0 && value != 0;
}

static inline uint32_t switch_boot_u32(const char *name, uint32_t fallback)
{
    uint32_t value = 0;
    return PE_parse_boot_argn(name, &value, sizeof(value)) ? value : fallback;
}

#ifdef __cplusplus
}
#endif

#endif

// SPDX-License-Identifier: GPL-2.0-only
/* The pinned dyld cache function-variant slots of one IPSW build (switch/fvariant.h). */
#ifndef SWITCH_FVARIANT_TYPES_H
#define SWITCH_FVARIANT_TYPES_H

#include <stdint.h>

struct switch_fvariant_slot {
    uint64_t slot;   /* unslid address of the pointer */
    uint64_t target; /* unslid address dyld stores there on this system */
};

struct switch_fvariant_cache {
    uint8_t uuid[16]; /* the main cache file's header UUID */
    uint32_t count;
    const struct switch_fvariant_slot *slots;
};

#endif

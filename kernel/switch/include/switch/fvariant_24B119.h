// SPDX-License-Identifier: GPL-2.0-only
/*
 * 24B119's dyld caches: each one's function-variant patch locations (the header's functionVariantInfo, all in
 * __TPRO_CONST) and the default implementation of each. Every table is per-process and picks its other
 * implementation only for per-process flag 2, MTE availability (has_sec_transition in apple[]), which this kernel never
 * passes (bsd/kern/kern_exec.c pushes it only under HAS_MTE). kernel/tests/switch_fvariant_test.cpp derives these from
 * the caches' bytes. 24B119's dyld compares a slot with its choice before it stores (0x1804a7d88: ldr, cmp, b.eq over
 * makeWriteable, str).
 */
#ifndef SWITCH_FVARIANT_24B119_H
#define SWITCH_FVARIANT_24B119_H

#include "fvariant_types.h"

/* dyld_shared_cache_arm64e: .25.dylddata's and .54.dylddata's first __TPRO_CONST pages. */
static const struct switch_fvariant_slot switch_fvariant_24B119_slots[] = {
    {UINT64_C(0x1ed9e8000), UINT64_C(0x1844f0cc8)}, /* QuartzCore, table 0 */
    {UINT64_C(0x1ed9e8008), UINT64_C(0x1844f0cc8)}, /* QuartzCore, table 1 */
    {UINT64_C(0x1ed9e8010), UINT64_C(0x1844f0cc8)}, /* QuartzCore, table 2 */
    {UINT64_C(0x1ed9e8018), UINT64_C(0x1844f0cc8)}, /* QuartzCore, table 3 */
    {UINT64_C(0x1ed9e8020), UINT64_C(0x1844f0cc8)}, /* QuartzCore, table 4 */
    {UINT64_C(0x1ed9e8028), UINT64_C(0x1af400988)}, /* libdispatch.dylib */
    {UINT64_C(0x2758d0000), UINT64_C(0x207abcb04)}, /* AVConference */
};

/* System/DriverKit/System/Library/dyld/dyld_shared_cache_arm64e: its one __TPRO_CONST page. */
static const struct switch_fvariant_slot switch_fvariant_24B119_driverkit_slots[] = {
    {UINT64_C(0x183800000), UINT64_C(0x181510540)}, /* DriverKit's libdispatch.dylib */
};

static const struct switch_fvariant_cache switch_fvariant_24B119[] = {
    {
        /* A2DCA908-9684-3507-B87F-FE22B886697A */
        .uuid = {0xa2, 0xdc, 0xa9, 0x08, 0x96, 0x84, 0x35, 0x07, 0xb8, 0x7f, 0xfe, 0x22, 0xb8, 0x86, 0x69, 0x7a},
        .count = sizeof(switch_fvariant_24B119_slots) / sizeof(switch_fvariant_24B119_slots[0]),
        .slots = switch_fvariant_24B119_slots,
    },
    {
        /* 7F89CBD0-2110-3D8C-B63A-37A69D1BC7CE */
        .uuid = {0x7f, 0x89, 0xcb, 0xd0, 0x21, 0x10, 0x3d, 0x8c, 0xb6, 0x3a, 0x37, 0xa6, 0x9d, 0x1b, 0xc7, 0xce},
        .count = sizeof(switch_fvariant_24B119_driverkit_slots) / sizeof(switch_fvariant_24B119_driverkit_slots[0]),
        .slots = switch_fvariant_24B119_driverkit_slots,
    },
};

#endif

/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWITCH_A3_ABI_H
#define SWITCH_A3_ABI_H

#define SWITCH_A3_CAPS_MAGIC   0x33415753
#define SWITCH_A3_CAPS_VERSION 1
#define SWITCH_A3_CAPS_BYTES   64
#define SWITCH_A3_MODULE_NAME  "switch_awdl_a3"
#define SWITCH_A3_CAPS_IOVAR   "switch_awdl_caps"

#define SWITCH_A3_COLD            0
#define SWITCH_A3_HEAP_READY      1
#define SWITCH_A3_ATTACHED        2
#define SWITCH_A3_REGISTER_FAILED 3
#define SWITCH_A3_BOUNDS_FAILED   4
#define SWITCH_A3_ROM_MISMATCH    5

#define SWITCH_A3_STATUS_OFFSET       12
#define SWITCH_A3_HEAP_BEGIN_OFFSET   44
#define SWITCH_A3_HEAP_END_OFFSET     48
#define SWITCH_A3_ORIGINAL_HEAP_BEGIN 0x20e670
#define SWITCH_A3_ORIGINAL_HEAP_END   0x23df6c
#define SWITCH_A3_BOOTSTRAP_MAX_BYTES 8192
#define SWITCH_A3_CANDIDATE_MAX_BYTES 16384
#if defined(SWITCH_A3_CANDIDATE) && SWITCH_A3_CANDIDATE
#define SWITCH_A3_HEAP_MIN_BYTES    178064
#define SWITCH_A3_MAX_PAYLOAD_BYTES SWITCH_A3_CANDIDATE_MAX_BYTES
#else
#define SWITCH_A3_HEAP_MIN_BYTES    186256
#define SWITCH_A3_MAX_PAYLOAD_BYTES SWITCH_A3_BOOTSTRAP_MAX_BYTES
#endif

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>

struct switch_a3_caps {
    uint32_t magic, version, size, status;
    int32_t register_status;
    uint32_t features, initialized, wlc, context;
    uint32_t island_begin, island_end, heap_begin, heap_end;
    uint32_t reserved[3];
};

#ifdef __cplusplus
#define SWITCH_A3_STATIC_ASSERT static_assert
#else
#define SWITCH_A3_STATIC_ASSERT _Static_assert
#endif
SWITCH_A3_STATIC_ASSERT(sizeof(struct switch_a3_caps) == SWITCH_A3_CAPS_BYTES, "A3 caps wire size");
SWITCH_A3_STATIC_ASSERT(offsetof(struct switch_a3_caps, status) == SWITCH_A3_STATUS_OFFSET, "A3 status offset");
SWITCH_A3_STATIC_ASSERT(offsetof(struct switch_a3_caps, heap_begin) == SWITCH_A3_HEAP_BEGIN_OFFSET,
                        "A3 heap begin offset");
SWITCH_A3_STATIC_ASSERT(offsetof(struct switch_a3_caps, heap_end) == SWITCH_A3_HEAP_END_OFFSET, "A3 heap end offset");
#undef SWITCH_A3_STATIC_ASSERT
#endif
#endif

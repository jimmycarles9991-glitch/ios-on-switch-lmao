// SPDX-License-Identifier: MIT
// GPU-written results: the 3D engine's sample counter and its reports, and the copy engine's small writes into memory a
// later reader takes them from (a copy, a constant, a semaphore release, a predicate on one).
#pragma once

#include "../SwitchGM20B/SwitchGM20B3D.h"
#include "../SwitchGM20B/SwitchGM20BCopy.h"
#include "nv_push.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_GM20B_SAMPLES_BEGIN_WORDS  2u
#define SWITCH_GM20B_SAMPLES_END_WORDS    1u
#define SWITCH_GM20B_SAMPLES_REPORT_WORDS 5u

// Zero the samples-passed counter and count from here (NVK's occlusion query begin)
void switch_gm20b_samples_begin(struct nv_push *p);

void switch_gm20b_samples_end(struct nv_push *p);

// The counter as 64 bits followed by a 64-bit timestamp: 16 bytes at `va`, 16-byte aligned (NVK's occlusion query end)
void switch_gm20b_samples_report(struct nv_push *p, uint64_t va);

#define SWITCH_GM20B_COPY_BYTES_WORDS 10u

// `bytes` (1..32767) from `src` to `dst`, pitch-linear both sides, through the L2 (FLUSH)
void switch_gm20b_copy_bytes(struct nv_push *p, uint64_t src, uint64_t dst, uint32_t bytes, uint8_t data_transfer_type);

#define SWITCH_GM20B_COPY_U64_WORDS 9u

// The 64-bit `value` written at `dst` (8-byte aligned) from the copy engine's remap constants
void switch_gm20b_copy_u64(struct nv_push *p, uint64_t dst, uint64_t value, uint8_t data_transfer_type);

#define SWITCH_GM20B_COPY_PREDICATE_WORDS 4u

// The copy engine's launches from here run under `mode` (NVB0B5_SET_RENDER_ENABLE_C_MODE_*): CONDITIONAL reads the
// value at `va`; TRUE (va 0) puts it back to always
void switch_gm20b_copy_predicate(struct nv_push *p, uint64_t va, uint32_t mode);

#define SWITCH_GM20B_COPY_RELEASE_WORDS 5u

// A one-word semaphore release of `payload` at `va` once the copy engine's earlier launches completed
void switch_gm20b_copy_release(struct nv_push *p, uint64_t va, uint32_t payload);

#ifdef __cplusplus
}
#endif

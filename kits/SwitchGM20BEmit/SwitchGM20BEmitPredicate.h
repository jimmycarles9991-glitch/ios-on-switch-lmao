// SPDX-License-Identifier: MIT
// Render enable on the compute class, as NVK's conditional rendering drives it (nvk_cmd_draw.c
// nvk_cmd_begin_cond_render_copy_engine): RENDER_IF_EQUAL / RENDER_IF_NOT_EQUAL compare two 64-bit operands, A at `va` and
// B at `va` + 16, `va` 16-byte aligned. The GLOBAL methods set it for the 3D and compute classes at once. SEND_PCAS and
// inline-to-memory writes on those subchannels obey it (nvk_cmd_dispatch.c, nvk_cmd_copy.c).
#pragma once

#include <stdbool.h>

#include "../SwitchGM20B/SwitchGM20BCompute.h"
#include "../SwitchGM20B/SwitchGM20BCopy.h"
#include "nv_push.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_GM20B_RENDER_ENABLE_WORDS 4u

// Launches and inline writes from here run where the operands at `va` compare as `mode`
// (NVB1C0_SET_GLOBAL_RENDER_ENABLE_C_MODE_RENDER_IF_EQUAL / _NOT_EQUAL)
void switch_gm20b_render_enable(struct nv_push *p, uint64_t va, uint32_t mode);

// Launches and inline writes from here always run
void switch_gm20b_render_enable_always(struct nv_push *p);

#define SWITCH_GM20B_INLINE_WORD_WORDS 8u

// One 32-bit word written by the compute class's inline-to-memory unit, flushed to L2; skipped where render enable fails
void switch_gm20b_compute_inline_word(struct nv_push *p, uint64_t va, uint32_t value);

#define SWITCH_GM20B_INLINE_WORDS_WORDS(count) (7u + (count))

// `count` 32-bit words written from `words` as one inline-to-memory line, flushed to L2; skipped where render enable fails
void switch_gm20b_compute_inline_words(struct nv_push *p, uint64_t va, const uint32_t *words, uint32_t count);

#define SWITCH_GM20B_FETCH_AFTER_IDLE_WORDS 1u

// Host waits until the engines are idle before it takes the methods after this one, so a pushbuffer segment fetched
// with NO_PREFETCH after it (GP_ENTRY1_SYNC_WAIT) sees what the work before wrote (NVK's pre-Hopper host WFI)
void switch_gm20b_fetch_after_idle(struct nv_push *p);

#define SWITCH_GM20B_COMPUTE_IDLE_WORDS 2u

// The compute work before has finished and its stores reached L2, for the copy engine to read
void switch_gm20b_compute_idle_flushed(struct nv_push *p);

#define SWITCH_GM20B_COPY_WORD_REDUCE_WORDS 15u

// The 32-bit word at `src` copied to `dst`, then made `reduction`(word, payload) (NVB0B5_LAUNCH_DMA_SEMAPHORE_REDUCTION_*,
// compared unsigned or signed) by the launch's own semaphore release, which comes after its copy completed
void switch_gm20b_copy_word_reduce(struct nv_push *p, uint64_t src, uint64_t dst, uint32_t payload, uint32_t reduction,
                                   bool is_signed);

#ifdef __cplusplus
}
#endif

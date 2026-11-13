/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

/* A job's acquire on an event's word (SwitchGpuEventWord.h) reaching `payload`: `pool` is the word's byte offset in the
 * fence pool, `queue` the uid of the channel that releases into it. */
struct switch_gpu_word_wait {
	uint64_t pool;
	uint32_t payload, queue;
};

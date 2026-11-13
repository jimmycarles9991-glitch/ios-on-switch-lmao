// SPDX-License-Identifier: MIT
// A channel slot's record in its connection's fence page (TegraGM20BAbiFence, then AbiFenceSemantic, then
// AbiFenceIdentity), read without the connection lock: the page outlives the slot's channels.
#pragma once
#include <stdint.h>
#include "TegraGM20BABI.h"

static inline uint64_t switch_gpu_fence_identity(const volatile uint32_t *record)
{
	const auto *identity =
		reinterpret_cast<const volatile TegraGM20BAbiFenceIdentity *>(record + 8);
	return __atomic_load_n(&identity->channel_uid, __ATOMIC_ACQUIRE);
}

// The channel `identity` names passed `seqno` and lives. A kill writes the error before moving the fence past it.
static inline bool switch_gpu_fence_passed(const volatile uint32_t *record, uint32_t seqno,
					   uint64_t identity)
{
	const uint32_t done = __atomic_load_n(&record[0], __ATOMIC_ACQUIRE);
	return !__atomic_load_n(&record[3], __ATOMIC_RELAXED) && int32_t(done - seqno) >= 0 &&
	       switch_gpu_fence_identity(record) == identity;
}

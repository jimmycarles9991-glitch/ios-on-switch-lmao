/* SPDX-License-Identifier: MIT */
#pragma once
#include "TegraGM20BABI.h"

static inline int switch_gpu_channel_copy_only(uint32_t engines)
{
	if (!(engines & TegraGM20BEngineCopyOnly))
		return 0;
	return engines == (TegraGM20BEngineCopy | TegraGM20BEngineCopyOnly) ? 1 : -22;
}

static inline uint32_t switch_gpu_channel_first_object(int copy_only)
{
	return copy_only ? 4u : 0u;
}

static inline uint32_t switch_gpu_channel_bind_words(uint32_t *words, const uint32_t *classes,
						     int copy_only)
{
	uint32_t count = 0;
	for (uint32_t subchannel = switch_gpu_channel_first_object(copy_only); subchannel < 5;
	     ++subchannel) {
		words[count++] = (1u << 29) | (1u << 16) | (subchannel << 13);
		words[count++] = classes[subchannel];
	}
	return count;
}

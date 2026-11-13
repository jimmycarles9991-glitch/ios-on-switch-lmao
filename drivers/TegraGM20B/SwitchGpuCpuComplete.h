// SPDX-License-Identifier: MIT
#pragma once
#include "SwitchGpuBacking.h"
#include "TegraGM20BABI.h"

struct SwitchGpuCpuRange {
	switch_gpu_backing *backing;
	uint64_t offset, bytes;
};

int switch_gpu_cpu_complete(const switch_gpu_backing_token &, const SwitchGpuCpuRange *,
			    uint32_t count, TegraGM20BAbiCpuCompleteReply *);

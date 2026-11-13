/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct switch_gpu_clock_state {
	uint32_t maximum_khz, minimum_khz, ceiling_khz, requested_khz;
};

/* Thread context. A zero request restores the boot ceiling. Only divider-reachable qualified rates are selected;
 * applied is the actual ceiling, which may be below the request. No voltage or PLL VCO change is authorized. */
int switch_gpu_clock_get(struct switch_gpu_clock_state *state);
int switch_gpu_clock_ceiling(uint32_t requested_khz, uint32_t *applied_khz);
#ifdef __cplusplus
}
#endif

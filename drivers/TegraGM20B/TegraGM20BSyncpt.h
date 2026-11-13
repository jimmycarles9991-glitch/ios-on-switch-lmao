// SPDX-License-Identifier: MIT
// Host1x syncpoints from a GM20B channel's push: the host's SYNCPOINTA/SYNCPOINTB methods (cl906f.h; nvgpu
// channel_sync_syncpt.c gk20a_channel_gen_syncpt_incr_cmd and add_wait_cmd), so the GPU and host1x's engines (VIC,
// NVDEC, NVENC, NVJPG) order on hardware with no CPU between them. Not yet seen working on the device: the T210 GPU host
// reaching host1x's syncpoints is what the first run that uses these must show.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TGM_SYNCPT_INCR_DWORDS (12u + 2u * TGM_SYNCPT_INCRS)
#define TGM_SYNCPT_WAIT_DWORDS 4u
/* nvgpu increments twice per release (gk20a_channel_get_syncpt_incr_per_release): wait for value + 2 */
#define TGM_SYNCPT_INCRS 2u

static inline uint32_t tgm_syncpt_header(uint32_t mthd)
{
	return (1u << 29) | (1u << 16) | (mthd >> 2);
}

/* The engines idle, their writes in DRAM (WFI, FB_FLUSH, L2_FLUSH_DIRTY, FB_FLUSH, as the channel fence does), then
 * syncpoint `id` incremented TGM_SYNCPT_INCRS times */
static inline uint32_t tgm_syncpt_incr(uint32_t *w, uint32_t id)
{
	uint32_t i = 0;
	w[i++] = tgm_syncpt_header(0x0078);  // WFI
	w[i++] = 0;
	w[i++] = tgm_syncpt_header(0x0024);  // FB_FLUSH
	w[i++] = 0;
	w[i++] = tgm_syncpt_header(0x0030);  // MEM_OP_C
	w[i++] = 0;
	w[i++] = tgm_syncpt_header(0x0034);  // MEM_OP_D
	w[i++] = 0x10u << 27;                // L2_FLUSH_DIRTY
	w[i++] = tgm_syncpt_header(0x0024);
	w[i++] = 0;
	w[i++] = tgm_syncpt_header(0x0070);  // SYNCPOINTA: payload, unused by an increment
	w[i++] = 0;
	for (uint32_t k = 0; k < TGM_SYNCPT_INCRS; k++) {
		w[i++] = tgm_syncpt_header(0x0074);  // SYNCPOINTB
		w[i++] = (id & 0xff) << 8 | 1u;      // INDEX, OPERATION INCR
	}
	return i;
}

/* The channel waits (switching away meanwhile) until syncpoint `id` reaches `threshold` */
static inline uint32_t tgm_syncpt_wait(uint32_t *w, uint32_t id, uint32_t threshold)
{
	w[0] = tgm_syncpt_header(0x0070);
	w[1] = threshold;
	w[2] = tgm_syncpt_header(0x0074);
	w[3] = (id & 0xff) << 8 | 1u << 4;  // INDEX, WAIT_SWITCH, OPERATION WAIT
	return 4;
}

#ifdef __cplusplus
}
#endif

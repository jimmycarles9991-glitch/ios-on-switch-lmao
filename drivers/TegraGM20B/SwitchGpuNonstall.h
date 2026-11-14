/* SPDX-License-Identifier: MIT */
/* GM20B's job interrupts on the nonstall tree, as nvgpu routes them: PFIFO_INTR_EN_1 sends PFIFO_INTR_0's channel bit to
 * PMC_INTR_1 (fifo_intr_en_1, gm20b) and PMC_INTR_MASK_1 lets PFIFO through on that tree (mc_intr_mask_1), whose line
 * is GPU_NONSTALL (Tegra X1 TRM table 11: global interrupt 158, GIC INTID 190; GPU_STALL is 157, INTID 189). The
 * errors stay on PFIFO_INTR_EN_0 and the stall tree, which nvkm handles. nvkm sets neither EN_1 nor MASK_1: its own
 * channel interrupt rides the stall tree with the errors. */
#pragma once
#include <stdint.h>
#include "../../kits/SwitchMMIO/SwitchMMIO.h"

#define SWITCH_GPU_PMC_INTR_1 0x000104u
#define SWITCH_GPU_PMC_INTR_MASK_0 0x000640u
#define SWITCH_GPU_PMC_INTR_MASK_1 0x000644u
#define SWITCH_GPU_PMC_PFIFO 0x00000100u
#define SWITCH_GPU_PFIFO_INTR_0 0x002100u
#define SWITCH_GPU_PFIFO_INTR_EN_0 0x002140u
#define SWITCH_GPU_PFIFO_INTR_EN_1 0x002528u
#define SWITCH_GPU_PFIFO_CHANNEL_INTR 0x80000000u
/* nvkm's interrupt vector for PFIFO on PMC tree 1 (leaf 1, PMC_PFIFO's bit): nvkm owns PMC_INTR_MASK_1 */
#define SWITCH_GPU_NONSTALL_VECTOR (32 + 8)

/* The nonstall line's pass, before it reads a fence: the channel bit acknowledged, then read back. Accesses to one device
 * stay in order, so the read returns once the acknowledgement is at the GPU, and as a load-acquire it keeps the fence
 * loads after it: a job that ends after the acknowledgement raises the bit again. 1: the tree is quiet. */
static inline int switch_gpu_nonstall_ack(volatile uint8_t *bar0)
{
	switch_mmio_write32(bar0, SWITCH_GPU_PFIFO_INTR_0, SWITCH_GPU_PFIFO_CHANNEL_INTR);
	return (switch_mmio_read32(bar0, SWITCH_GPU_PFIFO_INTR_0) & SWITCH_GPU_PFIFO_CHANNEL_INTR)
		       ? 0
		       : 1;
}

// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

// Noncacheable shared memory needs ordering at publication and consumption, without cache maintenance.
#ifndef SWITCH_DMA_ORDER
#define SWITCH_DMA_ORDER(kind) __asm__ volatile("dmb " kind ::: "memory")
#endif

static inline void switch_dma_publish(void)  // NOLINT(modernize-redundant-void-arg): a C header
{
    SWITCH_DMA_ORDER("oshst");
}

static inline void switch_dma_consume(void)  // NOLINT(modernize-redundant-void-arg): a C header
{
    SWITCH_DMA_ORDER("oshld");
}

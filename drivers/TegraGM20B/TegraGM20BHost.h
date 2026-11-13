/* SPDX-License-Identifier: MIT */
/* PFIFO registers the scheduler (sched.c) and hang recovery (recover.c) read: engine status as gk104_engn_status
 * decodes it, the preempt register as gf100_chan_preempt writes it, the PBDMA status as gk104_runq_idle reads it, its
 * interrupts as gk104_runq_intr reads them and the method it is on (method address 13:2) as gf100_runq_intr decodes it. */
#pragma once

#define PFIFO_ENG_STATUS(i) (0x002640 + (i) * 8)
#define PFIFO_ENG_BUSY 0x80000000
#define PFIFO_ENG_CHSW 0x00008000
#define PFIFO_ENG_CTX_STATUS(s) (((s) >> 13) & 7)
#define PFIFO_ENG_CTX_VALID 1
#define PFIFO_ENG_CTX_LOAD 5
#define PFIFO_ENG_CTX_SAVE 6
#define PFIFO_ENG_CTX_SWITCH 7
#define PFIFO_ENG_ID_TSG 0x00001000
#define PFIFO_ENG_ID(s) ((s) & 0xfff)
#define PFIFO_ENG_NEXT_ID_TSG 0x10000000
#define PFIFO_ENG_NEXT_ID(s) (((s) >> 16) & 0xfff)
/* a runlist's bit stops its scheduling: set by software (gk104_runl_block), and by a scheduler error (written 1 to
 * clear, gk104_runl_fault_clear) */
#define PFIFO_SCHED_DISABLE 0x002630
#define ERROR_SCHED_DISABLE 0x00262c
#define PFIFO_PREEMPT 0x002634
#define PFIFO_PREEMPT_PENDING 0x00100000
/* the preempt's target: a channel, or a channel group (gk110_cgrp_preempt) */
#define PFIFO_PREEMPT_TSG 0x01000000
#define PFIFO_PREEMPT_ID(v) ((v) & 0xfff)
#define PBDMA_STATUS(i) (0x003080 + (i) * 4)
#define PBDMA_BUSY 0x0000e000
#define PBDMA_CHANNEL(i) (0x040120 + (i) * 0x2000)
#define PBDMA_CHID(v) ((v) & 0xfff)
#define PBDMA_METHOD0(i) (0x0400c0 + (i) * 0x2000)
#define PBDMA_DATA0(i) (0x0400c4 + (i) * 0x2000)
#define PBDMA_METHOD_ADDR(v) ((v) & 0x3ffc)
/* what gf100_runq_intr writes over a PBDMA's method: VALID, NV906F 0x0008 (NOP) */
#define PBDMA_METHOD_NOP 0x80600008u
#define PBDMA_INTR_0(i) (0x040108 + (i) * 0x2000)
#define PBDMA_INTR_EN_0(i) (0x04010c + (i) * 0x2000)
#define PBDMA_INTR_1(i) (0x040148 + (i) * 0x2000)
#define PBDMA_INTR_EN_1(i) (0x04014c + (i) * 0x2000)

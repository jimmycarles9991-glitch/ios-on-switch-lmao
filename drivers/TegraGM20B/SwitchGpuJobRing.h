/* SPDX-License-Identifier: MIT */
/* The GPU job ring (--//kernel:recorder): each user channel's completed seqnos as tracepoints in the flight recorder's
 * kdebug ring (kernel/switch/recorder), from the stamps stats_fences has already read when the interrupt pass sees
 * the channel's fence move, after the waiters' wake. One SWITCH_KDBG_GPU_JOB per seqno: arg1 seqno << 32 |
 * compositor << 16 | channel, arg2 and arg3 the GPU's time (PTIMER ns, nvxnu_nanotime's domain:
 * TegraGM20BClockABI.h) when it started the submission (after the WFI on the one before) and when it finished it, 0
 * for a stamp the ring no longer holds or that belongs to a later seqno. The process is the one whose thread logged
 * the channel's SWITCH_KDBG_SUBMIT. */
#pragma once

#ifndef SWITCH_RECORDER
#define SWITCH_RECORDER 0
#endif

#define SWITCH_KDBG_GPU_JOB \
	0x3e8 /* kernel/switch/include/switch/recorder.h gives the job ring 0x3e8-0x3ef */

static inline void switch_gpu_job_ring(const struct tgm_chan *chan, u32 seqno, u64 start, u64 end)
{
	nvxnu_kdebug(SWITCH_KDBG_GPU_JOB,
		     (u64)seqno << 32 | (u64)(chan->critical != 0) << 16 | chan->chid, start, end);
}

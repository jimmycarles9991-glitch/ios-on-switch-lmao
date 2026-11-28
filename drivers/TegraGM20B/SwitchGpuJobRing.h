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
/* A compositor job kicked behind a dependency not yet settled, one event each (TGM_STATS, chan_note_foreign): arg1 the
 * waiting channel | the dependency's engine (SwitchGpuBacking.h) << 16 | its GM20B channel << 32 (0 for another
 * engine), arg2 the value it waits for, arg3 the waiting channel's last submitted seqno. */
#define SWITCH_KDBG_GPU_WAIT 0x3e9
/* A submission waiting for room in its channel's ring (tgm_chan_wait_ready), at its start and its end: arg1 the
 * channel, arg2 the ring's GET << 32 | its PUT, arg3 the channel's last submitted seqno << 32 | its last done (at the
 * end: the wait's result in the low word instead). */
#define SWITCH_KDBG_GPU_ROOM 0x3ea
#define SWITCH_KDBG_GPU_ROOM_DONE 0x3eb

static inline void switch_gpu_job_ring(const struct tgm_chan *chan, u32 seqno, u64 start, u64 end)
{
	nvxnu_kdebug(SWITCH_KDBG_GPU_JOB,
		     (u64)seqno << 32 | (u64)(chan->critical != 0) << 16 | chan->chid, start, end);
}

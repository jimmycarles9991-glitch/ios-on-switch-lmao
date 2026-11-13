// SPDX-License-Identifier: MIT
// One channel's quiet job ends as gpu_exec.c, gpu_quiet.c and the interrupt pass drive them: the GPU runs the channel's
// entries in order, a fence or wake entry that interrupts runs the pass, and a waiter publishes its seqno and asks
// before it tests the fence.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include "../SwitchGpuActiveIrq.h"
#include "../SwitchGpuJobFences.h"
#include "../SwitchGpuQuiet.h"

namespace quiet_model
{
struct Channel {
	struct Entry {
		uint32_t seqno;  // the fence's release; 0 for a wake entry
		bool interrupts;
	};

	explicit Channel(uint32_t start) : submitted(start), fence(start), seen(start)
	{
		switch_gpu_quiet_init(&quiet, start);
		switch_gpu_jobs_init(&jobs, start);
	}

	// chan_fence_job's decision and its kick, for a job releasing `seqnos`
	uint32_t submit(uint32_t seqnos, bool quietJob)
	{
		const int announce = switch_gpu_quiet_announces(&quiet, quietJob);
		return kick(seqnos, announce);
	}

	uint32_t kick(uint32_t seqnos, int announce)
	{
		const uint32_t first = submitted + 1, last = submitted + seqnos;
		switch_gpu_jobs_kick(&jobs, first, last, 0, !announce);
		if (announce)
			switch_gpu_quiet_announce(&quiet, last);
		fifo.push_back({last, announce != 0});
		submitted = last;
		return last;
	}

	// The owner's thread call (tgm_chan_wake_push): an entry behind the newest job unless the pass saw it end
	void ownerWake()
	{
		asked = false;
		if (!switch_gpu_quiet_wake_due(&quiet, submitted))
			return;
		switch_gpu_quiet_announce(&quiet, submitted);
		if (int32_t(seen - submitted) >= 0)
			return;
		switch_gpu_jobs_announce_newest(&jobs);
		fifo.push_back({0, true});
		++wakeEntries;
	}

	// tgm_chan_need, the owner's wake call left to ownerWake
	void need(uint32_t seqno)
	{
		if (int32_t(fence - seqno) < 0 && switch_gpu_quiet_need(&quiet, seqno))
			asked = true;
	}

	// tgm_chan_want: the waiter's slot, then the ask; true if it sleeps
	bool wait(uint32_t seqno)
	{
		switch_gpu_waiter_want(&wanted, seqno);
		need(seqno);
		sleeping = int32_t(fence - seqno) < 0;
		return sleeping;
	}

	// tgm_chan_announced: whether the interrupt that wakes a waiter of `seqno` has come
	bool announced(uint32_t seqno) const
	{
		int is = 0;
		const uint32_t end = switch_gpu_jobs_announced_by(&jobs, seqno, &is);
		return is && int32_t(fence - end) >= 0;
	}

	void run(size_t entries = ~size_t(0), bool passes = true)
	{
		while (entries-- && !fifo.empty()) {
			const Entry e = fifo.front();
			fifo.pop_front();
			if (e.seqno)
				fence = e.seqno;
			if (e.interrupts && passes)
				pass();
		}
	}

	void pass()
	{
		++interrupts;
		seen = fence;
		if (sleeping && switch_gpu_waiter_reached(&wanted, fence)) {
			sleeping = false;
			++woken;
		}
		if (passed)
			passed();
	}

	switch_gpu_quiet quiet{};
	switch_gpu_jobs jobs{};
	uint64_t wanted = 0;
	uint32_t submitted, fence, seen;
	std::deque<Entry> fifo;
	std::function<void()> passed;  // what else the channel's pass runs (present_watch_fenced)
	bool asked = false, sleeping = false;
	unsigned interrupts = 0, woken = 0, wakeEntries = 0;
};
}  // namespace quiet_model

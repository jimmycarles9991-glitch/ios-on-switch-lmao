// SPDX-License-Identifier: MIT
// gpu_inline.h's gate_try_enter and gate_leave on the host. A CPU is a mutex, held for one gate call as the kernel keeps
// preemption off for it, and each call of a thread lands on the next CPU, so a caller's enter and leave count on
// different cells, as they do for a thread that moved between them.
#pragma once

#include <atomic>
#include <mutex>

#include "../TegraGM20BSleepGate.h"

class GateCpus
{
public:
	tgm_gate_entry tryEnter(tgm_gate *gate)
	{
		const unsigned int cpu = nextCpu();
		std::lock_guard<std::mutex> hold(cpus_[cpu]);
		return tgm_gate_try_enter(gate, cpu);
	}

	bool leave(tgm_gate *gate)
	{
		const unsigned int cpu = nextCpu();
		std::lock_guard<std::mutex> hold(cpus_[cpu]);
		return tgm_gate_leave(gate, cpu) != 0;
	}

private:
	static constexpr unsigned int kCpus = 4;  // the X1's cores
	std::mutex cpus_[kCpus];
	std::atomic<unsigned int> threads_{0};

	unsigned int nextCpu()
	{
		thread_local unsigned int next = threads_++;
		return next++ % kCpus;
	}
};

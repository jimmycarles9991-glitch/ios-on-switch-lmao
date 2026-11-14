// SPDX-License-Identifier: MIT
#include "SwitchGpuJobStats.h"
#if SWITCH_GPU_JOB_STATS
#include <kern/cpu_number.h>
#include "SwitchGpuResourceSet.h"

extern "C" void _disable_preemption(void);  // NOLINT(bugprone-reserved-identifier): XNU's own name
extern "C" void _enable_preemption(void);   // NOLINT(bugprone-reserved-identifier): XNU's own name

namespace
{
constexpr uint32_t kPhases = 4;  // TGM_SUBMIT_RESOLVE, _LOCK, _EXEC, _COMMIT
constexpr uint32_t kCpus = 4;    // the X1's cores

enum : uint32_t {
	kJobs,
	kSets,
	kStale,
	kShared,
	kBackings,
	kSiblings,
	kTotal,
	kNs,
	kCauses = kNs + kPhases,
	kCounts = kCauses + SWITCH_GPU_SET_CAUSES
};

// A CPU's counts, stored only by that CPU with preemption off: each has one writer, and the line's reader sums the CPUs
// and prints what moved since its last line. The maxima are for the window whose number they carry.
struct Cpu {
	uint64_t counts[kCounts];
	uint64_t maxWindow, sharedMax, totalMax;
};

Cpu cpus[kCpus];
// the line's reader alone writes these
uint64_t windowNumber, printed[kCounts];

uint64_t load(const uint64_t &cell)
{
	return __atomic_load_n(&cell, __ATOMIC_RELAXED);
}

void store(uint64_t &cell, uint64_t value)
{
	__atomic_store_n(&cell, value, __ATOMIC_RELAXED);
}

template <typename Count> void on_this_cpu(Count count)
{
	_disable_preemption();
	count(cpus[static_cast<uint32_t>(cpu_number()) % kCpus]);
	_enable_preemption();
}

void bump(Cpu &cpu, uint32_t counter, uint64_t value)
{
	store(cpu.counts[counter], load(cpu.counts[counter]) + value);
}

void raise(Cpu &cpu, uint64_t shared, uint64_t total)
{
	const uint64_t window = load(windowNumber);
	if (load(cpu.maxWindow) != window) {
		store(cpu.sharedMax, 0);
		store(cpu.totalMax, 0);
		store(cpu.maxWindow, window);
	}
	if (shared > load(cpu.sharedMax))
		store(cpu.sharedMax, shared);
	if (total > load(cpu.totalMax))
		store(cpu.totalMax, total);
}
}  // namespace

extern "C" void switch_gpu_job_submitted(const uint64_t *ns, int named_set, uint32_t shared_uses,
					 uint32_t backings, uint32_t siblings)
{
	uint64_t total = 0;
	for (uint32_t p = 0; p < kPhases; ++p)
		total += ns[p];
	on_this_cpu([&](Cpu &cpu) {
		for (uint32_t p = 0; p < kPhases; ++p)
			bump(cpu, kNs + p, ns[p]);
		bump(cpu, kJobs, 1);
		bump(cpu, kSets, named_set ? 1 : 0);
		bump(cpu, kShared, shared_uses);
		bump(cpu, kBackings, backings);
		bump(cpu, kSiblings, siblings);
		bump(cpu, kTotal, total);
		raise(cpu, shared_uses, total);
	});
}

extern "C" void switch_gpu_job_stale(void)
{
	on_this_cpu([](Cpu &cpu) {
		bump(cpu, kStale, 1);
	});
}

extern "C" void switch_gpu_job_invalidated(uint32_t cause)
{
	if (cause < SWITCH_GPU_SET_CAUSES)
		on_this_cpu([cause](Cpu &cpu) {
			bump(cpu, kCauses + cause, 1);
		});
}

extern "C" void switch_gpu_job_window(char *line, size_t size)
{
	uint64_t moved[kCounts];
	for (uint32_t i = 0; i < kCounts; ++i) {
		uint64_t now = 0;
		for (const Cpu &cpu : cpus)
			now += load(cpu.counts[i]);
		moved[i] = now - printed[i];
		printed[i] = now;
	}
	const uint64_t window = load(windowNumber);
	uint64_t sharedMax = 0, totalMax = 0;
	for (const Cpu &cpu : cpus) {
		if (load(cpu.maxWindow) != window)
			continue;
		sharedMax = load(cpu.sharedMax) > sharedMax ? load(cpu.sharedMax) : sharedMax;
		totalMax = load(cpu.totalMax) > totalMax ? load(cpu.totalMax) : totalMax;
	}
	store(windowNumber, window + 1);
	uint64_t changed = 0;
	for (uint32_t c = 0; c < SWITCH_GPU_SET_CAUSES; ++c)
		changed += moved[kCauses + c];
	const uint64_t jobs = moved[kJobs], shared = moved[kShared], backings = moved[kBackings],
		       total = moved[kTotal];
	if (!size)
		return;
	line[0] = 0;
	if (!jobs && !moved[kStale] && !changed)
		return;
	const uint64_t n = jobs ? jobs : 1;
	const uint64_t *ns = moved + kNs, *causes = moved + kCauses;
	// The kernel has no stdio.h: the builtin is the kernel's snprintf there and libc's on the host.
	__builtin_snprintf(line, size,
			   "SubmitJob %llu x mean %llu us (resolve %llu, lock %llu, exec %llu, "
			   "commit %llu) max %llu us, "
			   "set %llu hit %llu stale, shared %llu.%llu per job max %llu, siblings "
			   "%llu, generations ended: "
			   "free %llu layout %llu shared %llu kill %llu replace %llu cpu %llu, "
			   "backings %llu.%llu per job",
			   jobs, total / n / 1000, ns[0] / n / 1000, ns[1] / n / 1000,
			   ns[2] / n / 1000, ns[3] / n / 1000, totalMax / 1000, moved[kSets],
			   moved[kStale], shared / n, shared * 10 / n % 10, sharedMax,
			   moved[kSiblings], causes[SWITCH_GPU_SET_CAUSE_FREE],
			   causes[SWITCH_GPU_SET_CAUSE_LAYOUT], causes[SWITCH_GPU_SET_CAUSE_SHARED],
			   causes[SWITCH_GPU_SET_CAUSE_KILL], causes[SWITCH_GPU_SET_CAUSE_REPLACE],
			   causes[SWITCH_GPU_SET_CAUSE_CPU], backings / n, backings * 10 / n % 10);
}
#endif

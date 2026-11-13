// SPDX-License-Identifier: MIT
// The kernel around gpu_promise_giveup.c and gpu_promise.c's GPU-reader park, as switch_gpu_promise_giveup_test runs
// them: a fake clock and call, a fence pool, channels whose fences the test moves, the hang rule as the deferred
// call's channel sees it, and the spinlock as what it is in XNU (interrupts off), so the interrupts-off stubs throw on
// anything that blocks under it.
#pragma once
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "switch_gpu_interrupts_off_stubs.h"
#define DEVELOPMENT 1
#define TGM_STATS 1
#include "drivers/TegraGM20B/SwitchGpuJobFences.h"
#include "drivers/TegraGM20B/SwitchGpuPathStats.h"
#include "drivers/TegraGM20B/SwitchGpuPromiseGiveUp.h"
#include "drivers/TegraGM20B/TegraGM20BJobABI.h"

typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
#define READ_ONCE(x) (*(const volatile __typeof__(x) *)&(x))
#define noinline __attribute__((noinline))
#define _Static_assert static_assert
#define TGM_FENCE_SLOT (16 * 1024)
#define TGM_FENCE_USER_PAGES 64
#define TGM_FENCE_PROMISE_WORD 512
#define TGM_FENCE_PROMISES 32

// gpu_internal.h's fences_state: the raise/remake guard
inline struct {
	uint32_t made[TGM_FENCE_USER_PAGES][TGM_FENCE_PROMISES];
	uint32_t raising;
} tgm_fence_pages;

namespace fake
{
inline uint64_t now = 0;

// The one call: armed at `due`, or not
struct Call {
	void (*fn)(void *) = nullptr;
	bool pending = false;
	uint64_t due = 0;
	unsigned armed = 0, cancelled = 0, ran = 0, immediate = 0;
};

inline Call call;

// every fence user page, so a promise slot of any page has its word
constexpr size_t kPoolWords = TGM_FENCE_USER_PAGES * TGM_FENCE_SLOT / 4;
inline volatile uint32_t pool[kPoolWords];
inline uint32_t owner = 1;

struct Chan {
	uint64_t id = 0;
	uint32_t error = 0;
	volatile uint32_t *fence_cpu = nullptr;
	switch_gpu_jobs jobs = {};
	char owner[17] = {};
	uint32_t submitted = 0;
	// the hang rule's bookkeeping as the deferred call's checks see it (tgm_chan_check_progress_locked)
	uint64_t stall_ns = 0;
	uint32_t stall_done = 0, strikes = 0;
	int recovery =
		0;  // what its second strike does: 1 kills the channel, 2 resets the engine under the job ahead
	std::vector<uint32_t> asked;
	volatile uint32_t fence = 0;
};

inline std::vector<Chan *> chans;
inline unsigned hangLooks = 0;
inline std::vector<std::pair<uint32_t, uint32_t>> abandoned;
inline unsigned pathAbandoned = 0, directGivenUp = 0, cpuRaised = 0;
// The GPU's power as the test sets it, off from tgm_promise_suspend to tgm_promise_resume
inline bool poweredDown = false;
// Run at a register read and at an abandonment, to hold the call there while another thread acts
inline std::function<void()> atRegisters, atAbandon;
}  // namespace fake

// The hang rule's look and the ACQUIRE probe read the GPU's registers: never while it is powered down
inline void registers(const char *who)
{
	if (fake::poweredDown)
		ADD_FAILURE() << who << " read registers with the GPU powered down";
	if (fake::atRegisters)
		fake::atRegisters();
}

#define tgm_chan fake_tgm_chan

struct fake_tgm_chan : fake::Chan {};

inline uint64_t nvxnu_nanotime()
{
	return fake::now;
}

inline void *nvxnu_work_alloc(void (*fn)(void *), void *)
{
	fake::call.fn = fn;
	return &fake::call;
}

inline void nvxnu_work_schedule_after(void *, uint64_t ns)
{
	fake::call.pending = true;
	fake::call.due = fake::now + ns;
	++fake::call.armed;
}

inline void nvxnu_work_schedule(void *)
{
	fake::call.pending = true;
	fake::call.due = fake::now;
	++fake::call.immediate;
}

inline void nvxnu_work_cancel(void *)
{
	if (fake::call.pending)
		++fake::call.cancelled;
	fake::call.pending = false;
}

// XNU's simple lock: preemption and interrupts off while it is held
struct spinlock_t {
	int held;
};

#define DEFINE_SPINLOCK(n) spinlock_t n = {0}
#define spin_lock_irqsave(l, f) ((f) = switch_linux_irq_save(), (l)->held = 1)
#define spin_unlock_irqrestore(l, f) ((l)->held = 0, switch_linux_irq_restore(f))

inline struct mutex tgm_live_lock;
inline int tgm_live_locked = 0;
inline unsigned tgm_live_takes = 0;

// tgm_live_lock sleeps as the kernel's does: a thread that takes it waits for the call holding it. Taken and given
// through these, as the kernel's lck_mtx is, outside clang's thread-safety analysis of the code under test.
inline std::mutex tgm_live_held;

__attribute__((no_thread_safety_analysis)) inline void tgm_live_take()
{
	tgm_live_held.lock();
}

__attribute__((no_thread_safety_analysis)) inline void tgm_live_give()
{
	tgm_live_held.unlock();
}

inline void mutex_unlock(struct mutex *)
{
	--tgm_live_locked;
	tgm_live_give();
}

#define mutex_lock(m) (mutex_lock(m), tgm_live_take(), ++tgm_live_takes, ++tgm_live_locked)

inline volatile uint32_t *tgm_fence_word(uint32_t offset)
{
	return offset && offset / 4 < fake::kPoolWords && !(offset & 3) ? &fake::pool[offset / 4]
									: nullptr;
}

inline uint32_t tgm_fence_owner(uint32_t)
{
	return fake::owner;
}

inline uint32_t tgm_fence_offset(const volatile uint32_t *word)
{
	return uint32_t((word - fake::pool) * 4);
}

inline fake_tgm_chan *live_by_uid(uint32_t uid)
{
	for (auto *chan : fake::chans)
		if (uint32_t(chan->id) == uid)
			return static_cast<fake_tgm_chan *>(chan);
	return nullptr;
}

inline uint32_t tgm_chan_completed(fake_tgm_chan *chan)
{
	return *chan->fence_cpu;
}

inline void tgm_chan_need(fake_tgm_chan *chan, uint32_t seqno)
{
	interrupts_off::mayBlock("tgm_chan_need (tgm_live_lock's caller)");
	chan->asked.push_back(seqno);
}

// tgm_chan_check_progress's rule for a waiter: a look every TGM_HANG_NS, the channel struck at a look that finds it
// stalled that long, recovered at the next. The recovery is the test's: a kill, or an engine reset that lets the job
// ahead complete.
inline uint64_t tgm_chan_check_progress_locked(fake_tgm_chan *chan, const char *, uint32_t)
{
	constexpr uint64_t kHangNs = 2000ull * 1000 * 1000;
	registers("the hang rule's look");
	const uint32_t done = *chan->fence_cpu;
	++fake::hangLooks;
	if (int32_t(done - chan->submitted) >= 0)
		return 0;
	if (!chan->stall_ns || chan->stall_done != done) {
		chan->stall_ns = fake::now;
		chan->stall_done = done;
		chan->strikes = 0;
		return kHangNs;
	}
	if (fake::now - chan->stall_ns < kHangNs)
		return kHangNs - (fake::now - chan->stall_ns);
	chan->stall_ns = fake::now;
	if (!chan->strikes++)
		return kHangNs;
	if (chan->recovery == 1) {
		chan->error = 1;
		*chan->fence_cpu = chan->submitted;
	} else if (chan->recovery == 2) {
		*chan->fence_cpu = done + 1;
	}
	return kHangNs;
}

inline void tgm_promise_abandon(uint32_t offset, uint32_t value)
{
	if (fake::atAbandon)
		fake::atAbandon();
	fake::abandoned.push_back({offset, value});
}

inline void tgm_promise_kept_first(uint32_t offset, uint32_t value)
{
	for (auto &a : fake::abandoned)
		if (a == std::make_pair(offset, value))
			a = {0, 0};
}

inline int tgm_promise_abandoned(uint32_t offset, uint32_t value)
{
	for (const auto &a : fake::abandoned)
		if (a == std::make_pair(offset, value))
			return 1;
	return 0;
}

inline unsigned presentReleased = 0;

inline void tgm_present_promise_released(uint32_t, uint32_t)
{
	++presentReleased;
}

inline const char *tgm_promise_maker(uint32_t)
{
	return "maker-app";
}

inline unsigned probedReaders = 0;

inline void tgm_acquire_probe_giveup(fake_tgm_chan *, uint64_t, uint64_t)
{
	registers("the ACQUIRE probe");
	interrupts_off::mayBlock("the ACQUIRE probe's look (tgm_live_lock's caller)");
	++probedReaders;
}

inline void tgm_promise_cpu_raised(uint32_t, uint32_t)
{
	++fake::cpuRaised;
}

inline void switch_gpu_path_promise_abandoned(uint64_t)
{
	++fake::pathAbandoned;
}

inline void switch_gpu_path_promise_direct(int what)
{
	if (what == SWITCH_GPU_PATH_DIRECT_GIVEN_UP)
		++fake::directGivenUp;
}

// SwitchGpuWriterPromise.cpp's status of a PROMISE token
inline int switch_gpu_backing_token_status(const switch_gpu_backing_token *token)
{
	if (tgm_promise_abandoned(token->queue, uint32_t(token->value)))
		return -5;
	return int32_t(fake::pool[token->queue / 4] - uint32_t(token->value)) >= 0 ? 1 : 0;
}

struct fake_present {
	fake_tgm_chan *chan;
};

inline fake_present tgm_present;

int tgm_promise_park(uint32_t offset, uint32_t value, const char *waiter, const char *who,
		     const struct switch_gpu_giveup_job *reader, int display);

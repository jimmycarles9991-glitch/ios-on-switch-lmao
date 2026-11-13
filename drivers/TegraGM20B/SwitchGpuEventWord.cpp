// SPDX-License-Identifier: MIT
#include "SwitchGpuEventInternal.h"
#include "SwitchGpuEventStats.h"
#include "SwitchGpuEventWord.h"
#include "TegraGM20BJobABI.h"

namespace
{
uint32_t wordValue(const switch_gpu_event_word *word)
{
	return __atomic_load_n(word->cpu, __ATOMIC_ACQUIRE);
}

#if SWITCH_GPU_EVENT_STATS
struct {
	uint64_t released, byWord, atEnd, waits;
} window;

uint64_t take(uint64_t *counter)
{
	return __atomic_exchange_n(counter, 0, __ATOMIC_RELAXED);
}
#endif
}  // namespace

switch_gpu_event_word *switch_gpu_event_word_create(volatile uint32_t *cpu, uint64_t pool,
						    void (*put)(const volatile uint32_t *))
{
	auto *word = IONewZero(switch_gpu_event_word, 1);
	if (!word)
		return nullptr;
	word->refs = 1;
	word->cpu = cpu;
	word->pool = pool;
	word->put = put;
	return word;
}

void switch_gpu_event_word_retain(switch_gpu_event_word *word)
{
	__atomic_add_fetch(&word->refs, 1u, __ATOMIC_RELAXED);
}

void switch_gpu_event_word_release(switch_gpu_event_word **owned)
{
	auto *word = *owned;
	*owned = nullptr;
	if (!word || __atomic_sub_fetch(&word->refs, 1u, __ATOMIC_ACQ_REL))
		return;
	if (word->put)
		word->put(word->cpu);
	IODelete(word, switch_gpu_event_word, 1);
}

int switch_gpu_event_word_arm(switch_gpu_event_word *word, switch_gpu_event *event,
			      uint32_t channel, uint64_t *base, uint32_t *floor)
{
	auto *s = switch_gpu_events_state();
	if (!s || !word || !event || !channel)
		return s ? -22 : -12;
	int result = 0;
	IOLockLock(s->lock);
	if (word->pending) {
		result = word->channel == channel ? 0 : -16;
	} else if (wordValue(word) >= TegraGM20BEventReleaseLimit - 1) {
		result = -28;
	} else {
		const uint32_t now = wordValue(word);
		uint64_t reached = switch_gpu_event_native_value(event->native);
		if (event->readyThrough > reached)
			reached = event->readyThrough;
		word->base = reached - now;
		word->channel = channel;
		word->last = now;
		word->armed = true;
	}
	*base = word->base;
	*floor = word->last > word->poisoned ? word->last : word->poisoned;
	IOLockUnlock(s->lock);
	return result;
}

bool switch_gpu_event_word_idle(const switch_gpu_event_word *word)
{
	return !__atomic_load_n(&word->pending, __ATOMIC_ACQUIRE);
}

void switch_gpu_event_word_disarm(switch_gpu_event_word *word)
{
	word->armed = false;
}

int switch_gpu_event_signal_release(switch_gpu_event_signal *signal, switch_gpu_event_word *word,
				    uint32_t channel, uint32_t payload)
{
	auto *s = switch_gpu_events_state();
	if (!s || !signal || !word || signal->word)
		return -22;
	IOLockLock(s->lock);
	const uint64_t offset = signal->value - word->base;
	const bool valid = word->armed && word->channel == channel && offset == payload &&
			   payload && offset < TegraGM20BEventReleaseLimit &&
			   payload > word->last && payload > word->poisoned;
	if (valid) {
		__atomic_add_fetch(&word->pending, 1u, __ATOMIC_RELAXED);
		signal->wordPrevious = word->last;
		word->last = payload;
		switch_gpu_event_word_retain(word);
		signal->word = word;
		signal->wordBase = word->base;
		signal->payload = payload;
	}
	IOLockUnlock(s->lock);
	return valid ? 0 : -22;
}

bool switch_gpu_event_use_word(const switch_gpu_event_use *use, uint64_t value,
			       switch_gpu_word_wait *wait)
{
	const switch_gpu_event_signal *signal = use ? use->authority : nullptr;
	if (!signal || !signal->word)
		return false;
	const uint64_t offset = value - signal->wordBase;
	if (!offset || offset > signal->payload)
		return false;
	*wait = {signal->word->pool, uint32_t(offset), signal->token.queue};
	switch_gpu_event_stats_waited();
	return true;
}

int switch_gpu_event_word_result_locked(const switch_gpu_event_signal *signal)
{
	const auto *word = signal->word;
	if (signal->payload <= word->poisoned || wordValue(word) < signal->payload)
		return 0;
	return signal->dependencies ? switch_gpu_event_waits_producers_locked(signal->dependencies)
				    : 1;
}

void switch_gpu_event_word_settle_locked(switch_gpu_event_signal *signal, int result, bool released)
{
	auto *word = signal->word;
	__atomic_sub_fetch(&word->pending, 1u, __ATOMIC_RELEASE);
	switch_gpu_event_stats_settled(released);
	if (released || wordValue(word) >= signal->payload)
		return;
	__atomic_store_n(word->cpu, signal->payload, __ATOMIC_RELEASE);
	__asm__ volatile("dsb sy" ::: "memory");
	if (result < 0 && signal->payload > word->poisoned)
		word->poisoned = signal->payload;
}

void switch_gpu_event_word_aborted_locked(switch_gpu_event_signal *signal)
{
	auto *word = signal->word;
	__atomic_sub_fetch(&word->pending, 1u, __ATOMIC_RELEASE);
	if (signal->wordPrevious < word->last)
		word->last = signal->wordPrevious;
}

#if SWITCH_GPU_EVENT_STATS
extern "C" void switch_gpu_event_stats_released(uint32_t signals)
{
	__atomic_fetch_add(&window.released, signals, __ATOMIC_RELAXED);
}

extern "C" void switch_gpu_event_stats_settled(int by_word)
{
	__atomic_fetch_add(by_word ? &window.byWord : &window.atEnd, 1, __ATOMIC_RELAXED);
}

extern "C" void switch_gpu_event_stats_waited(void)
{
	__atomic_fetch_add(&window.waits, 1, __ATOMIC_RELAXED);
}

extern "C" void switch_gpu_event_stats_window(char *line, size_t size)
{
	const uint64_t released = take(&window.released), byWord = take(&window.byWord);
	const uint64_t atEnd = take(&window.atEnd), waits = take(&window.waits);
	if (!size)
		return;
	line[0] = 0;
	if (!released && !byWord && !atEnd && !waits)
		return;
	// The kernel has no stdio.h: the builtin is the kernel's snprintf there and libc's on the host.
	__builtin_snprintf(line, size,
			   "%llu signals released mid-job: %llu completed from their word, %llu at "
			   "their job's end; "
			   "%llu waits acquired on a word",
			   (unsigned long long)released, (unsigned long long)byWord,
			   (unsigned long long)atEnd, (unsigned long long)waits);
}
#endif

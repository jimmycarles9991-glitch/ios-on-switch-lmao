// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include "SwitchGpuEventClient.h"
#include "SwitchGpuEventWord.h"
#include "TegraGM20BJobABI.h"

void SwitchGpuEventClient::attachWords(volatile uint32_t *page, uint64_t gpu, uint64_t pool,
				       void (*hold)(const volatile uint32_t *),
				       void (*put)(const volatile uint32_t *))
{
	IOLockLock(lock_);
	words_ = {page, gpu, pool, hold, put};
	IOLockUnlock(lock_);
}

int SwitchGpuEventClient::word(uint64_t handle, uint32_t channel, uint64_t *address, uint64_t *base,
			       uint32_t *floor)
{
	IOLockLock(lock_);
	const uint32_t slot = uint32_t(handle % kHandles);
	switch_gpu_event *event = find(handle);
	auto &entry = entries_[slot];
	int result = closed_                        ? -19
		     : !event || !words_.page       ? -22
		     : slot >= TegraGM20BEventWords ? -28
						    : 0;
	if (!result && !entry.word) {
		volatile uint32_t *cpu = words_.page + TegraGM20BEventWordFirst + slot;
		entry.word = switch_gpu_event_word_create(
			cpu, words_.pool + (TegraGM20BEventWordFirst + slot) * 4, words_.put);
		if (entry.word && words_.hold)
			words_.hold(cpu);
		result = entry.word ? 0 : -12;
	}
	switch_gpu_event_word *word = result ? nullptr : entry.word;
	if (word) {
		switch_gpu_event_word_retain(word);
		switch_gpu_event_retain(event);
	}
	const uint64_t gpu = words_.gpu + (TegraGM20BEventWordFirst + slot) * 4;
	IOLockUnlock(lock_);
	if (!word)
		return result;
	result = switch_gpu_event_word_arm(word, event, channel, base, floor);
	*address = gpu;
	switch_gpu_event_release(&event);
	switch_gpu_event_word_release(&word);
	return result;
}

switch_gpu_event_word *SwitchGpuEventClient::wordOf(uint64_t handle)
{
	IOLockLock(lock_);
	switch_gpu_event_word *word = find(handle) ? entries_[handle % kHandles].word : nullptr;
	if (word)
		switch_gpu_event_word_retain(word);
	IOLockUnlock(lock_);
	return word;
}

int SwitchGpuEventJob::release(SwitchGpuEventClient &client, uint64_t handle, uint32_t channel,
			       uint32_t payload)
{
	switch_gpu_event_word *word = client.wordOf(handle);
	const int result = word ? switch_gpu_event_signal_release(signals_[signalCount_ - 1], word,
								  channel, payload)
				: -22;
	switch_gpu_event_word_release(&word);
	released_ |= result == 0;
	return result;
}

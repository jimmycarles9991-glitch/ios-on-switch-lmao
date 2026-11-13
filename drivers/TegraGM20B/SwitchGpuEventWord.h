// SPDX-License-Identifier: MIT
// An imported event's word in its connection's fence page (TegraGM20BJobABI.h method 40): the bound channel's jobs release
// a signal's payload into it after the work before the signal, and the event layer completes that signal from the word on
// the release's nonstall interrupt. The word only grows: its one writer is the bound channel, whose released payloads grow
// in submission order (switch_gpu_event_signal_release refuses the rest) and run in its GPFIFO order, so no reduction is
// needed. A signal whose job ended without its release (a killed channel) moves the word to its payload, as a killed
// channel's fence moves past its seqnos, and later signals at or below that never complete from the word.
#pragma once
#include <stdint.h>
#include "SwitchGpuWordWait.h"

struct switch_gpu_event;
struct switch_gpu_event_signal;
struct switch_gpu_event_use;

struct switch_gpu_event_word {
	uint32_t refs;
	uint32_t pending;  // signals released here, reserved or committed and not yet complete
	volatile uint32_t *cpu;
	uint64_t
		pool;  // byte offset in the fence pool: a channel's view of it is its address space's fence view plus this
	void (*put)(const volatile uint32_t *);
	uint64_t base;
	uint32_t channel, last, poisoned;
	bool armed;
};

switch_gpu_event_word *switch_gpu_event_word_create(volatile uint32_t *cpu, uint64_t pool,
						    void (*put)(const volatile uint32_t *));
void switch_gpu_event_word_retain(switch_gpu_event_word *word);
void switch_gpu_event_word_release(switch_gpu_event_word **word);
// 0 with *base and *floor; -16: another channel's signals are outstanding; -28: the word passed half its range
int switch_gpu_event_word_arm(switch_gpu_event_word *word, switch_gpu_event *event,
			      uint32_t channel, uint64_t *base, uint32_t *floor);
// No signal released here is outstanding; once a word's handle is released this stays true
bool switch_gpu_event_word_idle(const switch_gpu_event_word *word);
// An idle word given to its slot's next handle: nothing releases into it before that handle's method 40
void switch_gpu_event_word_disarm(switch_gpu_event_word *word);

// Between signal_prepare and kick: the job's pushes release `payload` into `word` for the signal. -22 where that is
// not this signal's payload on the word for `channel`.
int switch_gpu_event_signal_release(switch_gpu_event_signal *signal, switch_gpu_event_word *word,
				    uint32_t channel, uint32_t payload);
// A job waiting on `use` for `value` acquires on the word instead of its producer's fence: true with that acquire
bool switch_gpu_event_use_word(const switch_gpu_event_use *use, uint64_t value,
			       switch_gpu_word_wait *wait);

// The events lock held, from retirement
int switch_gpu_event_word_result_locked(const switch_gpu_event_signal *signal);
void switch_gpu_event_word_settle_locked(switch_gpu_event_signal *signal, int result,
					 bool released);
void switch_gpu_event_word_aborted_locked(switch_gpu_event_signal *signal);

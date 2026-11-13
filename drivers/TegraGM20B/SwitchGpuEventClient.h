// SPDX-License-Identifier: MIT
#pragma once
#include <IOKit/IOLocks.h>
#include "SwitchGpuSharedEvent.h"
#include "SwitchGpuEventChannel.h"
#include "SwitchGpuTimeline.h"
#include "SwitchGpuWordWait.h"
#include "TegraGM20BABI.h"

struct switch_gpu_event_word;
struct TegraGM20BAbiSignalRelease;

class SwitchGpuEventClient
{
public:
	using Callback = void (*)(void *, uint64_t, int);
	bool initialize();
	void stop();
	void destroy();
	int import(void *task, uint32_t port, uint64_t *handle, uint64_t *value);
	int release(uint64_t handle);
	int capture(const TegraGM20BAbiEventUse *uses, uint32_t count, switch_gpu_event **events);
	int probe(uint64_t handle, uint64_t value, bool terminal = false);
	int observe(uint64_t handle, uint64_t value, uint64_t cookie, Callback callback,
		    void *context, bool terminal = false);
	int cancel(uint64_t handle, uint64_t cookie);
	// The connection's fence page, as the kernel, the connection's GPU address space and the fence pool see it
	void attachWords(volatile uint32_t *page, uint64_t gpu, uint64_t pool,
			 void (*hold)(const volatile uint32_t *),
			 void (*put)(const volatile uint32_t *));
	// TegraGM20BJobABI.h method 40
	int word(uint64_t handle, uint32_t channel, uint64_t *address, uint64_t *base,
		 uint32_t *floor);
	// The handle's word, retained, or null when method 40 never made it
	switch_gpu_event_word *wordOf(uint64_t handle);

private:
	static constexpr uint32_t kHandles = 256, kWatches = 128;

	struct Entry {
		uint64_t handle;
		switch_gpu_event *event;
		switch_gpu_event_word
			*word;  // kept past the handle's release, for the slot's next handle
	} entries_[kHandles] = {};

	struct Words {
		volatile uint32_t *page;
		uint64_t gpu, pool;
		void (*hold)(const volatile uint32_t *), (*put)(const volatile uint32_t *);
	} words_ = {};
	struct Watch;
	IOLock *lock_ = nullptr;
	Watch *watches_ = nullptr;
	uint64_t next_ = 1;
	uint32_t count_ = 0;
	bool closed_ = false;
	switch_gpu_event *find(uint64_t handle) const;
	bool unlink(Watch *watch);
	static void drop(Watch *watch);
	static void completed(void *context, uint64_t cookie, int result);
};

class SwitchGpuEventJob
{
public:
	// releases: one per signal (TegraGM20BJobABI.h), null when the job releases none mid-job; channel: the job's
	int prepare(SwitchGpuEventClient &client, const TegraGM20BAbiSubmitEvents *input,
		    const TegraGM20BAbiSignalRelease *releases = nullptr, uint32_t channel = 0);
	int prepareKernel(const switch_gpu_event_submission *input);
	int sequence(SwitchGpuEventChannel &channel);
	int bind(const switch_gpu_backing_token *completion);
	int captureDependencies(switch_gpu_backing_token *, uint32_t count);
	int inheritDependencies();
	int append(switch_gpu_backing_token *dependencies, uint32_t capacity,
		   uint32_t *count) const;
	void commit();
	void clear();

	bool signals() const
	{
		return signalCount_ != 0;
	}

	// A semantic fence (its own event waits, or ones its dependencies' fences pass on) is bound to the job's
	// completion; its batch is read again only when a completion is published, as the interrupt pass's end does.
	bool semanticFence() const
	{
		return record_ != nullptr;
	}

	// Its waits that acquire on an event's word, and whether its pushes release signals mid-job
	const switch_gpu_word_wait *words() const
	{
		return words_;
	}

	uint32_t wordCount() const
	{
		return wordCount_;
	}

	bool released() const
	{
		return released_;
	}

private:
	SwitchGpuEventChannel *channel_ = nullptr;
	SwitchGpuEventChannel::Record *record_ = nullptr;
	switch_gpu_event_waits *waits_ = nullptr;
	switch_gpu_timeline_semantic *semantic_ = nullptr;
	bool semanticBound_ = false, kernelPrepared_ = false;
	switch_gpu_event_signal **borrowedSignals_ = nullptr;
	switch_gpu_backing_token *captured_ = nullptr;
	uint32_t capturedCount_ = 0;
	static void completed(void *, int);
	switch_gpu_event_use *uses_[TegraGM20BMaxJobEvents] = {};
	switch_gpu_event *events_[TegraGM20BMaxJobEvents] = {};
	switch_gpu_event_signal *signals_[TegraGM20BMaxJobEvents] = {};
	switch_gpu_backing_token tokens_[TegraGM20BMaxJobEvents] = {};
	switch_gpu_word_wait words_[TegraGM20BMaxJobEvents] = {};
	bool onWord_[TegraGM20BMaxJobEvents] = {};  // tokens_[i]'s wait acquires on a word instead
	uint32_t eventCount_ = 0, signalCount_ = 0, tokenCount_ = 0, waitCount_ = 0, wordCount_ = 0;
	bool released_ = false;
	int release(SwitchGpuEventClient &client, uint64_t handle, uint32_t channel,
		    uint32_t payload);
};

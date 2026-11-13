// SPDX-License-Identifier: MIT
// A watched present (SwitchGpuPresentWatch.h) over producers whose jobs may end quiet (switch_gpu_quiet_model.h), as
// gpu_present.c drives it: the offer arms the watch, asks each producer's channel for the interrupt at its seqno
// (tgm_chan_need_tokens, by switch_gpu_quiet_token_on), then looks at the producer once more. A producer's pass makes
// the watch due and the pass's end delivers it; the work item takes what the last look found, a kill or a teardown. A
// present the watch does not take is the fence-only present job: its acquires wait for the producers on the GPU, and its
// fence, on a kernel channel, always interrupts. The deadline (gpu_present_deadline.c watch_late) never delivers. A
// frame behind a writer promise is watched PROMISED until the job that keeps it is submitted (gpu_promise.c), then on
// that job; the deadline gives up a promise still unkept, and the frame is dropped (`failed`: the display's
// dropLostFrame keeps the frame before it on the panel). `panel` is the frame window A shows: a delivered present
// flips it to the offered frame, from the nonstall filter first when its view of a GM20B producer sees it end
// (SwitchGpuPresentFilter.cpp: attached before each arm, detached at each claim), and a failed one leaves it.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <utility>
#include <vector>
#include "../SwitchGpuPresentWatch.h"
#include "switch_gpu_quiet_model.h"

namespace present_model
{
using quiet_model::Channel;

constexpr uint64_t kA = (uint64_t(2) << 32) | 17u;  // reset generation 3, uid 17
constexpr uint64_t kB = 18u;                        // reset generation 1, uid 18
constexpr uint64_t kWakeGraceNs = 50000000;         // TGM_PRESENT_WAKE_GRACE_NS

struct Producer {
	uint64_t id;  // tgm_chan.id: (reset generation - 1) << 32 | uid
	Channel channel;

	switch_gpu_backing_token token(uint32_t seqno) const
	{
		return {seqno, (id >> 32) + 1, SWITCH_GPU_ENGINE_GM20B, uint32_t(id)};
	}
};

enum class Deadline {
	Idle,      // nothing armed: delivered, or its work item has it
	Producer,  // still running: the pending producer's hang path
	Follows,   // its interrupt is queued: the hang path at the wake's position (`followed`)
	Rearmed,   // its wake entry is owed by the channel's owner
	Grace,     // its interrupt is due: the pass's grace
	Panic,     // lost
};

class Display
{
public:
	using Step = std::function<void()>;

	explicit Display(std::vector<Producer *> producers) : producers_(std::move(producers))
	{
		current_ = this;
		for (auto *producer : producers_)
			producer->channel.passed = [this, producer] {
				pass(*producer);
			};
	}

	// tgm_present_watch_dependencies; 1: the caller queues the present job
	int offer(const switch_gpu_backing_token *tokens, uint32_t count, const Step &afterArm = {},
		  const Step &afterAsk = {})
	{
		if (!switch_gpu_present_watchable(tokens, count)) {
			job.assign(tokens, tokens + count);
			return 1;
		}
		late = 0;
		deadlineAt = 0;
		switch_gpu_backing_token watched = tokens[0];
		for (const auto &k : keptJobs)
			if (watched.engine == SWITCH_GPU_ENGINE_PROMISE &&
			    k.first == promise(watched))
				watched = k.second;
		if (watched.engine == SWITCH_GPU_ENGINE_PROMISE) {
			switch_gpu_present_watch_promise(&watch, &watched);
			parked = true;
		} else {
			deadlineAt = clock + kPresentNs;
			view = true;
			switch_gpu_present_watch_arm(&watch, &watched);
		}
		if (afterArm)
			afterArm();
		if (asks)
			needTokens(&watched, 1);
		if (afterAsk)
			afterAsk();
		if (physical(&watch.token))
			work();
		return 0;
	}

	// tgm_chan_need_tokens over the live channels
	void needTokens(const switch_gpu_backing_token *tokens, uint32_t count)
	{
		for (uint32_t i = 0; i < count; ++i)
			for (auto *producer : producers_)
				if (switch_gpu_quiet_token_on(&tokens[i], producer->id))
					producer->channel.need(uint32_t(tokens[i].value));
	}

	// tgm_present_promise_kept: SubmitJob submitted the job that keeps the promise
	void kept(uint32_t offset, uint32_t value, const switch_gpu_backing_token &producer)
	{
		keptJobs.push_back({{offset, value}, producer});
		if (!switch_gpu_present_watch_kept(&watch, offset, value, &producer))
			return;
		view = true;
		switch_gpu_present_watch_rearm(&watch);
		parked = false;
		deadlineAt = clock + kPresentNs;
		if (asks)
			needTokens(&producer, 1);
		if (physical(&producer))
			work();
	}

	// releasePromise: a cancel, or a kill or close that abandons it first; then tgm_present_promise_released
	void cpuRelease(uint32_t offset, uint32_t value, bool abandon)
	{
		if (abandon)
			abandoned.insert({offset, value});
		words[offset] = value;
		if (switch_gpu_present_watch_promised(&watch, offset, value))
			work();
	}

	// tgm_present_filter in the nonstall line's primary filter, ahead of the work loop's pass: the display's callout
	// writes window A once the watched producer is complete and visible, once per view
	void filter()
	{
		if (!view || filterCalled || watch.token.engine != SWITCH_GPU_ENGINE_GM20B ||
		    physical(&watch.token) != 1)
			return;
		filterCalled = true;
		++filterFlips;
		panel = offered;
	}

	// the owners' thread calls
	void ownerWakes()
	{
		for (auto *producer : producers_)
			producer->channel.ownerWake();
	}

	// The present job's acquires pass once every producer is there; then its own fence interrupts.
	bool runJob()
	{
		for (const auto &token : job)
			if (physical(&token) != 1)
				return false;
		job.clear();
		++jobInterrupts;
		++presented;
		panel = offered;
		return true;
	}

	// tgm_chan_del: no pass of the channel comes after it
	void teardown(const Producer &producer)
	{
		if (switch_gpu_present_watch_fenced(&watch, producer.id, producer.channel.fence))
			work();
	}

	// SwitchGpuPromiseGiveUp.h's call for the watch parked on its promise, a second after the offer: a word a job the
	// watch never heard of released is delivered (tgm_present_promise_released); one still short is given up (marked,
	// raised, its line), and the work item drops the frame
	void giveUp()
	{
		if (!parked || watch.state != SWITCH_GPU_PRESENT_WATCH_PROMISED)
			return;
		parked = false;
		if (physical(&watch.token) != 1) {
			abandoned.insert(promise(watch.token));
			words[uint32_t(watch.token.queue)] = uint32_t(watch.token.value);
			++reported;
		}
		work();
	}

	// gpu_present_deadline.c at or after TGM_PRESENT_NS: a PROMISED watch has none
	Deadline deadline(uint64_t now)
	{
		if (watch.state != SWITCH_GPU_PRESENT_WATCH_ARMED)
			return Deadline::Idle;
		const int status = physical(&watch.token);
		if (!status)
			return Deadline::Producer;
		const Producer *producer = named(&watch.token);
		int where = SWITCH_GPU_QUIET_UNASKED;
		followed = uint32_t(watch.token.value);
		if (status > 0 && producer) {
			int recorded = 0;
			followed = switch_gpu_jobs_announced_by(&producer->channel.jobs, followed,
								&recorded);
			where = switch_gpu_quiet_interrupt(
				&producer->channel.quiet, uint32_t(watch.token.value),
				producer->channel.fence, &followed, recorded);
		}
		if (where == SWITCH_GPU_QUIET_QUEUED)
			return Deadline::Follows;
		if (where == SWITCH_GPU_QUIET_OWED)
			return Deadline::Rearmed;
		if (!late)
			late = now;
		return now - late < kWakeGraceNs ? Deadline::Grace : Deadline::Panic;
	}

	switch_gpu_present_watch watch{};
	std::vector<switch_gpu_backing_token> job;
	uint32_t syncpoints[256] = {};
	bool asks = true;
	unsigned presented = 0, failed = 0, reported = 0, workItems = 0, jobInterrupts = 0,
		 filterFlips = 0;
	static constexpr uint64_t kPresentNs =
		1000000000;  // TGM_PRESENT_NS: the watched offer's deadline
	uint64_t clock = 0, deadlineAt = 0;
	int offered = 2, panel = 1;
	std::map<uint32_t, uint32_t> words;  // a writer promise's word by its fence-pool offset
	std::set<std::pair<uint32_t, uint32_t>> abandoned;
	uint64_t late = 0;
	uint32_t followed = 0;
	bool parked = false;  // the watch parked on its promise (tgm_present.promise_parked)

private:
	static inline Display *current_ = nullptr;
	std::vector<Producer *> producers_;
	bool view = false, filterCalled = false;
	std::vector<std::pair<std::pair<uint32_t, uint32_t>, switch_gpu_backing_token>> keptJobs;

	static std::pair<uint32_t, uint32_t> promise(const switch_gpu_backing_token &token)
	{
		return {token.queue, uint32_t(token.value)};
	}

	static const Producer *named(const switch_gpu_backing_token *token)
	{
		for (auto *producer : current_->producers_)
			if (switch_gpu_quiet_token_on(token, producer->id))
				return producer;
		return nullptr;
	}

	static int physical(const switch_gpu_backing_token *token)
	{
		if (token->engine == SWITCH_GPU_ENGINE_PROMISE)
			return current_->abandoned.count(promise(*token))
				       ? -5
				       : int32_t(current_->words[token->queue] -
						 uint32_t(token->value)) >= 0;
		if (token->engine != SWITCH_GPU_ENGINE_GM20B)
			return int32_t(current_->syncpoints[token->queue] -
				       uint32_t(token->value)) >= 0;
		const Producer *producer = named(token);
		return producer ? int32_t(producer->channel.fence - uint32_t(token->value)) >= 0
				: -19;
	}

	// present_watch_fenced in the producer's pass, then tgm_present_pass at its end
	void pass(const Producer &producer)
	{
		switch_gpu_present_watch_fenced(&watch, producer.id, producer.channel.fence);
		if (watch.state != SWITCH_GPU_PRESENT_WATCH_DUE)
			return;
		if (switch_gpu_present_watch_take_visible(&watch, physical, physical))
			deliver(true);
		else
			work();
	}

	// present_watch_work
	void work()
	{
		++workItems;
		const int found = switch_gpu_present_watch_claim(&watch, physical, physical);
		failed += found == SWITCH_GPU_PRESENT_WATCH_FAILED;
		if (found != SWITCH_GPU_PRESENT_WATCH_PENDING)
			deliver(found != SWITCH_GPU_PRESENT_WATCH_FAILED);
	}

	// the claim detaches the filter's view; a delivered frame flips window A unless the filter already did
	void deliver(bool shown)
	{
		view = false;
		filterCalled = false;
		switch_gpu_present_watch_reset(&watch);
		++presented;
		if (shown)
			panel = offered;
	}
};
}  // namespace present_model

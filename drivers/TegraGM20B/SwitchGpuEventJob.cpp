// SPDX-License-Identifier: MIT
#include "SwitchGpuEventClient.h"
#include "SwitchGpuEventWord.h"
#include "TegraGM20BJobABI.h"

int SwitchGpuEventJob::prepare(SwitchGpuEventClient &client, const TegraGM20BAbiSubmitEvents *input,
			       const TegraGM20BAbiSignalRelease *releases, uint32_t channel)
{
	clear();
	if (!input)
		return releases ? -22 : 0;
	const auto *uses = reinterpret_cast<const TegraGM20BAbiEventUse *>(input + 1);
	const uint32_t total = input->wait_count + input->signal_count;
	if (input->wait_count > TegraGM20BMaxJobEvents ||
	    input->signal_count > TegraGM20BMaxJobEvents - input->wait_count)
		return -22;
	int result = client.capture(uses, total, events_);
	if (result)
		return result;
	eventCount_ = total;
	waitCount_ = input->wait_count;
	for (uint32_t i = 0; i < input->wait_count; ++i) {
		result = switch_gpu_event_resolve_use(events_[i], uses[i].value,
						      &tokens_[tokenCount_], &uses_[tokenCount_]);
		if (result == 2) {
			onWord_[tokenCount_] = switch_gpu_event_use_word(
				uses_[tokenCount_], uses[i].value, &words_[wordCount_]);
			wordCount_ += onWord_[tokenCount_];
			++tokenCount_;
		}
		if (result <= 0)
			return result ? result : -19;
	}
	for (uint32_t i = input->wait_count; i < total; ++i) {
		result = switch_gpu_event_signal_prepare(events_[i], uses[i].value,
							 &signals_[signalCount_]);
		if (result)
			return result;
		++signalCount_;
		const TegraGM20BAbiSignalRelease *release =
			releases ? &releases[i - input->wait_count] : nullptr;
		if (release && (release->flags & ~uint32_t(TegraGM20BSignalReleased)))
			return -22;
		if (release && (release->flags & TegraGM20BSignalReleased))
			if (const int refused = this->release(client, uses[i].handle, channel,
							      release->payload))
				return refused;
	}
	return 0;
}

int SwitchGpuEventJob::prepareKernel(const switch_gpu_event_submission *input)
{
	clear();
	kernelPrepared_ = true;
	if (!input)
		return 0;
	if (input->dependency_count > TegraGM20BMaxJobEvents ||
	    input->signal_count > TegraGM20BMaxJobEvents ||
	    (input->dependency_count && !input->dependencies) ||
	    (input->signal_count && !input->signals))
		return -22;
	for (uint32_t i = 0; i < input->dependency_count; ++i) {
		const int result = switch_gpu_backing_token_retain(&input->dependencies[i]);
		if (result)
			return result;
		tokens_[tokenCount_++] = input->dependencies[i];
	}
	borrowedSignals_ = input->signals;
	for (uint32_t i = 0; i < input->signal_count; ++i) {
		if (!input->signals[i])
			return -22;
		signals_[signalCount_++] = input->signals[i];
	}
	if (!input->waits && !signalCount_)
		return 0;
	int result = switch_gpu_event_waits_prepare(nullptr, 0, &waits_);
	if (!result && input->waits)
		result = switch_gpu_event_waits_merge_uncommitted(waits_, input->waits);
	return result;
}

int SwitchGpuEventJob::sequence(SwitchGpuEventChannel &channel)
{
	channel_ = &channel;
	if (kernelPrepared_) {
		if (!waits_)
			return 0;
		const int result = channel.adopt(waits_, &record_);
		return result ? result : switch_gpu_timeline_semantic_allocate(&semantic_);
	}
	if (!waitCount_ && !signalCount_)
		return 0;
	const int result = channel.prepare(uses_, tokenCount_, &waits_, &record_);
	return result ? result : switch_gpu_timeline_semantic_allocate(&semantic_);
}

int SwitchGpuEventJob::captureDependencies(switch_gpu_backing_token *tokens, uint32_t count)
{
	if (!switch_gpu_backing_has_semantics())
		return 0;
	if (capturedCount_)
		return -22;
	captured_ = tokens;
	for (uint32_t i = 0; i < count; ++i) {
		const int covered = switch_gpu_event_waits_covers(waits_, &tokens[i]);
		if (covered < 0)
			return covered;
		if (covered)
			continue;
		if (capturedCount_ == TegraGM20BMaxJobDependencies)
			return -28;
		const int result = switch_gpu_backing_token_retain(&tokens[i]);
		if (result)
			return result;
		captured_[capturedCount_++] = tokens[i];
	}
	return capturedCount_ ? 1 : 0;
}

int SwitchGpuEventJob::inheritDependencies()
{
	switch_gpu_event_waits *inferred = nullptr;
	int result =
		switch_gpu_event_waits_prepare_dependencies(captured_, capturedCount_, &inferred);
	for (uint32_t i = 0; i < capturedCount_; ++i)
		switch_gpu_backing_token_release(&captured_[i]);
	capturedCount_ = 0;
	captured_ = nullptr;
	if (!result && inferred) {
		if (waits_) {
			result = switch_gpu_event_waits_merge_uncommitted(waits_, inferred);
		} else {
			result = channel_->adopt(inferred, &record_);
			if (!result) {
				waits_ = inferred;
				inferred = nullptr;
			}
		}
	}
	switch_gpu_event_waits_abort(&inferred);
	if (!result && waits_)
		result = switch_gpu_timeline_semantic_allocate(&semantic_);
	return result;
}

void SwitchGpuEventJob::completed(void *context, int result)
{
	auto *node = static_cast<switch_gpu_timeline_semantic *>(context);
	switch_gpu_timeline_semantic_finish(&node, result);
}

int SwitchGpuEventJob::bind(const switch_gpu_backing_token *completion)
{
	if (record_) {
		if (const int result = switch_gpu_event_waits_bind(waits_, completion))
			return result;
		if (!semanticBound_) {
			if (const int result = switch_gpu_timeline_semantic_reserve(
				    completion, waits_, &semantic_))
				return result;
			if (const int result =
				    switch_gpu_event_waits_completion(waits_, completed, semantic_))
				return result;
			semanticBound_ = true;
		}
		channel_->bind(record_, uint32_t(completion->value));
	}
	for (uint32_t i = 0; i < signalCount_; ++i) {
		if (waits_)
			if (const int result = switch_gpu_event_signal_inherit(signals_[i], waits_))
				return result;
		if (const int result = switch_gpu_event_signal_bind(signals_[i], completion))
			return result;
	}
	return 0;
}

int SwitchGpuEventJob::append(switch_gpu_backing_token *dependencies, uint32_t capacity,
			      uint32_t *count) const
{
	if (*count > capacity || tokenCount_ > capacity - *count)
		return -28;
	for (uint32_t i = 0; i < tokenCount_; ++i)
		if (!onWord_[i])
			dependencies[(*count)++] = tokens_[i];
	return 0;
}

void SwitchGpuEventJob::commit()
{
	if (record_) {
		channel_->commit(&record_);
		semantic_ = nullptr;
		semanticBound_ = false;
		switch_gpu_event_waits_commit(&waits_);
	}
	for (uint32_t i = 0; i < signalCount_; ++i) {
		switch_gpu_event_signal_commit(&signals_[i]);
		if (borrowedSignals_)
			borrowedSignals_[i] = nullptr;
	}
	signalCount_ = 0;
}

void SwitchGpuEventJob::clear()
{
	for (uint32_t i = 0; i < capturedCount_; ++i)
		switch_gpu_backing_token_release(&captured_[i]);
	capturedCount_ = 0;
	captured_ = nullptr;
	if (semanticBound_) {
		semantic_ = nullptr;
		semanticBound_ = false;
	} else {
		switch_gpu_timeline_semantic_finish(&semantic_, -125);
	}
	if (!kernelPrepared_)
		for (uint32_t i = 0; i < signalCount_; ++i)
			switch_gpu_event_signal_abort(&signals_[i]);
	if (record_) {
		channel_->abort(&record_);
		switch_gpu_event_waits_abort(&waits_);
	} else if (kernelPrepared_) {
		switch_gpu_event_waits_abort(&waits_);
	} else {
		switch_gpu_event_waits_release(&waits_);
	}
	for (uint32_t i = 0; i < tokenCount_; ++i) {
		if (kernelPrepared_)
			switch_gpu_backing_token_release(&tokens_[i]);
		else
			switch_gpu_event_use_release(&uses_[i]);
	}
	kernelPrepared_ = false;
	borrowedSignals_ = nullptr;
	for (uint32_t i = 0; i < eventCount_; ++i)
		switch_gpu_event_release(&events_[i]);
	for (uint32_t i = 0; i < tokenCount_; ++i)
		onWord_[i] = false;
	eventCount_ = signalCount_ = tokenCount_ = waitCount_ = wordCount_ = 0;
	released_ = false;
	channel_ = nullptr;
}

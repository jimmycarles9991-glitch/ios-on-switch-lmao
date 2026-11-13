// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include "drivers/TegraGM20B/SwitchGpuEventClient.h"
#include "drivers/TegraGM20B/SwitchGpuEventWord.h"
#include "drivers/TegraGM20B/TegraGM20BJobABI.h"

struct switch_gpu_event {
	unsigned references = 1;
	uint64_t value = 0;
	int state = 0, terminalState = 0;
};

struct switch_gpu_event_use {
	unsigned refs = 1;
	switch_gpu_backing_token token = {};
};

struct switch_gpu_event_waits {
	unsigned refs = 1;
	switch_gpu_backing_token token = {};
	std::vector<switch_gpu_event_use *> uses;
	switch_gpu_event_waits *previous = nullptr;
	int result = 0;
	void (*completed)(void *, int) = nullptr;
	void *context = nullptr;
	std::vector<switch_gpu_backing_token> dependencies;
};

struct switch_gpu_timeline_semantic {
	bool bound = false;
};

struct switch_gpu_event_signal {
	switch_gpu_event *event;
	switch_gpu_backing_token token = {};
	switch_gpu_event_waits *waits = nullptr;
};

struct switch_gpu_event_watch {
	switch_gpu_event *event;
	void (*callback)(void *, uint64_t, int);
	void *context;
	uint64_t cookie;
	std::atomic<bool> delivered{false};
	bool terminal = false;
};

namespace
{
switch_gpu_event source;
std::vector<switch_gpu_event_watch *> watches;
std::vector<switch_gpu_event_signal *> committed;
std::vector<switch_gpu_event_waits *> batches;
std::function<void()> onObserve;
int observeError, prepareError, tokenReferences;
bool inlineReady;
int semanticNodes;
switch_gpu_backing_token semanticSource = {};
int semanticResult;

bool sameToken(const switch_gpu_backing_token &a, const switch_gpu_backing_token &b)
{
	return a.engine == b.engine && a.queue == b.queue &&
	       a.reset_generation == b.reset_generation && a.value == b.value;
}

void completeBatch(switch_gpu_event_waits *batch, int result)
{
	batch->result = result;
	auto callback = batch->completed;
	batch->completed = nullptr;
	if (callback)
		callback(batch->context, result);
}

void deliver(switch_gpu_event_watch *watch, int result)
{
	if (!watch->delivered.exchange(true))
		watch->callback(watch->context, watch->cookie, result);
}

struct Notice {
	std::atomic<unsigned> count{0};
	uint64_t cookie = 0;
	int result = 0;
};

void notified(void *context, uint64_t cookie, int result)
{
	auto &notice = *static_cast<Notice *>(context);
	notice.cookie = cookie;
	notice.result = result;
	++notice.count;
}

struct EventClient : testing::Test {
	SwitchGpuEventClient client;
	uint64_t handle = 0;

	void SetUp() override
	{
		source = {};
		watches.clear();
		committed.clear();
		batches.clear();
		onObserve = {};
		observeError = prepareError = tokenReferences = 0;
		inlineReady = false;
		semanticNodes = 0;
		semanticSource = {};
		semanticResult = 0;
		ASSERT_TRUE(client.initialize());
		uint64_t value = 0;
		ASSERT_EQ(client.import(this, 7, &handle, &value), 0);
	}

	void TearDown() override
	{
		client.destroy();
		for (auto *signal : committed) {
			switch_gpu_backing_token_release(&signal->token);
			switch_gpu_event_release(&signal->event);
			switch_gpu_event_waits_release(&signal->waits);
			delete signal;
		}
		for (auto *batch : batches) {
			completeBatch(batch, batch->result ? batch->result : 1);
			switch_gpu_event_waits_release(&batch);
		}
		EXPECT_EQ(semanticNodes, 0);
		EXPECT_EQ(tokenReferences, 0);
		EXPECT_EQ(source.references, 1u);
		for (auto *watch : watches)
			delete watch;
	}
};
}

extern "C" int switch_gpu_event_acquire(void *, uint32_t port, switch_gpu_event **out)
{
	if (port != 7)
		return -2;
	*out = &source;
	++source.references;
	return 0;
}

extern "C" void switch_gpu_event_retain(switch_gpu_event *event)
{
	++event->references;
}

extern "C" void switch_gpu_event_release(switch_gpu_event **event)
{
	if (*event) {
		--(*event)->references;
		*event = nullptr;
	}
}

extern "C" uint64_t switch_gpu_event_value(switch_gpu_event *event)
{
	return event->value;
}

extern "C" int switch_gpu_event_resolve(switch_gpu_event *event, uint64_t value,
					switch_gpu_backing_token *token)
{
	if (event->value >= value)
		return 1;
	if (event->state == 2) {
		*token = {9, 1, SWITCH_GPU_ENGINE_GM20B, 3};
		++tokenReferences;
	}
	return event->state;
}

extern "C" int switch_gpu_event_terminal(switch_gpu_event *event, uint64_t value)
{
	return event->terminalState ? event->terminalState : event->value >= value ? 1 : 0;
}

extern "C" int switch_gpu_backing_token_retain(const switch_gpu_backing_token *)
{
	++tokenReferences;
	return 0;
}

extern "C" void switch_gpu_backing_token_release(const switch_gpu_backing_token *)
{
	--tokenReferences;
}

extern "C" int switch_gpu_event_signal_prepare(switch_gpu_event *event, uint64_t,
					       switch_gpu_event_signal **out)
{
	if (prepareError)
		return prepareError;
	*out = new switch_gpu_event_signal{event, {}};
	++event->references;
	return 0;
}

extern "C" int switch_gpu_event_signal_bind(switch_gpu_event_signal *signal,
					    const switch_gpu_backing_token *token)
{
	if (signal->token.engine)
		return signal->token.value == token->value ? 0 : -22;
	signal->token = *token;
	++tokenReferences;
	return 0;
}

extern "C" void switch_gpu_event_signal_commit(switch_gpu_event_signal **signal)
{
	committed.push_back(*signal);
	*signal = nullptr;
}

extern "C" void switch_gpu_event_signal_abort(switch_gpu_event_signal **signal)
{
	if (!*signal)
		return;
	if ((*signal)->token.engine)
		--tokenReferences;
	switch_gpu_event_release(&(*signal)->event);
	switch_gpu_event_waits_release(&(*signal)->waits);
	delete *signal;
	*signal = nullptr;
}

extern "C" int switch_gpu_event_observe(switch_gpu_event *event, uint64_t, uint64_t cookie,
					void (*callback)(void *, uint64_t, int), void *context,
					switch_gpu_event_watch **out)
{
	if (observeError)
		return observeError;
	*out = new switch_gpu_event_watch{event, callback, context, cookie};
	watches.push_back(*out);
	++event->references;
	if (onObserve)
		onObserve();
	if (inlineReady)
		deliver(*out, 0);
	return 0;
}

extern "C" int switch_gpu_event_observe_terminal(switch_gpu_event *event, uint64_t value,
						 uint64_t cookie,
						 void (*callback)(void *, uint64_t, int),
						 void *context, switch_gpu_event_watch **out)
{
	const int result = switch_gpu_event_observe(event, value, cookie, callback, context, out);
	if (!result)
		(*out)->terminal = true;
	return result;
}

extern "C" void switch_gpu_event_observe_cancel(switch_gpu_event_watch **watch)
{
	if (!*watch)
		return;
	auto *owned = *watch;
	*watch = nullptr;
	deliver(owned, -125);
	switch_gpu_event_release(&owned->event);
}

extern "C" int switch_gpu_event_resolve_use(switch_gpu_event *event, uint64_t value,
					    switch_gpu_backing_token *token,
					    switch_gpu_event_use **use)
{
	const int result = switch_gpu_event_resolve(event, value, token);
	if (result == 2)
		*use = new switch_gpu_event_use{1, *token};
	return result;
}

extern "C" void switch_gpu_event_use_release(switch_gpu_event_use **use)
{
	if (!*use)
		return;
	if (!--(*use)->refs) {
		--tokenReferences;
		delete *use;
	}
	*use = nullptr;
}

extern "C" int switch_gpu_event_waits_prepare(switch_gpu_event_use *const *uses, uint32_t count,
					      switch_gpu_event_waits **out)
{
	*out = new switch_gpu_event_waits;
	for (unsigned i = 0; i < count; ++i) {
		++uses[i]->refs;
		(*out)->uses.push_back(uses[i]);
	}
	return 0;
}

extern "C" int switch_gpu_event_waits_bind(switch_gpu_event_waits *batch,
					   const switch_gpu_backing_token *token)
{
	if (batch->token.engine)
		return batch->token.value == token->value ? 0 : -22;
	batch->token = *token;
	++tokenReferences;
	return 0;
}

extern "C" void switch_gpu_event_waits_retain(switch_gpu_event_waits *batch)
{
	++batch->refs;
}

extern "C" void switch_gpu_event_waits_release(switch_gpu_event_waits **owned)
{
	if (!*owned)
		return;
	auto *batch = *owned;
	*owned = nullptr;
	if (--batch->refs)
		return;
	if (batch->token.engine)
		--tokenReferences;
	for (auto *use : batch->uses)
		switch_gpu_event_use_release(&use);
	switch_gpu_event_waits_release(&batch->previous);
	delete batch;
}

extern "C" void switch_gpu_event_waits_abort(switch_gpu_event_waits **batch)
{
	if (*batch)
		completeBatch(*batch, -125);
	switch_gpu_event_waits_release(batch);
}

extern "C" void switch_gpu_event_waits_commit(switch_gpu_event_waits **batch)
{
	batches.push_back(*batch);
	*batch = nullptr;
}

extern "C" int switch_gpu_event_waits_inherit(switch_gpu_event_waits *batch,
					      switch_gpu_event_waits *previous)
{
	batch->previous = previous;
	++previous->refs;
	return 0;
}

extern "C" int switch_gpu_event_signal_inherit(switch_gpu_event_signal *signal,
					       switch_gpu_event_waits *batch)
{
	if (signal->waits && signal->waits != batch)
		return -22;
	if (!signal->waits) {
		signal->waits = batch;
		++batch->refs;
	}
	return 0;
}

extern "C" int switch_gpu_event_waits_status(switch_gpu_event_waits *batch)
{
	return batch->result;
}

extern "C" int switch_gpu_event_waits_wait(switch_gpu_event_waits *batch, uint64_t)
{
	return batch->result ? batch->result : -110;
}

extern "C" int switch_gpu_backing_has_semantics()
{
	return semanticSource.engine != 0;
}

extern "C" int switch_gpu_event_waits_covers(switch_gpu_event_waits *batch,
					     const switch_gpu_backing_token *token)
{
	if (!sameToken(*token, semanticSource))
		return 1;
	if (semanticResult < 0)
		return semanticResult;
	if (batch)
		for (const auto &covered : batch->dependencies)
			if (sameToken(covered, *token))
				return 1;
	return 0;
}

extern "C" int switch_gpu_event_waits_prepare_dependencies(const switch_gpu_backing_token *tokens,
							   uint32_t count,
							   switch_gpu_event_waits **out)
{
	*out = nullptr;
	if (!count)
		return 0;
	*out = new switch_gpu_event_waits;
	(*out)->dependencies.assign(tokens, tokens + count);
	return 0;
}

extern "C" int switch_gpu_event_waits_merge_uncommitted(switch_gpu_event_waits *dst,
							switch_gpu_event_waits *src)
{
	dst->dependencies.insert(dst->dependencies.end(), src->dependencies.begin(),
				 src->dependencies.end());
	return 0;
}

extern "C" int switch_gpu_event_waits_completion(switch_gpu_event_waits *batch,
						 void (*callback)(void *, int), void *context)
{
	if (batch->completed)
		return -22;
	batch->completed = callback;
	batch->context = context;
	return 0;
}

namespace
{
struct WordRelease {
	switch_gpu_event_word *word;
	uint32_t channel, payload;
};

std::vector<WordRelease> wordReleases;
int releaseError, wordHolds;
bool waitOnWord;
uint32_t fencePage[1024];

void holdWord(const volatile uint32_t *)
{
	++wordHolds;
}

void putWord(const volatile uint32_t *)
{
	--wordHolds;
}
}  // namespace

switch_gpu_event_word *switch_gpu_event_word_create(volatile uint32_t *cpu, uint64_t pool,
						    void (*put)(const volatile uint32_t *))
{
	auto *word = new switch_gpu_event_word{};
	*word = {1, 0, cpu, pool, put, 0, 0, 0, 0, false};
	return word;
}

void switch_gpu_event_word_retain(switch_gpu_event_word *word)
{
	++word->refs;
}

void switch_gpu_event_word_release(switch_gpu_event_word **word)
{
	if (*word && !--(*word)->refs) {
		(*word)->put((*word)->cpu);
		delete *word;
	}
	*word = nullptr;
}

int switch_gpu_event_word_arm(switch_gpu_event_word *word, switch_gpu_event *, uint32_t channel,
			      uint64_t *base, uint32_t *floor)
{
	if (word->pending && word->channel != channel)
		return -16;
	word->armed = true;
	word->channel = channel;
	*base = word->base = 40;
	*floor = word->last;
	return 0;
}

bool switch_gpu_event_word_idle(const switch_gpu_event_word *word)
{
	return !word->pending;
}

void switch_gpu_event_word_disarm(switch_gpu_event_word *word)
{
	word->armed = false;
}

int switch_gpu_event_signal_release(switch_gpu_event_signal *, switch_gpu_event_word *word,
				    uint32_t channel, uint32_t payload)
{
	if (releaseError || !word->armed)
		return releaseError ? releaseError : -22;
	wordReleases.push_back({word, channel, payload});
	++word->pending;
	return 0;
}

bool switch_gpu_event_use_word(const switch_gpu_event_use *, uint64_t value,
			       switch_gpu_word_wait *wait)
{
	if (!waitOnWord)
		return false;
	*wait = {0x9000, uint32_t(value - 40), 3};
	return true;
}

extern "C" int switch_gpu_timeline_semantic_allocate(switch_gpu_timeline_semantic **out)
{
	if (!*out) {
		*out = new switch_gpu_timeline_semantic;
		++semanticNodes;
	}
	return 0;
}

extern "C" int switch_gpu_timeline_semantic_reserve(const switch_gpu_backing_token *,
						    switch_gpu_event_waits *,
						    switch_gpu_timeline_semantic **out)
{
	if (!*out)
		return -12;
	(*out)->bound = true;
	return 0;
}

extern "C" void switch_gpu_timeline_semantic_finish(switch_gpu_timeline_semantic **out, int)
{
	if (*out) {
		delete *out;
		*out = nullptr;
		--semanticNodes;
	}
}

TEST_F(EventClient, HandlesRejectStaleSlotReuseAndUnknownImports)
{
	const uint64_t old = handle;
	EXPECT_EQ(client.release(handle), 0);
	uint64_t value = 0;
	ASSERT_EQ(client.import(this, 7, &handle, &value), 0);
	EXPECT_NE(old, handle);
	EXPECT_EQ(client.probe(old, 1), -22);
	EXPECT_EQ(client.import(this, 8, &handle, &value), -2);
}

TEST_F(EventClient, ProbeDoesNotArmAndReleasesResolvedProducer)
{
	EXPECT_EQ(client.probe(handle, 1), -19);
	source.state = 2;
	EXPECT_EQ(client.probe(handle, 1), 0);
	EXPECT_EQ(tokenReferences, 0);
	source.state = -5;
	EXPECT_EQ(client.probe(handle, 1), -5);
	source.value = 1;
	EXPECT_EQ(client.probe(handle, 1), 0);
	EXPECT_TRUE(watches.empty());
}

TEST_F(EventClient, TerminalProbeKeepsPublishedButUnfinishedProducerPending)
{
	source.state = 2;
	EXPECT_EQ(client.probe(handle, 1), 0);
	EXPECT_EQ(client.probe(handle, 1, true), -19);
	source.terminalState = -5;
	EXPECT_EQ(client.probe(handle, 1, true), -5);
	source.terminalState = 1;
	EXPECT_EQ(client.probe(handle, 1, true), 0);
	EXPECT_EQ(tokenReferences, 0);
	EXPECT_TRUE(watches.empty());
}

TEST_F(EventClient, TerminalObserverDeliversLaterFailureAndReturnsCapacity)
{
	Notice notice;
	source.state = 2;
	ASSERT_EQ(client.observe(handle, 1, 42, notified, &notice, true), 0);
	EXPECT_TRUE(watches.back()->terminal);
	EXPECT_EQ(notice.count, 0u);
	deliver(watches.back(), -5);
	EXPECT_EQ(notice.count, 1u);
	EXPECT_EQ(notice.result, -5);
	EXPECT_EQ(client.cancel(handle, 42), 0);
	EXPECT_EQ(notice.count, 1u);
}

TEST_F(EventClient, TerminalObserverStopCancelsExactlyOnce)
{
	Notice notice;
	ASSERT_EQ(client.observe(handle, 1, 42, notified, &notice, true), 0);
	client.stop();
	EXPECT_EQ(notice.count, 1u);
	EXPECT_EQ(notice.result, -125);
	deliver(watches.back(), -5);
	EXPECT_EQ(notice.count, 1u);
}

TEST_F(EventClient, InlineReadinessRetiresSlotAndOriginalCallbackExactlyOnce)
{
	inlineReady = true;
	Notice notice;
	EXPECT_EQ(client.observe(handle, 1, 42, notified, &notice), 0);
	EXPECT_EQ(notice.count, 1u);
	EXPECT_EQ(notice.cookie, 42u);
	EXPECT_EQ(notice.result, 0);
	EXPECT_EQ(client.cancel(handle, 42), 0);
	EXPECT_EQ(notice.count, 1u);
	for (unsigned i = 0; i < 300; ++i)
		ASSERT_EQ(client.observe(handle, 1, i, notified, &notice), 0);
	EXPECT_EQ(notice.count, 301u);
}

TEST_F(EventClient, AsyncReadinessThenCancelHasOneCompletion)
{
	Notice notice;
	ASSERT_EQ(client.observe(handle, 1, 7, notified, &notice), 0);
	deliver(watches.back(), 0);
	EXPECT_EQ(notice.count, 1u);
	EXPECT_EQ(client.cancel(handle, 7), 0);
	EXPECT_EQ(notice.count, 1u);
}

TEST_F(EventClient, CancelThenLateReadinessHasOneCompletion)
{
	Notice notice;
	ASSERT_EQ(client.observe(handle, 1, 7, notified, &notice), 0);
	EXPECT_EQ(client.cancel(handle, 7), 0);
	deliver(watches.back(), 0);
	EXPECT_EQ(notice.count, 1u);
	EXPECT_EQ(notice.result, -125);
}

TEST_F(EventClient, CancellationDuringRegistrationWaitsForNativeHandle)
{
	Notice notice;
	onObserve = [&] {
		EXPECT_EQ(client.cancel(handle, 7), 0);
		EXPECT_EQ(notice.count, 0u);
	};
	EXPECT_EQ(client.observe(handle, 1, 7, notified, &notice), 0);
	EXPECT_EQ(notice.count, 1u);
	EXPECT_EQ(notice.result, -125);
}

TEST_F(EventClient, StopDuringRegistrationCancelsAndRejectsLaterWork)
{
	Notice notice;
	onObserve = [&] {
		client.stop();
		EXPECT_EQ(notice.count, 0u);
	};
	EXPECT_EQ(client.observe(handle, 1, 7, notified, &notice), 0);
	EXPECT_EQ(notice.count, 1u);
	EXPECT_EQ(client.probe(handle, 1), -19);
	EXPECT_EQ(client.observe(handle, 1, 8, notified, &notice), -19);
}

TEST_F(EventClient, ArmFailureDoesNotTransferContextOrLeakSlots)
{
	Notice notice;
	observeError = -12;
	for (unsigned i = 0; i < 300; ++i)
		EXPECT_EQ(client.observe(handle, 1, i, notified, &notice), -12);
	EXPECT_EQ(notice.count, 0u);
	observeError = 0;
	EXPECT_EQ(client.observe(handle, 1, 1, notified, &notice), 0);
	client.cancel(handle, 1);
	EXPECT_EQ(notice.count, 1u);
}

TEST_F(EventClient, DuplicateCookieIsRejectedAndReleaseCancelsObservers)
{
	Notice first, second;
	EXPECT_EQ(client.observe(handle, 1, 9, notified, &first), 0);
	EXPECT_EQ(client.observe(handle, 1, 9, notified, &second), -22);
	EXPECT_EQ(client.release(handle), 0);
	EXPECT_EQ(first.count, 1u);
	EXPECT_EQ(second.count, 0u);
}

TEST_F(EventClient, PendingObserversAreBoundedAndCancellationReturnsCapacity)
{
	Notice notices[129];
	for (unsigned i = 0; i < 128; ++i)
		EXPECT_EQ(client.observe(handle, 1, i, notified, &notices[i]), 0);
	EXPECT_EQ(client.observe(handle, 1, 128, notified, &notices[128]), -28);
	client.cancel(handle, 7);
	EXPECT_EQ(client.observe(handle, 1, 128, notified, &notices[128]), 0);
	client.stop();
	for (auto &notice : notices)
		EXPECT_EQ(notice.count, 1u);
}

TEST_F(EventClient, ConcurrentCancelAndReadinessDeliverOnce)
{
	for (unsigned i = 0; i < 100; ++i) {
		Notice notice;
		ASSERT_EQ(client.observe(handle, 1, i, notified, &notice), 0);
		auto *watch = watches.back();
		std::thread ready([&] {
			deliver(watch, 0);
		});
		std::thread cancel([&] {
			client.cancel(handle, i);
		});
		ready.join();
		cancel.join();
		EXPECT_EQ(notice.count, 1u);
	}
}

TEST_F(EventClient, JobUnpublishedWaitCannotPrepareSignals)
{
	struct {
		TegraGM20BAbiSubmitEvents header;
		TegraGM20BAbiEventUse uses[2];
	} input = {{1, 1, 0}, {{handle, 1}, {handle, 2}}};

	SwitchGpuEventJob job;
	EXPECT_EQ(job.prepare(client, &input.header), -19);
	EXPECT_TRUE(committed.empty());
	job.clear();
}

TEST_F(EventClient, JobBindsReservedFenceAndAbortsBeforePublication)
{
	struct {
		TegraGM20BAbiSubmitEvents header;
		TegraGM20BAbiEventUse use;
	} input = {{0, 1, 0}, {handle, 1}};

	SwitchGpuEventJob job;
	ASSERT_EQ(job.prepare(client, &input.header), 0);
	const switch_gpu_backing_token fence = {0, 2, SWITCH_GPU_ENGINE_GM20B, 7};
	EXPECT_EQ(job.bind(&fence), 0);
	EXPECT_EQ(job.bind(&fence), 0);
	EXPECT_EQ(tokenReferences, 1);
	job.clear();
	EXPECT_EQ(tokenReferences, 0);
	EXPECT_TRUE(committed.empty());
}

TEST_F(EventClient, JobSignalCommitTransfersReservedToken)
{
	struct {
		TegraGM20BAbiSubmitEvents header;
		TegraGM20BAbiEventUse use;
	} input = {{0, 1, 0}, {handle, 1}};

	SwitchGpuEventJob job;
	ASSERT_EQ(job.prepare(client, &input.header), 0);
	const switch_gpu_backing_token fence = {77, 3, SWITCH_GPU_ENGINE_GM20B, 8};
	ASSERT_EQ(job.bind(&fence), 0);
	job.commit();
	job.clear();
	ASSERT_EQ(committed.size(), 1u);
	EXPECT_EQ(committed[0]->token.value, 77u);
	EXPECT_EQ(tokenReferences, 1);
}

TEST_F(EventClient, DependencyOverflowRefusesInsteadOfDroppingEvent)
{
	struct {
		TegraGM20BAbiSubmitEvents header;
		TegraGM20BAbiEventUse use;
	} input = {{1, 0, 0}, {handle, 1}};

	source.state = 2;
	SwitchGpuEventJob job;
	ASSERT_EQ(job.prepare(client, &input.header), 0);
	switch_gpu_backing_token dependencies[2] = {};
	uint32_t count = 2;
	EXPECT_EQ(job.append(dependencies, 2, &count), -28);
	EXPECT_EQ(count, 2u);
	count = 1;
	EXPECT_EQ(job.append(dependencies, 2, &count), 0);
	EXPECT_EQ(count, 2u);
	EXPECT_EQ(dependencies[1].value, 9u);
	job.clear();
}

TEST_F(EventClient, AcceptedWaitOwnsProducerUntilConsumerAndCompletionQuery)
{
	SwitchGpuEventChannel channel;
	ASSERT_TRUE(channel.initialize());

	struct {
		TegraGM20BAbiSubmitEvents header;
		TegraGM20BAbiEventUse use;
	} input = {{1, 0, 0}, {handle, 1}};

	source.state = 2;
	SwitchGpuEventJob job;
	ASSERT_EQ(job.prepare(client, &input.header), 0);
	ASSERT_EQ(job.sequence(channel), 0);
	EXPECT_TRUE(job.semanticFence());
	const switch_gpu_backing_token fence = {10, 1, SWITCH_GPU_ENGINE_GM20B, 8};
	ASSERT_EQ(job.bind(&fence), 0);
	EXPECT_EQ(channel.wait(10, 1), -110);
	job.commit();
	job.clear();
	EXPECT_EQ(tokenReferences, 2);
	ASSERT_EQ(batches.size(), 1u);
	completeBatch(batches[0], 1);
	EXPECT_EQ(channel.wait(10, 1), 1);
	channel.destroy();
}

TEST_F(EventClient, SplitWaitPrefixFlowsIntoLaterSignalWithoutAnotherWait)
{
	SwitchGpuEventChannel channel;
	ASSERT_TRUE(channel.initialize());

	struct {
		TegraGM20BAbiSubmitEvents header;
		TegraGM20BAbiEventUse use;
	} input = {{1, 0, 0}, {handle, 1}};

	source.state = 2;
	SwitchGpuEventJob prefix;
	ASSERT_EQ(prefix.prepare(client, &input.header), 0);
	ASSERT_EQ(prefix.sequence(channel), 0);
	const switch_gpu_backing_token first = {10, 1, SWITCH_GPU_ENGINE_GM20B, 8};
	ASSERT_EQ(prefix.bind(&first), 0);
	prefix.commit();
	prefix.clear();
	input.header = {0, 1, 0};
	input.use.value = 2;
	SwitchGpuEventJob last;
	ASSERT_EQ(last.prepare(client, &input.header), 0);
	ASSERT_EQ(last.sequence(channel), 0);
	const switch_gpu_backing_token final = {20, 1, SWITCH_GPU_ENGINE_GM20B, 8};
	ASSERT_EQ(last.bind(&final), 0);
	last.commit();
	last.clear();
	ASSERT_EQ(committed.size(), 1u);
	ASSERT_EQ(batches.size(), 2u);
	EXPECT_EQ(committed[0]->waits, batches[1]);
	EXPECT_EQ(batches[1]->previous, batches[0]);
	completeBatch(batches[0], -5);
	completeBatch(batches[1], -5);
	EXPECT_EQ(channel.wait(20, 1), -5);
	EXPECT_EQ(channel.wait(20, 1), -5);
	EXPECT_EQ(channel.wait(9, 1), 1);
	channel.destroy();
}

TEST_F(EventClient, WaitCompletionConsumesOnlyRequestedPrefixAcrossSequenceWrap)
{
	SwitchGpuEventChannel channel;
	ASSERT_TRUE(channel.initialize());
	for (uint32_t seq : {UINT32_MAX, 0u}) {
		switch_gpu_event_waits *batch = nullptr;
		SwitchGpuEventChannel::Record *record = nullptr;
		ASSERT_EQ(channel.prepare(nullptr, 0, &batch, &record), 0);
		const switch_gpu_backing_token token = {seq, 1, SWITCH_GPU_ENGINE_GM20B, 8};
		ASSERT_EQ(switch_gpu_event_waits_bind(batch, &token), 0);
		channel.bind(record, seq);
		channel.commit(&record);
		switch_gpu_event_waits_commit(&batch);
	}
	completeBatch(batches[0], 1);
	EXPECT_EQ(channel.wait(UINT32_MAX, 1), 1);
	EXPECT_EQ(channel.wait(0, 1), -110);
	completeBatch(batches[1], 1);
	EXPECT_EQ(channel.wait(0, 1), 1);
	const uint64_t generation = channel.generation();
	channel.reset();
	EXPECT_EQ(channel.wait(0, 1, generation), -19);
	channel.destroy();
}

TEST_F(EventClient, PlainBackingDependencyGetsSemanticFenceWithoutExplicitEvent)
{
	SwitchGpuEventChannel channel;
	ASSERT_TRUE(channel.initialize());
	SwitchGpuEventJob job;
	ASSERT_EQ(job.prepare(client, nullptr), 0);
	ASSERT_EQ(job.sequence(channel), 0);
	EXPECT_EQ(semanticNodes, 0);
	EXPECT_FALSE(job.semanticFence());
	semanticSource = {9, 3, SWITCH_GPU_ENGINE_GM20B, 8};
	EXPECT_EQ(job.captureDependencies(&semanticSource, 1), 1);
	EXPECT_EQ(tokenReferences, 1);
	ASSERT_EQ(job.inheritDependencies(), 0);
	EXPECT_EQ(tokenReferences, 0);
	EXPECT_EQ(semanticNodes, 1);
	// its batch is read again only at a published completion: SubmitJob never lets this job end quiet
	EXPECT_TRUE(job.semanticFence());
	EXPECT_EQ(job.captureDependencies(&semanticSource, 1), 0);
	const switch_gpu_backing_token completion = {10, 4, SWITCH_GPU_ENGINE_GM20B, 9};
	ASSERT_EQ(job.bind(&completion), 0);
	job.commit();
	job.clear();
	ASSERT_EQ(batches.size(), 1u);
	completeBatch(batches[0], -5);
	EXPECT_EQ(channel.wait(10, 1), -5);
	channel.destroy();
}

TEST_F(EventClient, RingRetryMergesChangedProducerAfterReservedSemanticFence)
{
	SwitchGpuEventChannel channel;
	ASSERT_TRUE(channel.initialize());
	SwitchGpuEventJob job;
	ASSERT_EQ(job.sequence(channel), 0);
	semanticSource = {9, 3, SWITCH_GPU_ENGINE_GM20B, 8};
	ASSERT_EQ(job.captureDependencies(&semanticSource, 1), 1);
	ASSERT_EQ(job.inheritDependencies(), 0);
	const switch_gpu_backing_token completion = {10, 4, SWITCH_GPU_ENGINE_GM20B, 9};
	ASSERT_EQ(job.bind(&completion), 0);
	semanticSource.value = 10;
	ASSERT_EQ(job.captureDependencies(&semanticSource, 1), 1);
	ASSERT_EQ(job.inheritDependencies(), 0);
	EXPECT_EQ(semanticNodes, 1);
	EXPECT_EQ(job.captureDependencies(&semanticSource, 1), 0);
	ASSERT_EQ(job.bind(&completion), 0);
	job.clear();
	EXPECT_EQ(semanticNodes, 0);
	EXPECT_EQ(tokenReferences, 0);
	EXPECT_EQ(channel.wait(10, 1), 1);
	channel.destroy();
}

TEST_F(EventClient, CompletedSemanticRecordsPruneBeforeBoundedCapacityRunsOut)
{
	SwitchGpuEventChannel channel;
	ASSERT_TRUE(channel.initialize());
	for (uint32_t i = 1; i != 400; ++i) {
		switch_gpu_event_waits *batch = nullptr;
		SwitchGpuEventChannel::Record *record = nullptr;
		ASSERT_EQ(channel.prepare(nullptr, 0, &batch, &record), 0);
		channel.bind(record, i);
		channel.commit(&record);
		completeBatch(batch, 1);
		switch_gpu_event_waits_release(&batch);
	}
	EXPECT_EQ(channel.wait(399, 1), 1);
	channel.destroy();
}

TEST_F(EventClient, KernelDependencyPayloadRetainsExactTokensUntilAdmissionEnds)
{
	SwitchGpuEventJob job;
	const switch_gpu_backing_token token = {9, 7, SWITCH_GPU_ENGINE_GM20B, 3};
	const switch_gpu_event_submission input = {&token, 1, nullptr, nullptr, 0};
	ASSERT_EQ(job.prepareKernel(&input), 0);
	EXPECT_EQ(tokenReferences, 1);
	switch_gpu_backing_token output[1] = {};
	uint32_t count = 0;
	ASSERT_EQ(job.append(output, 1, &count), 0);
	EXPECT_EQ(count, 1u);
	EXPECT_TRUE(sameToken(output[0], token));
	job.clear();
	EXPECT_EQ(tokenReferences, 0);
}

TEST_F(EventClient, RejectedKernelSubmissionLeavesSignalsOwnedByCaller)
{
	switch_gpu_event_signal *signal = nullptr;
	ASSERT_EQ(switch_gpu_event_signal_prepare(&source, 12, &signal), 0);
	auto *owned = signal;
	const switch_gpu_event_submission input = {nullptr, 0, nullptr, &signal, 1};
	SwitchGpuEventChannel channel;
	ASSERT_TRUE(channel.initialize());
	SwitchGpuEventJob job;
	ASSERT_EQ(job.prepareKernel(&input), 0);
	ASSERT_EQ(job.sequence(channel), 0);
	const switch_gpu_backing_token token = {7, 3, SWITCH_GPU_ENGINE_GM20B, 2};
	ASSERT_EQ(job.bind(&token), 0);
	job.clear();
	EXPECT_EQ(signal, owned);
	EXPECT_TRUE(committed.empty());
	switch_gpu_event_signal_abort(&signal);
	channel.destroy();
	EXPECT_EQ(tokenReferences, 0);
	EXPECT_EQ(semanticNodes, 0);
}

TEST_F(EventClient, AcceptedKernelSubmissionConsumesSignalsAndBindsOwnCompletion)
{
	switch_gpu_event_signal *signal = nullptr;
	ASSERT_EQ(switch_gpu_event_signal_prepare(&source, 12, &signal), 0);
	const switch_gpu_event_submission input = {nullptr, 0, nullptr, &signal, 1};
	SwitchGpuEventChannel channel;
	ASSERT_TRUE(channel.initialize());
	SwitchGpuEventJob job;
	ASSERT_EQ(job.prepareKernel(&input), 0);
	ASSERT_EQ(job.sequence(channel), 0);
	const switch_gpu_backing_token token = {7, 3, SWITCH_GPU_ENGINE_GM20B, 2};
	ASSERT_EQ(job.bind(&token), 0);
	job.commit();
	job.clear();
	EXPECT_EQ(signal, nullptr);
	ASSERT_EQ(committed.size(), 1u);
	EXPECT_TRUE(sameToken(committed[0]->token, token));
	for (auto *batch : batches)
		completeBatch(batch, 1);
	channel.destroy();
}

TEST_F(EventClient, EventWordIsTheSlotsWordInTheFencePageAndOutlivesItsHandle)
{
	client.attachWords(fencePage, 0x7000, 0x40000, holdWord, putWord);
	uint64_t address = 0, base = 0;
	uint32_t floor = 0;
	ASSERT_EQ(client.word(handle, 2, &address, &base, &floor), 0);
	const uint32_t slot = uint32_t(handle % 256);
	EXPECT_EQ(address, 0x7000u + (TegraGM20BEventWordFirst + slot) * 4);
	EXPECT_EQ(base, 40u);
	EXPECT_EQ(wordHolds, 1);
	switch_gpu_event_word *word = client.wordOf(handle);
	ASSERT_NE(word, nullptr);
	EXPECT_EQ(word->cpu, &fencePage[TegraGM20BEventWordFirst + slot]);
	EXPECT_EQ(word->pool, 0x40000u + (TegraGM20BEventWordFirst + slot) * 4);
	EXPECT_EQ(client.word(handle + 1, 2, &address, &base, &floor), -22);
	++word->pending;
	ASSERT_EQ(client.release(handle), 0);
	EXPECT_EQ(client.wordOf(handle), nullptr);
	uint64_t again = 0, value = 0;
	ASSERT_EQ(client.import(this, 7, &again, &value), 0);
	EXPECT_NE(again % 256, slot);
	EXPECT_EQ(client.release(again), 0);
	--word->pending;
	ASSERT_EQ(client.import(this, 7, &again, &value), 0);
	EXPECT_EQ(again % 256, slot);
	EXPECT_FALSE(word->armed);
	switch_gpu_event_word_release(&word);
	client.destroy();
	EXPECT_EQ(wordHolds, 0);
}

TEST_F(EventClient, JobReleasesItsSignalIntoTheWordAndWaitsOnAProducersWord)
{
	client.attachWords(fencePage, 0x7000, 0x40000, holdWord, putWord);
	uint64_t address = 0, base = 0;
	uint32_t floor = 0;
	ASSERT_EQ(client.word(handle, 2, &address, &base, &floor), 0);

	struct {
		TegraGM20BAbiSubmitEvents header;
		TegraGM20BAbiEventUse uses[2];
	} input = {{1, 1, 0}, {{handle, 45}, {handle, 47}}};

	const TegraGM20BAbiSignalRelease release = {7, TegraGM20BSignalReleased};

	wordReleases.clear();
	source.state = 2;
	waitOnWord = true;
	SwitchGpuEventJob job;
	ASSERT_EQ(job.prepare(client, &input.header, &release, 2), 0);
	EXPECT_TRUE(job.released());
	ASSERT_EQ(wordReleases.size(), 1u);
	EXPECT_EQ(wordReleases[0].channel, 2u);
	EXPECT_EQ(wordReleases[0].payload, 7u);
	ASSERT_EQ(job.wordCount(), 1u);
	EXPECT_EQ(job.words()[0].pool, 0x9000u);
	EXPECT_EQ(job.words()[0].payload, 5u);
	switch_gpu_backing_token dependencies[4] = {};
	uint32_t count = 0;
	ASSERT_EQ(job.append(dependencies, 4, &count), 0);
	EXPECT_EQ(count, 0u);
	job.clear();
	EXPECT_EQ(job.wordCount(), 0u);
	EXPECT_FALSE(job.released());

	const TegraGM20BAbiSignalRelease unknown = {7, 1u << 1};
	EXPECT_EQ(job.prepare(client, &input.header, &unknown, 2), -22);
	job.clear();
	waitOnWord = false;
	releaseError = -22;
	EXPECT_EQ(job.prepare(client, &input.header, &release, 2), -22);
	job.clear();
	releaseError = 0;
	switch_gpu_event_word *word = client.wordOf(handle);
	word->pending = 0;
	switch_gpu_event_word_release(&word);
}

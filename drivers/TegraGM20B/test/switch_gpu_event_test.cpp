// SPDX-License-Identifier: MIT
#include "switch_gpu_event_fixture.h"

TEST_F(Events, CanonicalPortAliasesShareAcceptedProducer)
{
	uint32_t alias = nextPort++;
	{
		std::lock_guard held(nativeLock);
		ports[alias] = native;
	}
	switch_gpu_event *other = nullptr;
	ASSERT_EQ(switch_gpu_event_acquire(this, alias, &other), 0);
	EXPECT_EQ(other, event);
	signal(9, token());
	switch_gpu_backing_token t{};
	EXPECT_EQ(switch_gpu_event_resolve(other, 9, &t), 2);
	switch_gpu_backing_token_release(&t);
	switch_gpu_event_release(&other);
	{
		std::lock_guard held(nativeLock);
		ports.erase(alias);
	}
}

TEST_F(Events, PrepareInvisibleAndAbortBalancesReservedToken)
{
	auto t = token();
	auto *s = prepare(7, t);
	switch_gpu_backing_token out{};
	EXPECT_EQ(switch_gpu_event_resolve(event, 7, &out), 0);
	EXPECT_EQ(switch_gpu_event_signal_bind(s, &t), 0);
	auto other = token();
	EXPECT_EQ(switch_gpu_event_signal_bind(s, &other), -22);
	switch_gpu_event_signal_abort(&s);
	pump();
	EXPECT_EQ(tokens[key(t)].refs, 0);
}

TEST_F(Events, Full64ValueAndWrappedZeroFenceStayIntact)
{
	auto t = token(0);
	signal((1ULL << 40) + 9, t);
	auto *u = resolve((1ULL << 40) + 8);
	finish(t);
	EXPECT_EQ(native->value, (1ULL << 40) + 9);
	switch_gpu_event_use_release(&u);
	pump();
}

TEST_F(Events, ExecutionCompletionDoesNotPublishUntilExternallyVisible)
{
	auto t = token();
	signal(55, t);
	tokens[key(t)].execution = 1;
	switch_gpu_events_completed();
	pump();
	EXPECT_EQ(native->value, 0u);
	finish(t);
	EXPECT_EQ(native->value, 55u);
}

TEST_F(Events, FailedProducerNeverSignalsNativeValue)
{
	auto t = token();
	signal(55, t);
	finish(t, -5);
	switch_gpu_backing_token out{};
	EXPECT_EQ(native->value, 0u);
	EXPECT_EQ(switch_gpu_event_resolve(event, 55, &out), -5);
	switch_gpu_event_native_signal(native, 56, true);
	EXPECT_EQ(switch_gpu_event_resolve(event, 55, &out), 1);
}

TEST_F(Events, PublishedPendingProducerWakesOnlyDependentObserver)
{
	auto &ready = makeNotices();
	auto &other = makeNotices();
	observe(9, ready);
	observe(11, other);
	pump();
	EXPECT_EQ(ready.count, 0);
	signal(10, token());
	pump();
	EXPECT_EQ(ready.count, 1);
	EXPECT_EQ(ready.result, 0);
	EXPECT_EQ(other.count, 0);
}

TEST_F(Events, NativeCpuSignalUsesFull64Notification)
{
	auto &n = makeNotices();
	observe((1ULL << 40) + 1, n);
	pump();
	switch_gpu_event_native_signal(native, (1ULL << 40) + 1, true);
	nativePump();
	EXPECT_EQ(n.count, 1);
	EXPECT_EQ(n.result, 0);
	EXPECT_EQ(n.cookie, 0x123456789ULL);
}

TEST_F(Events, CancelPendingReleasesNativeListAndCallbacksOnce)
{
	auto &n = makeNotices();
	auto *w = observe(9, n);
	cancel(w);
	EXPECT_EQ(n.count, 1);
	EXPECT_EQ(n.result, -125);
	EXPECT_TRUE(pending.empty());
	switch_gpu_event_native_signal(native, 10, true);
	nativePump();
	EXPECT_EQ(n.count, 1);
}

TEST_F(Events, CancelAfterNativeDequeueKeepsCallbackOwnerAlive)
{
	auto &n = makeNotices();
	auto *w = observe(9, n);
	switch_gpu_event_native_signal(native, 9, true);
	ASSERT_EQ(queued.size(), 1u);
	cancel(w);
	EXPECT_EQ(n.count, 1);
	EXPECT_EQ(n.result, -125);
	nativePump();
	EXPECT_EQ(n.count, 1);
}

TEST_F(Events, ObserveFailureNeverTransfersCallback)
{
	auto &n = makeNotices();
	switch_gpu_event_watch *w = nullptr;
	failWaiter = true;
	EXPECT_EQ(switch_gpu_event_observe(event, 9, 0, notice, &n, &w), -12);
	failWaiter = false;
	pump();
	EXPECT_EQ(n.count, 0);
	EXPECT_EQ(w, nullptr);
}

TEST_F(Events, ConsumerPinsProducerAfterProducerCompletes)
{
	auto a = token(), b = token();
	signal(1, a);
	auto *u = resolve(1);
	auto *w = batch(u, b);
	switch_gpu_event_use_release(&u);
	commit(w);
	finish(a);
	EXPECT_GT(tokens[key(a)].refs, 0);
	EXPECT_EQ(switch_gpu_event_waits_status(w), 0);
	finish(b);
	EXPECT_EQ(switch_gpu_event_waits_status(w), 1);
	switch_gpu_event_waits_release(&w);
	pump();
	EXPECT_EQ(tokens[key(a)].refs, 0);
}

TEST_F(Events, FailedAuthorityPropagatesAcrossAlreadyAcceptedThreeStageChain)
{
	auto a = token(), b = token(), c = token();
	signal(1, a);
	auto *ua = resolve(1);
	auto *wb = batch(ua, b);
	switch_gpu_event_use_release(&ua);
	auto *sb = prepare(2, b);
	ASSERT_EQ(switch_gpu_event_signal_inherit(sb, wb), 0);
	commit(wb);
	switch_gpu_event_signal_commit(&sb);
	auto *ub = resolve(2);
	auto *wc = batch(ub, c);
	switch_gpu_event_use_release(&ub);
	auto *sc = prepare(3, c);
	ASSERT_EQ(switch_gpu_event_signal_inherit(sc, wc), 0);
	commit(wc);
	switch_gpu_event_signal_commit(&sc);
	finish(c);
	finish(b);
	EXPECT_EQ(native->value, 0u);
	finish(a, -5);
	EXPECT_EQ(switch_gpu_event_waits_status(wb), -5);
	EXPECT_EQ(switch_gpu_event_waits_status(wc), -5);
	EXPECT_EQ(native->value, 0u);
	switch_gpu_event_waits_release(&wb);
	switch_gpu_event_waits_release(&wc);
	pump();
}

TEST_F(Events, SplitPrefixFailureFlowsIntoLaterSignalWithoutRepeatedWaits)
{
	auto a = token(), b = token(), c = token();
	signal(1, a);
	auto *u = resolve(1);
	auto *first = batch(u, b);
	switch_gpu_event_use_release(&u);
	commit(first);
	auto *last = batch(nullptr, c);
	ASSERT_EQ(switch_gpu_event_waits_inherit(last, first), 0);
	auto *s = prepare(2, c);
	ASSERT_EQ(switch_gpu_event_signal_inherit(s, last), 0);
	commit(last);
	switch_gpu_event_signal_commit(&s);
	finish(b);
	finish(c);
	finish(a, -5);
	EXPECT_EQ(switch_gpu_event_waits_status(last), -5);
	EXPECT_EQ(native->value, 0u);
	switch_gpu_event_waits_release(&first);
	switch_gpu_event_waits_release(&last);
	pump();
}

TEST_F(Events, SemanticWaitSleepsUntilCompletionAndHonorsDeadline)
{
	auto t = token();
	auto *w = batch(nullptr, t);
	commit(w);
	EXPECT_EQ(switch_gpu_event_waits_wait(w, mach_absolute_time() + 1000000), -110);
	auto waiting = std::async(std::launch::async, [w] {
		return switch_gpu_event_waits_wait(w, 0);
	});
	EXPECT_EQ(waiting.wait_for(std::chrono::milliseconds(10)), std::future_status::timeout);
	finish(t);
	EXPECT_EQ(waiting.get(), 1);
	switch_gpu_event_waits_release(&w);
	pump();
}

TEST_F(Events, ConcurrentCancellationAndReadinessHaveOneWinner)
{
	for (unsigned i = 0; i < 100; ++i) {
		auto &n = makeNotices();
		auto *w = observe(i + 1, n);
		std::thread canceler([&] {
			cancel(w);
		});
		switch_gpu_event_native_signal(native, i + 1, true);
		nativePump();
		canceler.join();
		EXPECT_EQ(n.count, 1);
		EXPECT_TRUE(n.result == 0 || n.result == -125);
	}
}

TEST_F(Events, GlobalBoundIncludesRetainedUsesAndRecoversAfterRelease)
{
	signal(1, token());
	std::vector<switch_gpu_event_use *> uses;
	int result = 0;
	for (unsigned i = 0; i < 5000; ++i) {
		switch_gpu_event_use *u = nullptr;
		switch_gpu_backing_token t{};
		result = switch_gpu_event_resolve_use(event, 1, &t, &u);
		if (result != 2)
			break;
		uses.push_back(u);
	}
	EXPECT_EQ(result, -16);
	EXPECT_LT(uses.size(), 4096u);
	for (auto *&u : uses)
		switch_gpu_event_use_release(&u);
	pump();
	auto *u = resolve(1);
	switch_gpu_event_use_release(&u);
	pump();
}

TEST_F(Events, LongPrefixChainReclaimsIteratively)
{
	switch_gpu_event_waits *previous = nullptr;
	std::vector<switch_gpu_backing_token> completions;
	for (unsigned i = 0; i < 512; ++i) {
		auto t = token();
		completions.push_back(t);
		auto *w = batch(nullptr, t);
		if (previous) {
			ASSERT_EQ(switch_gpu_event_waits_inherit(w, previous), 0);
			switch_gpu_event_waits_release(&previous);
		}
		commit(w);
		previous = w;
	}
	for (auto &t : completions) {
		tokens[key(t)].execution = tokens[key(t)].external = 1;
	}
	switch_gpu_events_completed();
	pump();
	EXPECT_EQ(switch_gpu_event_waits_status(previous), 1);
	switch_gpu_event_waits_release(&previous);
	pump();
}

TEST_F(Events, PrepublicationAbortWakesConcurrentSemanticWaiter)
{
	auto t = token();
	auto *w = batch(nullptr, t);
	switch_gpu_event_waits_retain(w);
	auto *record = w;
	auto waiting = std::async(std::launch::async, [record] {
		return switch_gpu_event_waits_wait(record, 0);
	});
	EXPECT_EQ(waiting.wait_for(std::chrono::milliseconds(10)), std::future_status::timeout);
	switch_gpu_event_waits_abort(&w);
	EXPECT_EQ(waiting.get(), -125);
	switch_gpu_event_waits_release(&record);
	pump();
}

TEST_F(Events, ReleasingPreparedRecordDoesNotCancelOwnersReservation)
{
	auto t = token();
	auto *w = batch(nullptr, t);
	switch_gpu_event_waits_retain(w);
	auto *record = w;
	switch_gpu_event_waits_release(&record);
	EXPECT_EQ(switch_gpu_event_waits_status(w), 0);
	commit(w);
	finish(t);
	EXPECT_EQ(switch_gpu_event_waits_status(w), 1);
	switch_gpu_event_waits_release(&w);
	pump();
}

TEST_F(Events, PlainBackingConsumerInheritsFailedEventAuthority)
{
	auto a = token(), b = token(), c = token();
	signal(1, a);
	auto *use = resolve(1);
	auto *prefix = batch(use, b);
	switch_gpu_event_use_release(&use);
	commit(prefix);
	semantics[key(b)] = prefix;
	switch_gpu_event_waits *consumer = nullptr;
	switch_gpu_backing_token sources[] = {b, b};
	ASSERT_EQ(switch_gpu_event_waits_prepare_dependencies(sources, 2, &consumer), 0);
	ASSERT_NE(consumer, nullptr);
	ASSERT_EQ(switch_gpu_event_waits_bind(consumer, &c), 0);
	auto *signalC = prepare(2, c);
	ASSERT_EQ(switch_gpu_event_signal_inherit(signalC, consumer), 0);
	commit(consumer);
	switch_gpu_event_signal_commit(&signalC);
	finish(c);
	finish(b);
	finish(a, -5);
	EXPECT_EQ(native->value, 0u);
	EXPECT_EQ(switch_gpu_event_waits_status(consumer), -5);
	semantics.clear();
	switch_gpu_event_waits_release(&prefix);
	switch_gpu_event_waits_release(&consumer);
	pump();
}

TEST_F(Events, OrdinaryDependenciesWithoutSemanticHistoryAllocateNoBatch)
{
	auto t = token();
	switch_gpu_event_waits *consumer = nullptr;
	ASSERT_EQ(switch_gpu_event_waits_prepare_dependencies(&t, 1, &consumer), 0);
	EXPECT_EQ(consumer, nullptr);
}

TEST_F(Events, AbortCompletionRunsOnceOutsideRegistryLock)
{
	auto t = token();
	auto *work = batch(nullptr, t);
	unsigned callbacks = 0;
	ASSERT_EQ(switch_gpu_event_waits_completion(
			  work,
			  [](void *context, int status) {
				  EXPECT_EQ(status, -125);
				  ++*static_cast<unsigned *>(context);
			  },
			  &callbacks),
		  0);
	switch_gpu_event_waits_retain(work);
	auto *held = work;
	switch_gpu_event_waits_abort(&work);
	EXPECT_EQ(callbacks, 1u);
	switch_gpu_event_waits_abort(&held);
	EXPECT_EQ(callbacks, 1u);
	pump();
}

TEST_F(Events, InternalDependencySetExceedsUserEventCountWithoutStackStorage)
{
	std::vector<switch_gpu_event_waits *> sources;
	std::vector<switch_gpu_backing_token> sourceTokens;
	for (unsigned i = 0; i < 40; ++i) {
		auto t = token();
		auto *source = batch(nullptr, t);
		commit(source);
		semantics[key(t)] = source;
		sources.push_back(source);
		sourceTokens.push_back(t);
	}
	switch_gpu_event_waits *consumer = nullptr;
	ASSERT_EQ(switch_gpu_event_waits_prepare_dependencies(
			  sourceTokens.data(), uint32_t(sourceTokens.size()), &consumer),
		  0);
	ASSERT_NE(consumer, nullptr);
	auto done = token();
	ASSERT_EQ(switch_gpu_event_waits_bind(consumer, &done), 0);
	commit(consumer);
	finish(done);
	EXPECT_EQ(switch_gpu_event_waits_status(consumer), 0);
	for (const auto &t : sourceTokens)
		finish(t);
	EXPECT_EQ(switch_gpu_event_waits_status(consumer), 1);
	semantics.clear();
	for (auto *source : sources)
		switch_gpu_event_waits_release(&source);
	switch_gpu_event_waits_release(&consumer);
	pump();
}

TEST_F(Events, RetryMergesIntoBoundBatchWithoutWaitingOnUncommittedSource)
{
	auto a = token(), b = token(), c = token();
	signal(1, a);
	auto *use = resolve(1);
	auto *prefix = batch(use, b);
	switch_gpu_event_use_release(&use);
	commit(prefix);
	semantics[key(b)] = prefix;
	switch_gpu_event_waits *captured = nullptr;
	ASSERT_EQ(switch_gpu_event_waits_prepare_dependencies(&b, 1, &captured), 0);
	auto *consumer = batch(nullptr, c);
	auto *out = prepare(2, c);
	ASSERT_EQ(switch_gpu_event_signal_inherit(out, consumer), 0);
	ASSERT_EQ(switch_gpu_event_waits_merge_uncommitted(consumer, captured), 0);
	ASSERT_EQ(switch_gpu_event_waits_merge_uncommitted(consumer, captured), 0);
	switch_gpu_event_waits_abort(&captured);
	commit(consumer);
	switch_gpu_event_signal_commit(&out);
	finish(c);
	finish(b);
	finish(a, -5);
	EXPECT_EQ(switch_gpu_event_waits_status(consumer), -5);
	EXPECT_EQ(native->value, 0u);
	semantics.clear();
	switch_gpu_event_waits_release(&prefix);
	switch_gpu_event_waits_release(&consumer);
	pump();
}

TEST_F(Events, MergeRejectsPublishedOrPrefixCarryingSource)
{
	auto a = token(), b = token();
	auto *source = batch(nullptr, a);
	auto *destination = batch(nullptr, b);
	ASSERT_EQ(switch_gpu_event_waits_inherit(source, destination), 0);
	EXPECT_EQ(switch_gpu_event_waits_merge_uncommitted(destination, source), -22);
	commit(source);
	EXPECT_EQ(switch_gpu_event_waits_merge_uncommitted(destination, source), -22);
	switch_gpu_event_waits_abort(&destination);
	finish(a);
	switch_gpu_event_waits_release(&source);
	pump();
}

TEST_F(Events, CoversTracksAuthorityReplacementRatherThanPhysicalPrefix)
{
	auto a = token(), b = token(), c = token();
	auto *first = batch(nullptr, a);
	auto *second = batch(nullptr, b);
	commit(first);
	commit(second);
	semantics[key(a)] = first;
	switch_gpu_event_waits *captured = nullptr;
	EXPECT_EQ(switch_gpu_event_waits_covers(nullptr, &a), 0);
	ASSERT_EQ(switch_gpu_event_waits_prepare_dependencies(&a, 1, &captured), 0);
	ASSERT_NE(captured, nullptr);
	EXPECT_EQ(switch_gpu_event_waits_covers(captured, &a), 1);
	EXPECT_EQ(switch_gpu_event_waits_covers(captured, &c), 1);
	semantics[key(a)] = second;
	EXPECT_EQ(switch_gpu_event_waits_covers(captured, &a), 0);
	finish(b, -5);
	EXPECT_EQ(switch_gpu_event_waits_covers(captured, &a), -5);
	semantics.clear();
	switch_gpu_event_waits_abort(&captured);
	finish(a);
	switch_gpu_event_waits_release(&first);
	switch_gpu_event_waits_release(&second);
	pump();
}

TEST_F(Events, CachedTerminalStatusPrecedesDeferredEngineNodeCallback)
{
	struct Callback {
		std::promise<void> entered, leave;
		std::shared_future<void> released = leave.get_future().share();
	} callback;

	auto t = token();
	auto *work = batch(nullptr, t);
	ASSERT_EQ(switch_gpu_event_waits_completion(
			  work,
			  [](void *context, int status) {
				  auto &state = *static_cast<Callback *>(context);
				  EXPECT_EQ(status, 0);
				  state.entered.set_value();
				  state.released.wait();
			  },
			  &callback),
		  0);
	commit(work);
	auto worker = std::async(std::launch::async, [t] {
		finish(t);
	});
	callback.entered.get_future().wait();
	EXPECT_EQ(switch_gpu_event_waits_cached_status(work), 1);
	callback.leave.set_value();
	worker.get();
	switch_gpu_event_waits_release(&work);
	pump();
}

TEST_F(Events, PlainProducerFailureSurvivesHardwareConsumerCompletion)
{
	auto source = token(), consumer = token();
	switch_gpu_event_waits *work = nullptr;
	ASSERT_EQ(switch_gpu_event_waits_prepare_tokens(&source, 1, &work), 0);
	ASSERT_NE(work, nullptr);
	ASSERT_EQ(switch_gpu_event_waits_bind(work, &consumer), 0);
	commit(work);
	finish(consumer);
	EXPECT_EQ(switch_gpu_event_waits_status(work), 0);
	finish(source, -5);
	EXPECT_EQ(switch_gpu_event_waits_status(work), -5);
	switch_gpu_event_waits_release(&work);
	pump();
}

TEST_F(Events, MergedPlainTokensKeepEachIndependentProducer)
{
	auto first = token(), second = token(), consumer = token();
	switch_gpu_event_waits *a = nullptr, *b = nullptr;
	ASSERT_EQ(switch_gpu_event_waits_prepare_tokens(&first, 1, &a), 0);
	ASSERT_EQ(switch_gpu_event_waits_prepare_tokens(&second, 1, &b), 0);
	ASSERT_EQ(switch_gpu_event_waits_merge_uncommitted(a, b), 0);
	EXPECT_EQ(switch_gpu_event_waits_covers_token(a, &first), 1);
	EXPECT_EQ(switch_gpu_event_waits_covers_token(a, &second), 1);
	switch_gpu_event_waits_abort(&b);
	ASSERT_EQ(switch_gpu_event_waits_bind(a, &consumer), 0);
	commit(a);
	finish(consumer);
	finish(first);
	EXPECT_EQ(switch_gpu_event_waits_status(a), 0);
	finish(second, -125);
	EXPECT_EQ(switch_gpu_event_waits_status(a), -125);
	switch_gpu_event_waits_release(&a);
	pump();
}

TEST_F(Events, PlainTokenCoverageRejectsReplacementAndFailedProducer)
{
	auto source = token(), replacement = source;
	++replacement.value;
	tokens[key(replacement)] = {};
	switch_gpu_event_waits *work = nullptr;
	ASSERT_EQ(switch_gpu_event_waits_prepare_tokens(&source, 1, &work), 0);
	EXPECT_EQ(switch_gpu_event_waits_covers_token(work, &source), 1);
	EXPECT_EQ(switch_gpu_event_waits_covers_token(work, &replacement), 0);
	finish(source, -5);
	EXPECT_EQ(switch_gpu_event_waits_covers_token(work, &source), -5);
	switch_gpu_event_waits_abort(&work);
	pump();
}

TEST_F(Events, AlreadySuccessfulPlainProducerNeedsNoAuthorityAllocation)
{
	auto source = token();
	finish(source);
	switch_gpu_event_waits *work = nullptr;
	ASSERT_EQ(switch_gpu_event_waits_prepare_tokens(&source, 1, &work), 0);
	EXPECT_EQ(work, nullptr);
	EXPECT_EQ(switch_gpu_event_waits_covers_token(nullptr, &source), 1);
}

TEST_F(Events, TerminalObserverDoesNotTreatPendingPublishedProducerAsCompleted)
{
	auto producer = token();
	signal(55, producer);
	auto &n = makeNotices();
	observe(55, n, true);
	pump();
	EXPECT_EQ(switch_gpu_event_terminal(event, 55), 0);
	EXPECT_EQ(n.count, 0);
	tokens[key(producer)].execution = 1;
	switch_gpu_events_completed();
	pump();
	EXPECT_EQ(n.count, 0);
	finish(producer);
	EXPECT_EQ(n.count, 1);
	EXPECT_EQ(n.result, 0);
	EXPECT_EQ(switch_gpu_event_terminal(event, 55), 1);
}

TEST_F(Events, TerminalObserverRetainsFailureDespiteHigherNativeSignal)
{
	auto producer = token();
	signal(55, producer);
	auto &n = makeNotices();
	observe(55, n, true);
	switch_gpu_event_native_signal(native, 100, true);
	nativePump();
	EXPECT_EQ(n.count, 0);
	finish(producer, -5);
	EXPECT_EQ(n.count, 1);
	EXPECT_EQ(n.result, -5);
}

TEST_F(Events, FutureTerminalObserverPinsAuthorityAtCommitBeforeWorkerRuns)
{
	auto &n = makeNotices();
	observe(55, n, true);
	auto producer = token();
	signal(55, producer);
	switch_gpu_event_native_signal(native, 100, true);
	finish(producer, -5);
	nativePump();
	EXPECT_EQ(n.count, 1);
	EXPECT_EQ(n.result, -5);
}

TEST_F(Events, TerminalObserverJoinsLogicalDependencyFailureAfterPhysicalSuccess)
{
	auto source = token(), consumer = token();
	signal(1, source);
	auto *use = resolve(1);
	auto *work = batch(use, consumer);
	switch_gpu_event_use_release(&use);
	auto *output = prepare(2, consumer);
	ASSERT_EQ(switch_gpu_event_signal_inherit(output, work), 0);
	switch_gpu_event_signal_commit(&output);
	commit(work);
	auto &n = makeNotices();
	observe(2, n, true);
	finish(consumer);
	EXPECT_EQ(n.count, 0);
	finish(source, -5);
	EXPECT_EQ(n.count, 1);
	EXPECT_EQ(n.result, -5);
	EXPECT_EQ(switch_gpu_event_terminal(event, 2), -5);
	switch_gpu_event_waits_release(&work);
	pump();
}

TEST_F(Events, TerminalNativeCpuSignalAndQueuedCancellationRemainOneShot)
{
	constexpr uint64_t value = (1ULL << 48) + 17;
	auto &ready = makeNotices();
	observe(value, ready, true);
	auto &cancelled = makeNotices();
	auto *watch = observe(value, cancelled, true);
	switch_gpu_event_native_signal(native, value, true);
	cancel(watch);
	nativePump();
	EXPECT_EQ(ready.count, 1);
	EXPECT_EQ(ready.result, 0);
	EXPECT_EQ(cancelled.count, 1);
	EXPECT_EQ(cancelled.result, -125);
	EXPECT_EQ(switch_gpu_event_terminal(event, value), 1);
}

TEST_F(Events, TerminalObserverKeepsAlreadyRetiredFailureChosenAtArm)
{
	auto producer = token();
	signal(55, producer);
	finish(producer, -5);
	auto &n = makeNotices();
	observe(55, n, true);
	switch_gpu_event_native_signal(native, 100, true);
	nativePump();
	EXPECT_EQ(n.count, 1);
	EXPECT_EQ(n.result, -5);
}

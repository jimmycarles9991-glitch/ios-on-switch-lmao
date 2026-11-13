// SPDX-License-Identifier: MIT
#pragma once
#include <gtest/gtest.h>
#include <algorithm>
#include <future>
#include <map>
#include <memory>
#include <thread>
#include <tuple>
#include <vector>
#include "switch_gpu_event_stubs.h"
#include "drivers/TegraGM20B/SwitchGpuSharedEvent.h"
#include "drivers/TegraGM20B/SwitchGpuSharedEventNative.h"

thread_call_t switch_event_call;

namespace
{
struct NativeEvent final : OSObject {
	std::atomic<uint64_t> value{0};
};

struct NativeWaiter final : OSObject {
	NativeEvent *event;
	OSObject *owner, *listener;
	uint64_t value;
	void *context;
	SwitchGpuEventNativeCallback callback;

	~NativeWaiter() override
	{
		owner->release();
		event->release();
		listener->release();
	}
};

std::mutex nativeLock, tokensLock;
std::map<uint32_t, NativeEvent *> ports;
std::vector<NativeWaiter *> pending, queued;
using Key = std::tuple<uint32_t, uint32_t, uint64_t, uint64_t>;

struct Token {
	int execution = 0, external = 0, refs = 0;
};

std::map<Key, Token> tokens;
uint32_t nextQueue = 1, nextPort = 1;
bool failWaiter;
uint32_t statusEngine = 0;
int (*statusOverride)(const switch_gpu_backing_token *) = nullptr;
int (*physicalOverride)(const switch_gpu_backing_token *) = nullptr;
std::map<Key, switch_gpu_event_waits *> semantics;

Key key(const switch_gpu_backing_token &t)
{
	return {t.engine, t.queue, t.reset_generation, t.value};
}

void pump()
{
	for (unsigned i = 0; switch_event_call && switch_event_call->pending.exchange(false); ++i) {
		ASSERT_LT(i, 10000u);
		switch_event_call->fn(switch_event_call->arg, nullptr);
	}
}

void nativePump()
{
	std::vector<NativeWaiter *> ready;
	{
		std::lock_guard held(nativeLock);
		ready.swap(queued);
	}
	for (auto *waiter : ready) {
		waiter->callback(waiter->owner, waiter->event, waiter->value, waiter->context);
		waiter->release();
	}
	pump();
}

void finish(const switch_gpu_backing_token &t, int result = 1)
{
	{
		std::lock_guard held(tokensLock);
		tokens[key(t)].execution = tokens[key(t)].external = result;
	}
	switch_gpu_events_completed();
	pump();
}

struct Notices {
	std::atomic<int> count{0}, result{99};
	uint64_t cookie = 0;
};

void notice(void *ctx, uint64_t cookie, int result)
{
	auto &n = *static_cast<Notices *>(ctx);
	n.cookie = cookie;
	n.result = result;
	++n.count;
}

class Events : public testing::Test
{
protected:
	NativeEvent *native = nullptr;
	switch_gpu_event *event = nullptr;
	uint32_t port;
	std::vector<switch_gpu_event_watch *> watches;
	std::vector<std::unique_ptr<Notices>> notices;

	Notices &makeNotices()
	{
		notices.push_back(std::make_unique<Notices>());
		return *notices.back();
	}

	void SetUp() override
	{
		statusEngine = 0;
		statusOverride = physicalOverride = nullptr;
		native = new NativeEvent;
		port = nextPort++;
		{
			std::lock_guard held(nativeLock);
			ports[port] = native;
		}
		ASSERT_EQ(switch_gpu_event_acquire(this, port, &event), 0);
	}

	void TearDown() override
	{
		for (auto *watch : watches)
			switch_gpu_event_observe_cancel(&watch);
		{
			std::lock_guard held(tokensLock);
			for (auto &[k, t] : tokens)
				if (!t.execution || !t.external)
					t.execution = t.external = 1;
		}
		switch_gpu_events_completed();
		pump();
		nativePump();
		switch_gpu_event_release(&event);
		pump();
		EXPECT_EQ(native->refs.load(), 1u);
		{
			std::lock_guard held(nativeLock);
			ports.erase(port);
		}
		native->release();
		for (auto &[k, t] : tokens)
			EXPECT_EQ(t.refs, 0);
		semantics.clear();
		EXPECT_TRUE(pending.empty());
		EXPECT_TRUE(queued.empty());
	}

	switch_gpu_backing_token token(uint64_t value = 1)
	{
		switch_gpu_backing_token t{value, 7, SWITCH_GPU_ENGINE_GM20B, nextQueue++};
		std::lock_guard held(tokensLock);
		tokens[key(t)] = {};
		return t;
	}

	switch_gpu_event_signal *prepare(uint64_t value, const switch_gpu_backing_token &t)
	{
		switch_gpu_event_signal *s = nullptr;
		EXPECT_EQ(switch_gpu_event_signal_prepare(event, value, &s), 0);
		EXPECT_EQ(switch_gpu_event_signal_bind(s, &t), 0);
		return s;
	}

	void signal(uint64_t value, const switch_gpu_backing_token &t)
	{
		auto *s = prepare(value, t);
		switch_gpu_event_signal_commit(&s);
	}

	switch_gpu_event_watch *observe(uint64_t value, Notices &n, bool terminal = false)
	{
		switch_gpu_event_watch *w = nullptr;
		EXPECT_EQ((terminal ? switch_gpu_event_observe_terminal : switch_gpu_event_observe)(
				  event, value, 0x123456789ULL, notice, &n, &w),
			  0);
		if (w)
			watches.push_back(w);
		return w;
	}

	void cancel(switch_gpu_event_watch *w)
	{
		auto it = std::find(watches.begin(), watches.end(), w);
		if (it != watches.end())
			*it = nullptr;
		switch_gpu_event_observe_cancel(&w);
	}

	switch_gpu_event_use *resolve(uint64_t value)
	{
		switch_gpu_event_use *use = nullptr;
		switch_gpu_backing_token t{};
		EXPECT_EQ(switch_gpu_event_resolve_use(event, value, &t, &use), 2);
		return use;
	}

	switch_gpu_event_waits *batch(switch_gpu_event_use *use, const switch_gpu_backing_token &t)
	{
		switch_gpu_event_waits *b = nullptr;
		EXPECT_EQ(switch_gpu_event_waits_prepare(use ? &use : nullptr, use ? 1 : 0, &b), 0);
		EXPECT_EQ(switch_gpu_event_waits_bind(b, &t), 0);
		return b;
	}

	void commit(switch_gpu_event_waits *b)
	{
		switch_gpu_event_waits_retain(b);
		switch_gpu_event_waits_commit(&b);
	}
};
}

extern "C" OSObject *switch_gpu_event_native_lookup(void *, uint32_t port)
{
	std::lock_guard held(nativeLock);
	auto it = ports.find(port);
	if (it == ports.end())
		return nullptr;
	it->second->retain();
	return it->second;
}

extern "C" uint64_t switch_gpu_event_native_value(OSObject *object)
{
	return static_cast<NativeEvent *>(object)->value;
}

extern "C" OSObject *switch_gpu_event_native_listener_create(uint32_t)
{
	return new OSObject;
}

extern "C" OSObject *switch_gpu_event_native_waiter_create(OSObject *event, OSObject *listener,
							   uint64_t value, void *context,
							   OSObject *owner,
							   SwitchGpuEventNativeCallback callback)
{
	if (failWaiter)
		return nullptr;
	auto *w = new NativeWaiter;
	w->event = static_cast<NativeEvent *>(event);
	w->listener = listener;
	w->value = value;
	w->context = context;
	w->owner = owner;
	w->callback = callback;
	event->retain();
	listener->retain();
	owner->retain();
	return w;
}

extern "C" void switch_gpu_event_native_waiter_arm(OSObject *, OSObject *object)
{
	auto *w = static_cast<NativeWaiter *>(object);
	std::lock_guard held(nativeLock);
	w->retain();
	(w->event->value >= w->value ? queued : pending).push_back(w);
}

bool switch_gpu_event_native_waiter_cancel(OSObject *, OSObject *object)
{
	auto *w = static_cast<NativeWaiter *>(object);
	bool removed = false;
	{
		std::lock_guard held(nativeLock);
		auto it = std::find(pending.begin(), pending.end(), w);
		if (it != pending.end()) {
			pending.erase(it);
			removed = true;
		}
	}
	if (removed)
		w->release();
	return removed;
}

extern "C" void switch_gpu_event_native_signal(OSObject *object, uint64_t value, bool)
{
	auto *event = static_cast<NativeEvent *>(object);
	std::lock_guard held(nativeLock);
	event->value = std::max(event->value.load(), value);
	for (auto it = pending.begin(); it != pending.end();) {
		auto *w = *it;
		if (w->event == event && w->value <= event->value) {
			queued.push_back(w);
			it = pending.erase(it);
		} else
			++it;
	}
}

extern "C" int switch_gpu_backing_token_retain(const switch_gpu_backing_token *t)
{
	std::lock_guard held(tokensLock);
	auto it = tokens.find(key(*t));
	if (it == tokens.end())
		return -19;
	++it->second.refs;
	return 0;
}

extern "C" void switch_gpu_backing_token_release(const switch_gpu_backing_token *t)
{
	std::lock_guard held(tokensLock);
	ASSERT_GT(tokens[key(*t)].refs, 0);
	--tokens[key(*t)].refs;
}

extern "C" int switch_gpu_backing_token_status(const switch_gpu_backing_token *t)
{
	if (t->engine == statusEngine && statusOverride)
		return statusOverride(t);
	std::lock_guard held(tokensLock);
	return tokens[key(*t)].execution;
}

extern "C" int switch_gpu_backing_token_external_status(const switch_gpu_backing_token *t)
{
	std::lock_guard held(tokensLock);
	return tokens[key(*t)].external;
}

extern "C" int switch_gpu_backing_token_physical_status(const switch_gpu_backing_token *t,
							int external)
{
	if (t->engine == statusEngine && physicalOverride)
		return physicalOverride(t);
	return external ? switch_gpu_backing_token_external_status(t)
			: switch_gpu_backing_token_status(t);
}

extern "C" int switch_gpu_backing_has_semantics()
{
	return !semantics.empty();
}

extern "C" int switch_gpu_backing_semantic_snapshot(const switch_gpu_backing_token *t,
						    switch_gpu_event_waits **out)
{
	*out = nullptr;
	auto at = semantics.find(key(*t));
	if (at == semantics.end())
		return 0;
	const int status = switch_gpu_event_waits_status(at->second);
	if (status < 0)
		return status;
	if (status == 1)
		return 0;
	switch_gpu_event_waits_retain(at->second);
	*out = at->second;
	return 1;
}

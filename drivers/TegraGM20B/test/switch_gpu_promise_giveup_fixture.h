// SPDX-License-Identifier: MIT
// The give-up tests' kernel (switch_gpu_promise_giveup_stubs.h, gpu_promise_giveup.c whole and gpu_promise.c's GPU-reader
// park) and their fixture: a reader channel and a maker channel on one promise, the call run by a clock the test moves.
#pragma once
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "switch_gpu_promise_giveup_stubs.h"
#include "drivers/TegraGM20B/test/promise_giveup_source.h"
#include "drivers/TegraGM20B/test/promise_note_source.h"

namespace promise_giveup
{
constexpr uint64_t kMs = 1000 * 1000;
constexpr uint32_t kOffset = 2052, kValue = 9;
constexpr int kPrevious = 1, kPromised = 2;

struct Kind {
	const char *name;
	const char *owner;  // the reader's channel
	bool present;       // the GPU present's own channel
};

const Kind kKinds[] = {
	{"RawUntimedWaitFence", "Camera", false},   {"WriterWaitIOSurfaceLock", "Photos", false},
	{"FenceOnlyPresent", "present", true},      {"Composite", "backboardd", false},
	{"BridgeVicNvdec", "host1x-bridge", false},
};

class Fixture : public testing::Test
{
protected:
	virtual const char *readerOwner() const
	{
		return "Camera";
	}

	virtual bool readerPresent() const
	{
		return false;
	}

	fake_tgm_chan reader, maker;
	std::vector<std::string> lines;
	uint32_t readerFirst = 0;

	void SetUp() override
	{
		fake::now = 1000 * kMs;
		fake::call = {};
		fake::chans = {&reader, &maker};
		fake::abandoned.clear();
		fake::owner = 1;
		fake::hangLooks = fake::pathAbandoned = fake::directGivenUp = fake::cpuRaised = 0;
		presentReleased = 0;
		fake::poweredDown = false;
		fake::atRegisters = nullptr;
		fake::atAbandon = nullptr;
		giveup_down = 0;
		probedReaders = 0;
		std::memset(&giveups, 0, sizeof(giveups));
		giveup_call = nullptr;
		for (auto &word : fake::pool)
			word = 0;
		fake::pool[kOffset / 4] = kValue - 1;
		std::memset(&tgm_fence_pages, 0, sizeof(tgm_fence_pages));
		tgm_fence_pages.made[0][kOffset / 4 - TGM_FENCE_PROMISE_WORD] = kValue;
		setChan(reader, 0x10, readerOwner(), 40);
		setChan(maker, 0x30, "maker-app", 20);
		tgm_present.chan = readerPresent() ? &reader : nullptr;
		interrupts_off::logged = &lines;
		ASSERT_EQ(tgm_promise_giveup_open(), 0);
	}

	void TearDown() override
	{
		interrupts_off::logged = nullptr;
		EXPECT_EQ(interrupts_off::depth, 0);
		EXPECT_EQ(tgm_live_locked, 0);
	}

	static void setChan(fake_tgm_chan &chan, uint64_t id, const char *owner, uint32_t done)
	{
		chan.id = id;
		chan.error = 0;
		chan.fence = done;
		chan.fence_cpu = &chan.fence;
		chan.submitted = done;
		chan.stall_ns = 0;
		chan.strikes = 0;
		chan.recovery = 0;
		chan.asked.clear();
		std::strncpy(chan.owner, owner, sizeof(chan.owner) - 1);
		switch_gpu_jobs_init(&chan.jobs, done);
	}

	// The reader's job, its acquire on the promise kicked: chan_exec parks it, then fences the job
	void readerSubmits()
	{
		readerFirst = reader.submitted + 1;
		const switch_gpu_backing_token promise = {kValue, 0, SWITCH_GPU_ENGINE_PROMISE,
							  kOffset};
		tgm_chan_park_promises(&reader, &promise, 1);
		reader.submitted = readerFirst;
	}

	// The maker's job that keeps the promise, kicked behind `ahead` jobs of its channel still running
	void makerKeeps(uint32_t ahead)
	{
		const uint32_t first = maker.submitted + 1;
		maker.submitted += 1;
		maker.fence = first - 1 - ahead;
		switch_gpu_jobs_kick(&maker.jobs, first, first, fake::now, 1);
		const switch_gpu_backing_token job = {first, uint32_t(maker.id >> 32) + 1,
						      SWITCH_GPU_ENGINE_GM20B, uint32_t(maker.id)};
		tgm_promise_kept(kOffset, kValue, &maker, &job);
	}

	// The keeping job's end: its fence, then its push's release of the word, then its interrupt's pass
	void makerReleases()
	{
		maker.fence = maker.submitted;
		fake::pool[kOffset / 4] = kValue;
		pass();
	}

	// GPU readers parked on the word go on once it reaches the value
	void gpuRuns()
	{
		if (int32_t(fake::pool[kOffset / 4] - kValue) >= 0)
			reader.fence = reader.submitted;
	}

	// The end of a GPU interrupt pass, where nothing may lock: interrupts off as strictly as a filter
	void pass()
	{
		unsigned long flags = 0;
		local_irq_save(flags);
		tgm_promise_pass();
		local_irq_restore(flags);
	}

	// The clock moves to `t`, the call running whenever it is due
	void until(uint64_t t)
	{
		while (fake::call.pending && fake::call.due <= t) {
			fake::now = fake::call.due;
			fake::call.pending = false;
			++fake::call.ran;
			fake::call.fn(nullptr);
			gpuRuns();
		}
		fake::now = t;
	}

	int status() const
	{
		const switch_gpu_backing_token promise = {kValue, 0, SWITCH_GPU_ENGINE_PROMISE,
							  kOffset};
		return switch_gpu_backing_token_status(&promise);
	}

	// What the panel shows once the reader's present of the surface finishes (finishGpuCopy flips only a 0)
	int shown() const
	{
		return status() == 1 ? kPromised : kPrevious;
	}
};

class GiveUp : public Fixture, public testing::WithParamInterface<Kind>
{
protected:
	const char *readerOwner() const override
	{
		return GetParam().owner;
	}

	bool readerPresent() const override
	{
		return GetParam().present;
	}
};
}  // namespace promise_giveup

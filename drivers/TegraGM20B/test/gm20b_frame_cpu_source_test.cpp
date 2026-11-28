// SPDX-License-Identifier: MIT
// The kernel's CPU work per job, kick and interrupt pass, where no host model runs the unit: a job's kick publishes its
// GPFIFO entries with one barrier, and an interrupt pass wakes only the threads whose condition it met.
#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"

using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::SourceText;

namespace
{
std::string unit(const char *name)
{
	return SourceText(std::string("../") + name);
}
}  // namespace

// nvxnu_kdebug is a function whose body is empty in release, so a call outside TGM_STATS still evaluates its
// arguments there: the room wait's GP_GET read over MMIO on every full ring. Each call, and the helpers that read GP_GET
// and the fence for one, sit inside an `#if TGM_STATS`.
TEST(GM20BFrameCpuSource, GpuTracepointsAndTheReadsBehindThemAreDebugOnly)
{
	std::istringstream exec(unit("gpu_exec.c"));
	std::vector<std::string> open;
	std::string line;
	int calls = 0;
	while (std::getline(exec, line)) {
		if (line.rfind("#if", 0) == 0)
			open.push_back(line);
		else if (line.rfind("#endif", 0) == 0 && !open.empty())
			open.pop_back();
		const bool traced =
			line.find("nvxnu_kdebug(") != std::string::npos ||
			line.find("static u64 chan_ring_position(") != std::string::npos ||
			line.find("static u32 chan_done(") != std::string::npos;
		if (!traced)
			continue;
		++calls;
		bool stats = false;
		for (const std::string &o : open)
			stats = stats || o.find("TGM_STATS") != std::string::npos ||
				o.find("SWITCH_FRAME_TIMELINE") != std::string::npos;
		EXPECT_TRUE(stats) << line;
	}
	EXPECT_GE(calls, 6);
}

// A pass wakes a thread on the global word only while one sleeps there, and moves a sequence it alone writes.
TEST(GM20BFrameCpuSource, ThePassWakesTheGlobalWordOnlyForASleeper)
{
	const std::string glue = unit("shim/xnu_glue.cpp");
	EXPECT_TRUE(Block(glue, "void wakeAfterPass()")
			    .has("if (switch_gpu_wake_pass(&gWake)) wakeGlobal();"));
	EXPECT_TRUE(Block(glue, "extern \"C\" void nvxnu_wake(void)")
			    .has("if (switch_gpu_wake_other(&gWake)) wakeGlobal();"));
	EXPECT_TRUE(Block(glue, "extern \"C\" int nvxnu_idle(nvxnu_u32 seq, nvxnu_u64 deadline_ns)")
			    .has("return nvxnu_wword_idle(&gWake, seq, deadline_ns);"));
	EXPECT_TRUE(Block(glue, "extern \"C\" int nvxnu_wword_idle(")
			    .before("switch_gpu_wake_sleep(word, 1);", "idleOn("));
	EXPECT_TRUE(
		Block(glue, "void gpuInterrupt(OSObject *, IOInterruptEventSource *source, int)")
			.has("wakeAfterPass();"));
}

// The kernel's waits on one channel's fence sleep on that channel's word, woken by the pass that moves its fence. The
// room wait stays on the global word: its GET report lands after the fence's release, so a pass between the two finds
// no room and the job's own pass sees no progress to wake it for.
TEST(GM20BFrameCpuSource, ChannelWaitsSleepOnTheirChannel)
{
	const CodeText room =
		Block(unit("gpu_exec.c"),
		      "int tgm_chan_wait_ready(struct tgm_chan *chan, u32 gpfifo_nr, u32 push_nr)");
	EXPECT_TRUE(room.has("nvxnu_idle(seq, end)"));
	EXPECT_FALSE(room.has("tgm_waiter_idle("));
	for (const char *wait : {"static int sync_wait(u64 id, u32 seqno, u64 end)",
				 "static void writer_wait(u32 uid, u32 seqno,"}) {
		const CodeText body = Block(unit("gpu_sync.c"), wait);
		EXPECT_FALSE(body.has("nvxnu_idle(")) << wait;
		EXPECT_TRUE(body.has("tgm_waiter_idle(")) << wait;
	}
	const CodeText del =
		Block(unit("gpu_chan.c"), "void tgm_chan_del(struct tgm_chan **pchan)");
	EXPECT_FALSE(del.has("nvxnu_idle(")) << "the teardown's drain sleeps on the global word";
	EXPECT_TRUE(del.has("tgm_waiter_idle("));
}

// A slow app's kick into a frame whose window was kept takes its holds after the kick's barrier and before its doorbell
// (gm20b_deadline_window_test runs the holds); only a user channel's kick looks.
TEST(GM20BFrameCpuSource, AnAppsFirstKickIsHeldBeforeItsDoorbell)
{
	const CodeText fence = Block(unit("gpu_exec.c"), "static u32 chan_fence_job(");
	EXPECT_TRUE(fence.before("WRITE_ONCE(chan->submitted, next);",
				 "switch_gpu_busy_kick(&tgm_busy, chan->chid, next);"));
	EXPECT_TRUE(fence.before("switch_gpu_busy_kick(&tgm_busy, chan->chid, next);",
				 "chan_doorbell(chan);"));
	const CodeText doorbell =
		Block(unit("gpu_exec.c"), "static void chan_doorbell(struct tgm_chan *chan)");
	EXPECT_TRUE(
		doorbell.before("wmb();", "chan->app && !chan->high && tgm_deadline_app_kicked()"));
	EXPECT_TRUE(doorbell.before("tgm_deadline_app_kicked()",
				    "writel_relaxed(chan->chan.gpfifo.cur,"));
	EXPECT_EQ(doorbell.count("wmb();"), 2u)
		<< "the kick's one, and one after a hold's register writes";
}

// No atomic read-modify-write or lock of the kernel's on a job's kick or its interrupt pass for the idle clock or the
// channel words: the idle clock reads tgm_busy, and the pass moves the words' own sequence (SwitchGpuWakeWord.h).
TEST(GM20BFrameCpuSource, TheKickAndThePassKeepNoSharedCounters)
{
	EXPECT_FALSE(Block(unit("gpu_exec.c"), "static u32 chan_fence_job(").has("idle_kicked("));
	const CodeText pass =
		Block(unit("SwitchGpuChannelCompletion.h"), "static int chan_completion(");
	EXPECT_FALSE(pass.has("idle_fenced("));
	EXPECT_TRUE(
		Block(unit("gpu_chan.c"), "static void chan_wake_waiters(struct tgm_chan *chan)")
			.has("chan_wake_all_waiters(chan, 1);"));
	EXPECT_TRUE(Block(unit("gpu_inline.h"), "static inline void chan_wake_all_waiters(")
			    .has("nvxnu_wword_wake(&tgm_waiter_words[waiter], pass);"));
}

// A job's kick arms no per-channel completion event and the pass completes no event: the pass scans tgm_busy.
TEST(GM20BFrameCpuSource, NoPerJobCompletionEvent)
{
	const CodeText fence = Block(unit("gpu_exec.c"), "static u32 chan_fence_job(");
	EXPECT_FALSE(fence.has("switch_gpu_irq_arm(")) << "an exchange per kick";
	EXPECT_FALSE(fence.has("nvif_event_allow(")) << "nvkm's event lock per kick";
	EXPECT_FALSE(
		Block(unit("gpu_quiet.c"), "void tgm_chan_wake_push(").has("nvif_event_allow("));
	const CodeText completion(unit("SwitchGpuChannelCompletion.h"));
	EXPECT_FALSE(completion.has("switch_gpu_irq_complete(")) << "a compare-and-swap per pass";
	EXPECT_FALSE(completion.has("NVIF_EVENT_DROP")) << "nvkm's event lock per pass";
	EXPECT_FALSE(Block(unit("nouveau/nvkm/engine/fifo/gk104.c"),
			   "gk104_fifo_intr(struct nvkm_inth *inth)")
			     .has("nvkm_event_ntfy"))
		<< "nvkm's event list lock per interrupt";
	EXPECT_TRUE(Block(unit("shim/xnu_glue.cpp"),
			  "void gpuInterrupt(OSObject *, IOInterruptEventSource *source, int)")
			    .before("nvxnu_poll_irqs();", "tgm_chans_completed();"));
	EXPECT_TRUE(Block(unit("gpu_chan.c"), "void tgm_chan_del(struct tgm_chan **pchan)")
			    .before("switch_gpu_chan_table_set(&tgm_chan_table, chan->chid, NULL);",
				    "nvxnu_pass_barrier();"));
	EXPECT_TRUE(Block(unit("gpu_chan.c"), "void tgm_chan_del(struct tgm_chan **pchan)")
			    .before("nvxnu_pass_barrier();", "chan_free(chan);"));
}

// A present job's frame is learned at the pass's end, after the scan: a compositor channel the scan completes after the
// present's channel (a higher chid) still adds its GPU time to the frame it belongs to.
TEST(GM20BFrameCpuSource, APresentJobsFrameIsLearnedAfterTheScan)
{
	const CodeText fenced = Block(unit("gpu_fenced.h"), "static inline void present_fenced(");
	EXPECT_FALSE(fenced.has("tgm_deadline_presented("));
	EXPECT_FALSE(fenced.has("nvxnu_work_schedule(tgm_present.completion_work);"));
	const CodeText end = Block(unit("gpu_present.c"), "void tgm_present_pass(void)");
	EXPECT_TRUE(end.before("tgm_deadline_presented(tgm_present.fenced_queued, 1);",
			       "nvxnu_work_schedule(tgm_present.completion_work);"));
	EXPECT_TRUE(Block(unit("shim/xnu_glue.cpp"),
			  "void gpuInterrupt(OSObject *, IOInterruptEventSource *source, int)")
			    .before("tgm_chans_completed();", "tgm_present_pass();"));
}

// The interrupt pass's own counts have one writer, the work loop, whose runs never overlap: a load and a store, and no
// exchange guarding nvkm's handler against a second caller there is none of.
TEST(GM20BFrameCpuSource, ThePassTakesNoRmwOfItsOwn)
{
	EXPECT_FALSE(Block(unit("shim/linux.c"), "int nvxnu_poll_irqs(void)")
			     .has("__atomic_compare_exchange_n"));
	const CodeText pass =
		Block(unit("shim/xnu_glue.cpp"),
		      "void gpuInterrupt(OSObject *, IOInterruptEventSource *source, int)");
	EXPECT_FALSE(pass.has("__atomic_fetch_add"));
	EXPECT_FALSE(pass.has("__atomic_exchange_n"));
}

// SPDX-License-Identifier: MIT
// Quiet job ends (SwitchGpuQuiet.h) need every kernel sleep on a user channel's fence to ask for its interrupt first,
// and every lost-wake panic to be keyed to the interrupt that wakes it, not to the fence passing the seqno.
// switch_gpu_quiet_test runs the protocol; these guard that each waiter takes part in it.
#include <gtest/gtest.h>

#include <string>

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

TEST(GM20BQuietSource, TheFenceInterruptsOnlyWhenItAnnounces)
{
	const CodeText fence = Block(unit("gpu_exec.c"), "static u32 chan_fence_job(");
	EXPECT_TRUE(
		fence.Before("switch_gpu_quiet_announces(&chan->quiet, quiet)", "if (announce) {"));
	EXPECT_EQ(fence.Count("NON_STALL_INTERRUPT"), 1u);
	EXPECT_TRUE(fence.Before("if (announce) {", "NON_STALL_INTERRUPT"));
	EXPECT_TRUE(
		fence.Before("switch_gpu_quiet_announce(&chan->quiet, next);", "PUSH_KICK(push);"));
	const CodeText exec =
		Block(unit("gpu_exec.c"),
		      "static inline __attribute__((always_inline)) int chan_exec_body(");
	EXPECT_TRUE(exec.Has("(visibility & SWITCH_GPU_SUBMIT_QUIET) &&"));
	EXPECT_TRUE(exec.Has("__atomic_load_n(&chan->wake, __ATOMIC_ACQUIRE)"));
	const CodeText job(unit("TegraGM20BUserClientJobSubmit.cpp"));
	EXPECT_TRUE(job.Has(
		"(head.flags & TegraGM20BJobQuiet) && !(head.flags & TegraGM20BJobHasEvents)"));
	// a semantic fence bound to the job (its own, or one a dependency passed on) resolves at a published completion, and
	// a watched present's producer writes a backing the display scans in place
	EXPECT_TRUE(job.Before("error = events->bind(&completion);",
			       "switch_gpu_quiet_end(quiet, events->semanticFence(),"));
	EXPECT_TRUE(job.Before("scanoutWrites |= switch_gpu_backing_scanned(bo->backing) != 0;",
			       "switch_gpu_quiet_end(quiet, events->semanticFence(),"));
	EXPECT_TRUE(job.Has("scanoutWrites, closesGroup);"));
	EXPECT_TRUE(job.Has("!(head.flags & TegraGM20BJobAdmission) || (head.flags & "
			    "TegraGM20BJobAdmissionEnd);"));
	EXPECT_TRUE(job.Before("const bool quietEnd",
			       "(quietEnd ? uint32_t(SWITCH_GPU_SUBMIT_QUIET) : 0u)"));
	EXPECT_TRUE(job.Has("(jobVisibility & SWITCH_GPU_SUBMIT_QUIET) && count ? 1 : 0"));
}

TEST(GM20BQuietSource, EverySleepOnAUserChannelAsksFirst)
{
	const std::string chan = unit("gpu_chan.c"), sync = unit("gpu_sync.c"),
			  power = unit("gpu_power.c");
	EXPECT_TRUE(Block(chan, "void tgm_chan_want(")
			    .Before("switch_gpu_waiter_want(", "tgm_chan_need(chan, seqno);"));
	EXPECT_TRUE(Block(chan, "void tgm_chan_del(")
			    .Before("tgm_chan_quiet_wake(chan);", "nvxnu_idle(seq, end)"));
	EXPECT_TRUE(Block(sync, "static int sync_wait(")
			    .Before("tgm_chan_need_id(id, seqno);", "nvxnu_idle(seq, end)"));
	EXPECT_TRUE(Block(sync, "static void writer_wait(")
			    .Before("tgm_chan_need_uid(uid, seqno);", "nvxnu_idle(seq, end)"));
	EXPECT_TRUE(Block(power, "static void sleep_fences(")
			    .Before("tgm_chan_wake_push(chan);", "nvxnu_idle(seq, end)"));
	const CodeText room = Block(unit("gpu_exec.c"), "int tgm_chan_wait_ready(");
	EXPECT_TRUE(room.Before("switch_gpu_quiet_need(&chan->quiet, chan->submitted)",
				"tgm_chan_wake_push(chan);"));
	EXPECT_TRUE(room.Before("tgm_chan_wake_push(chan);", "nvxnu_idle(seq, end)"));
	EXPECT_TRUE(Block(unit("SwitchGpuClientRetirement.h"), "void AGXDeviceUserClient::retire(")
			    .Has("tgm_chan_need(chans_[slot], "
				 "uint32_t(record->snapshots[slot].value));"));
	EXPECT_TRUE(CodeText(unit("TegraGM20BUserClient.cpp"))
			    .Before("tgm_chan_want(chan, in->seqno);", "tgm_waiter_idle("));
}

// A token is asked only of the GM20B channel it names, by uid and reset generation: a host1x engine's token holds a
// syncpoint in its queue. A bare uid is asked only by writer_wait, whose uids are GM20B channels'.
TEST(GM20BQuietSource, ATokenIsAskedOnlyOfItsChannel)
{
	EXPECT_TRUE(Block(unit("gpu_quiet.c"), "void tgm_chan_need_tokens(")
			    .Before("switch_gpu_quiet_token_on(&tokens[i], chan->id)",
				    "tgm_chan_need(chan, (u32)tokens[i].value);"));
	size_t uids = 0;
	for (const char *name : {"gpu_bringup.c", "gpu_chan.c", "gpu_sync.c", "gpu_power.c",
				 "gpu_exec.c", "gpu_present.c", "gpu_scrub.c", "gpu_hang.c"})
		uids += CodeText(unit(name)).Count("tgm_chan_need_uid(");
	EXPECT_EQ(uids, 1u) << "a token's channel is asked through tgm_chan_need_tokens";
	EXPECT_TRUE(Block(unit("gpu_sync.c"), "static void writer_wait(")
			    .Has("tgm_chan_need_uid(uid, seqno);"));
}

// A user channel's lost-wake panic fires once the interrupt that wakes the wait was due; the two left on the fence alone
// wait on kernel channels, which never end quiet.
TEST(GM20BQuietSource, LostWakePanicsWaitForTheAnnouncingInterrupt)
{
	const std::string chan = unit("gpu_chan.c"), sync = unit("gpu_sync.c"),
			  power = unit("gpu_power.c");
	EXPECT_TRUE(Block(chan, "void tgm_chan_del(")
			    .Has("tgm_chan_announced(chan, chan->submitted)) nvxnu_wake_due("));
	EXPECT_TRUE(Block(sync, "static int sync_wait(").Has("tgm_chan_announced_id(id, seqno))"));
	EXPECT_TRUE(
		Block(sync, "static void writer_wait(").Has("tgm_chan_announced_uid(uid, seqno))"));
	EXPECT_TRUE(
		Block(power, "static void sleep_fences(").Has("tgm_chan_announced_id(id, seqno))"));
	EXPECT_TRUE(
		Block(unit("TegraGM20BUserClient.cpp"), "IOReturn AGXDeviceUserClient::waitFence(")
			.Has("tgm_chan_announced(chan, in->seqno)"));
	size_t panics = 0;
	for (const char *name :
	     {"gpu_bringup.c", "gpu_chan.c", "gpu_sync.c", "gpu_power.c", "gpu_exec.c",
	      "gpu_present.c", "gpu_scrub.c", "gpu_hang.c", "gpu_quiet.c"})
		panics += CodeText(unit(name)).Count("nvxnu_wake_due(");
	EXPECT_EQ(panics, 6u) << "a new fence wait: ask for its interrupt and key its panic to it";
	EXPECT_TRUE(Block(power, "static void wake_fence(")
			    .Has("tgm_scrub.chan ? tgm_scrub.chan : tgm_present.chan"));
}

// The wake entry is pushed in SubmitJob's order (the channel's submit lock, then the submit gate) and inside the GPU's
// gate, so it never interleaves with a submission and never reaches a GPU that is asleep.
TEST(GM20BQuietSource, WakeEntriesKeepTheSubmissionOrder)
{
	const CodeText wake = Block(unit("TegraGM20BUserClientWake.cpp"),
				    "void AGXDeviceUserClient::pushWakes()");
	EXPECT_TRUE(wake.Before("IOLockLock(submitLocks_[slot]);", "submitGate_.enterFast()"));
	EXPECT_TRUE(wake.Before("submitGate_.enterFast()", "tgm_chan_quiet_wake(chans_[slot]);"));
	const std::string quiet = unit("gpu_quiet.c");
	EXPECT_TRUE(Block(quiet, "void tgm_chan_quiet_wake(")
			    .Before("tgm_gate_enter();", "tgm_chan_wake_push(chan);"));
	const CodeText push = Block(quiet, "void tgm_chan_wake_push(");
	EXPECT_TRUE(push.Before("NON_STALL_INTERRUPT", "PUSH_KICK(push);"));
	EXPECT_TRUE(push.Before("switch_gpu_irq_arm(", "PUSH_KICK(push);"));
	EXPECT_TRUE(push.Has("nvxnu_panic("));
	EXPECT_TRUE(Block(unit("TegraGM20BUserClientChannel.cpp"),
			  "IOReturn AGXDeviceUserClient::channelCreate(")
			    .Has("tgm_chan_on_wake(chans_[slot], wakeAsked, this, slot);"));
	EXPECT_TRUE(Block(unit("SwitchGpuClientRetirement.h"),
			  "void AGXDeviceUserClient::drainResources()")
			    .Before("tgm_chan_del(&chan);", "thread_call_cancel_wait(wakeCall_);"));
}

// The idle clock counts a quiet job's seqnos once something reads its fence: its tick settles them when nothing was
// kicked for its drop time.
TEST(GM20BQuietSource, TheIdleClockSettlesQuietEnds)
{
	EXPECT_TRUE(Block(unit("SwitchGpuIdleDeadline.h"), "void tegra_gm20b_idle_tick(void)")
			    .Before("if (idle_in_flight()) {",
				    "nvxnu_work_schedule(tgm_idle.settle_w);"));
	const CodeText settle = Block(unit("gpu_quiet.c"), "void tgm_idle_settle(void *arg)");
	EXPECT_TRUE(settle.Before("idle_retire(chan, tgm_chan_completed(chan));",
				  "tgm_chan_need(chan, READ_ONCE(chan->submitted));"));
	EXPECT_FALSE(settle.Has("nvxnu_idle("));
}

// Writer promises (SwitchGpuWriterPromise.h): a present and another connection's job wait for them on the GPU; the job
// that keeps one releases it after its own fence; the CPU releases the rest; nothing waits for its own promise; no lease
// a present or a display slot took behind one holds the promised job's write off; a CPU reader waits for the promised
// job.
TEST(GM20BPromiseSource, PresentsAndJobsOrderBehindWriterPromises)
{
	EXPECT_TRUE(
		Block(unit("SwitchGpuScanoutSnapshot.h"), "int add(switch_gpu_backing *backing)")
			.Before("switch_gpu_backing_snapshot_read_promises(", "tokens + count,"));
	const CodeText job(unit("TegraGM20BUserClientJobSubmit.cpp"));
	EXPECT_TRUE(job.Before("keepPromises(record.promises, releases)",
			       "switch_gpu_chan_submit_enter(chan);"));
	EXPECT_TRUE(job.Before(
		"events->captureDependencies(job.dependencies, dependencies);",
		"switch_gpu_backing_promise_dependencies(&transaction, job.dependencies,"));
	EXPECT_TRUE(job.Before(
		"switch_gpu_backing_promise_dependencies(&transaction, job.dependencies,",
		"switch_gpu_chan_exec_events("));
	// between begin and commit the transaction holds every use's backing lock: a promise read that takes one again
	// (switch_gpu_backing_promise_tokens) never returns on XNU
	EXPECT_FALSE(job.Has("switch_gpu_backing_promise_tokens("));
	EXPECT_TRUE(job.Before("switch_gpu_backing_commit(&transaction);",
			       "keptPromises(record.promises, slot, completion);"));
	const std::string exec = unit("gpu_exec.c");
	EXPECT_TRUE(Block(exec, "static u32 chan_fence_job(")
			    .Before("lower_32_bits(chan->fence_gpu)", "releases[s].value"));
	// A GPU reader parks on a pending promise once its acquires are kicked, never while they can still be rolled back:
	// a give-up armed for a push that was not submitted would drop a later frame whose writes did arrive.
	const CodeText acquire = Block(exec, "int tgm_backing_dependencies(");
	EXPECT_TRUE(acquire.Has("token->engine == SWITCH_GPU_ENGINE_PROMISE"));
	EXPECT_FALSE(acquire.Has("tgm_chan_note_promise("));
	const CodeText kick =
		Block(exec, "static inline __attribute__((always_inline)) int chan_exec_body(");
	EXPECT_TRUE(kick.Before(
		"ret = tgm_backing_dependencies(chan, dependencies, dependency_count, 1);",
		"PUSH_KICK(&chan->chan.push);"));
	EXPECT_TRUE(kick.Before("PUSH_KICK(&chan->chan.push);",
				"tgm_chan_park_promises(chan, dependencies, dependency_count);"));
	const CodeText copy = Block(unit("gpu_present.c"), "static int present_copy(");
	EXPECT_TRUE(copy.Before(
		"ret = tgm_backing_dependencies(chan, tgm_present.tokens.tokens, dependency_count, "
		"1);",
		"tgm_chan_park_promises(chan, tgm_present.tokens.tokens, dependency_count);"));
	EXPECT_TRUE(copy.Before(
		"tgm_chan_park_promises(chan, tgm_present.tokens.tokens, dependency_count);",
		"chan_fence(chan);"));
	// the hang rule raises no promise: a reader parked on one is a victim it spares, and the give-up is the one place
	EXPECT_FALSE(Block(unit("gpu_hang.c"), "enum tgm_hang tgm_hang_recover(").Has("promise"));
	// The raise is a plain store-release to the write-combined fence page, which the parked PBDMA reads from DRAM
	// (volatile, no L2): a DSB completes it, as it does after the CPU's own release (releasePromise). No exclusive
	// access (compare-and-swap, atomic read-modify-write) reaches the pool: T210's bus answers one with an SError.
	const std::string giveup = unit("gpu_promise_giveup.c");
	const CodeText raise = Block(giveup, "static int promise_raise(");
	EXPECT_TRUE(raise.Before("local_irq_save(flags);",
				 "__atomic_store_n(word, value, __ATOMIC_RELEASE);"));
	EXPECT_TRUE(raise.Before("__atomic_store_n(word, value, __ATOMIC_RELEASE);",
				 "local_irq_restore(flags);"));
	for (const CodeText &body :
	     {raise, Block(unit("SwitchGpuWriterPromise.h"),
			   "static inline int switch_gpu_promise_give_up(")}) {
		EXPECT_FALSE(body.Has("__atomic_compare_exchange"));
		EXPECT_FALSE(body.Has("__atomic_fetch_"));
		EXPECT_FALSE(body.Has("__atomic_exchange"));
	}
	// switch_gpu_promise_giveup_test runs these steps: the mark first, taken back if the maker won, the DSB, one line
	const CodeText stepped = Block(giveup, "static void giveup_stepped(");
	EXPECT_TRUE(stepped.Before("step == SWITCH_GPU_PROMISE_RAISING",
				   "tgm_promise_abandon(offset, value);"));
	EXPECT_TRUE(stepped.Before("step == SWITCH_GPU_PROMISE_KEPT_FIRST",
				   "tgm_promise_kept_first(offset, value);"));
	EXPECT_TRUE(stepped.Before("tgm_promise_kept_first(offset, value);", "giveup_report("));
	EXPECT_EQ(stepped.Count("giveup_report("), 1u);
	// the raise's own DSB, inside its window, and its guard around the look and the store
	EXPECT_TRUE(raise.Before("switch_gpu_promise_raise_mark(&tgm_fence_pages.raising, offset);",
				 "switch_gpu_promise_raise_made_still(made, value)"));
	EXPECT_TRUE(raise.Before("__atomic_store_n(word, value, __ATOMIC_RELEASE);",
				 "__asm__ volatile(\"dsb sy\" ::: \"memory\");"));
	EXPECT_TRUE(raise.Before("__asm__ volatile(\"dsb sy\" ::: \"memory\");",
				 "switch_gpu_promise_raise_unmark(&tgm_fence_pages.raising);"));
	EXPECT_TRUE(raise.Before("switch_gpu_promise_raise_unmark(&tgm_fence_pages.raising);",
				 "local_irq_restore(flags);"));
	// the page's guard row is zeroed with the page at hand-out
	EXPECT_TRUE(Block(unit("gpu_chan.c"), "int tgm_fence_page_get(")
			    .Before("memset((u8 *)tgm_fence_pages.va + i * TGM_FENCE_SLOT, 0, "
				    "PAGE_SIZE);",
				    "memset(tgm_fence_pages.made[i], 0, "
				    "sizeof(tgm_fence_pages.made[i]));"));
	// the call's look at the end of every GPU interrupt pass, after the present's
	EXPECT_TRUE(CodeText(unit("shim/xnu_glue.cpp"))
			    .Before("tgm_present_pass(); tgm_promise_pass();", "nvxnu_wake();"));
	// The give-up is the promise's one cadence: a present's deadline and a wait's hang checks keep their own period
	// whatever the waited channel acquires, and nothing in the hang rule reads a promise.
	EXPECT_TRUE(copy.Before(
		"tgm_present.recheck = TGM_PRESENT_NS;",
		"nvxnu_work_schedule_after(tgm_present.deadline_work, tgm_present.recheck);"));
	EXPECT_TRUE(copy.Before(
		"nvxnu_work_schedule_after(tgm_present.deadline_work, tgm_present.recheck);",
		"chan_fence(chan);"));
	EXPECT_TRUE(
		Block(unit("gpu_hang.c"), "static u64 progress_step(").Has("return TGM_HANG_NS;"));
	EXPECT_TRUE(Block(unit("gpu_hang.c"), "uint64_t tgm_chan_check_progress(")
			    .Has("tgm_chan_stalled(chan, who, seqno, stalled);"));
	EXPECT_FALSE(CodeText(unit("gpu_hang.c")).Has("promise"));
	EXPECT_FALSE(CodeText(unit("TegraGM20BRecover.h")).Has("promise"));
	const CodeText deadline =
		Block(unit("gpu_present_deadline.c"), "void tgm_present_deadline(void *arg)");
	EXPECT_TRUE(deadline.Before("if (age < tgm_present.recheck) {",
				    "tgm_hang_recover(late, \"present\""));
	EXPECT_TRUE(deadline.Before(
		"tgm_present.recheck = TGM_PRESENT_NS;",
		"nvxnu_work_schedule_after(tgm_present.deadline_work, tgm_present.recheck);"));
	EXPECT_FALSE(deadline.Has("promise"));
	// The ACQUIRE probe is a debug kernel's line, never a panic: looked at by the hang checks and the stats window.
	const std::string probe = unit("gpu_acquire.c");
	EXPECT_TRUE(CodeText(probe).Before("#if TGM_STATS\n/* The ACQUIRE probe",
					   "void tgm_acquire_probe_look("));
	EXPECT_TRUE(
		CodeText(probe).Before("void tgm_acquire_probe_window(",
				       "#endif\n/* tgm_live_lock held: the rule for a channel"));
	EXPECT_FALSE(Block(probe, "void tgm_acquire_probe_look(").Has("panic"));
	EXPECT_TRUE(Block(probe, "static void acquire_timed_out(")
			    .Has("#if TGM_STATS\n\tprobe.fired++;\n#endif"));
	EXPECT_TRUE(
		Block(unit("gpu_hang.c"), "enum tgm_hang tgm_hang_recover(")
			.Has("#if TGM_STATS\n\t\ttgm_acquire_probe_look(chans[i], now);\n#endif"));
	EXPECT_TRUE(Block(unit("gpu_stats_tick.c"), "void tgm_stats_tick(void)")
			    .Has("tgm_acquire_probe_window(line, sizeof(line), now);"));
	// and at a give-up, the one look a promise park gets past its first second
	EXPECT_FALSE(Block(probe, "void tgm_acquire_probe_giveup(").Has("panic"));
	EXPECT_TRUE(stepped.Before("#if TGM_STATS\n\t\tgiveup_probe(e);",
				   "tgm_promise_abandon(offset, value);"));
	EXPECT_TRUE(CodeText(unit("SwitchGpuAdmission.cpp"))
			    .Has("switch_gpu_backing_exclusive_readers(backing)"));
	EXPECT_TRUE(CodeText(unit("SwitchGpuOwnership.cpp"))
			    .Has("switch_gpu_backing_exclusive_readers(backing)"));
	EXPECT_TRUE(CodeText(unit("SwitchGpuCpuAccess.cpp")).Has("--backing->promised_readers;"));
	EXPECT_TRUE(Block(unit("SwitchGpuCpuAccess.cpp"), "bool cpu_conflicts(")
			    .Has("switch_gpu_backing_promise_pending_locked(backing)"));
	EXPECT_TRUE(
		Block(unit("SwitchGpuCpuLease.cpp"), "static int snapshot_read(")
			.Before("while (!avoid &&",
				"(cpu && switch_gpu_backing_promise_pending_locked(backing))))"));
	// an engine reader of a promised surface (VIC, NVDEC) orders behind the promise through the bridge
	for (const char *reader :
	     {"../TegraVIC/SwitchVICJobs.cpp", "../TegraNVDEC/SwitchNVDECJobs.cpp"})
		EXPECT_TRUE(CodeText(unit(reader))
				    .Before("switch_gpu_backing_promise_dependencies(&transaction,",
					    "dependency.engine == SWITCH_GPU_ENGINE_PROMISE"));
	const std::string retirement = unit("SwitchGpuClientRetirement.h");
	EXPECT_TRUE(Block(retirement, "void AGXDeviceUserClient::drainResources()")
			    .Before("releasePromises(true);", "tgm_chan_del(&chan);"));
	EXPECT_TRUE(Block(retirement, "void AGXDeviceUserClient::retireBatch()")
			    .Has("releasePromises(false);"));
	EXPECT_TRUE(Block(unit("TegraGM20BUserClientChannel.cpp"),
			  "IOReturn AGXDeviceUserClient::channelDestroy(")
			    .Has("releasePromises(false);"));
}

// SPDX-License-Identifier: MIT
// SubmitJob's global locks that switch_gpu_submit_cost_test cannot run: its exec's producer channels resolved under one
// tgm_live_lock a pass, every status read before it, the job's ownership transaction taking no reference of its own on
// its uses (the connection's BOs hold them inside the submit gate), system sleep's gate counting per CPU, and every
// method running without IOKit's read lock on the connection.
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

TEST(GM20BSubmitLocksSource, ProducerChannelsResolveUnderOneLiveLockAPass)
{
	const std::string exec = unit("gpu_exec.c");
	const CodeText all = Block(exec, "int tgm_backing_dependencies(");
	EXPECT_EQ(all.Count("mutex_lock(&tgm_live_lock)"), 1u);
	EXPECT_TRUE(all.Before("switch_gpu_backing_token_status(&tokens[i])",
			       "mutex_lock(&tgm_live_lock)"));
	EXPECT_TRUE(all.Before("mutex_lock(&tgm_live_lock)",
			       "backing_dependency(chan, &tokens[i], emit)"));
	const CodeText one = Block(exec, "static int backing_dependency(");
	EXPECT_TRUE(one.Has("live_by_uid(token->queue)"));
	EXPECT_FALSE(one.Has("mutex_lock("));
	EXPECT_FALSE(one.Has("switch_gpu_backing_token_status("));
}

TEST(GM20BSubmitLocksSource, SubmitJobTakesNoReferencePerUse)
{
	const CodeText job = Block(unit("TegraGM20BUserClientJobSubmit.cpp"),
				   "IOReturn AGXDeviceUserClient::runJobUses(");
	EXPECT_TRUE(job.Has("switch_gpu_backing_begin_held("));
	EXPECT_FALSE(job.Has("switch_gpu_backing_begin_checked("));
	EXPECT_FALSE(job.Has("switch_gpu_backing_begin_conversion_checked("));
}

// A job's records go up at exec's point of no return: past the last step that can still fail and roll its push back,
// before the dependencies' acquires (tgm_live_lock), the GPFIFO writes and the doorbell, so none of those runs under its
// backings' locks. The emitting pass skips a producer gone since the checking pass, and both submission paths hand exec
// their transaction instead of committing it after the kick (switch_gpu_kick_race_test runs what that buys).
TEST(GM20BSubmitLocksSource, AJobsRecordsGoUpBeforeItsKick)
{
	const std::string exec = unit("gpu_exec.c");
	const CodeText body =
		Block(exec, "static inline __attribute__((always_inline)) int chan_exec_body(");
	EXPECT_EQ(body.Count("chan->chan.gpfifo.cur = ring_cur;"), 1u);
	EXPECT_TRUE(body.AfterLast("chan->chan.gpfifo.cur = ring_cur;")
			    .Has("switch_gpu_backing_commit(owned);"));
	const CodeText published = body.AfterLast("switch_gpu_backing_commit(owned);");
	EXPECT_TRUE(published.StartsWith(
		"tgm_backing_dependencies(chan, dependencies, dependency_count, 1);"));
	EXPECT_TRUE(published.Has("PUSH_KICK(&chan->chan.push);"));
	EXPECT_FALSE(published.Has("return ret;"));
	EXPECT_FALSE(published.Has("return -"));
	EXPECT_TRUE(
		Block(exec, "static int backing_dependency(").Has("return emit ? 0 : -ENODEV;"));
	const CodeText job = Block(unit("TegraGM20BUserClientJobSubmit.cpp"),
				   "IOReturn AGXDeviceUserClient::runJobUses(");
	EXPECT_TRUE(job.Has("jobVisibility, releases, keptCount, &transaction);"));
	EXPECT_FALSE(job.Has("switch_gpu_backing_commit("));
	const CodeText legacy = Block(unit("TegraGM20BUserClientSubmit.cpp"),
				      "IOReturn AGXDeviceUserClient::exec(");
	EXPECT_TRUE(legacy.Has("jobVisibility, nullptr, 0, &transaction);"));
	EXPECT_FALSE(legacy.Has("switch_gpu_backing_commit("));
}

// System sleep's gate is on every submission's path: an open enter or leave counts on the caller's own CPU's cell with
// preemption off, a plain store and a fence, so it writes no line another CPU writes. gm20b_sleep_test runs the counts
// with callers moving between CPUs.
TEST(GM20BSubmitLocksSource, TheSleepGateCountsOnTheCallersCpu)
{
	const std::string gate = unit("TegraGM20BSleepGate.h");
	for (const char *step :
	     {"static inline enum tgm_gate_entry tgm_gate_try_enter(",
	      "static inline int tgm_gate_leave(", "static inline void tgm_gate_count("}) {
		const CodeText body = Block(gate, step);
		for (const char *shared :
		     {"_fetch", "fetch_", "compare_exchange", "__atomic_exchange", "mutex", "lock"})
			EXPECT_FALSE(body.Has(shared)) << step;
	}
	const std::string inl = unit("gpu_inline.h");
	const CodeText enter = Block(inl, "static inline enum tgm_gate_entry gate_try_enter(void)");
	const CodeText leave = Block(inl, "static inline void gate_leave(void)");
	EXPECT_TRUE(enter.Before("_disable_preemption();",
				 "tgm_gate_try_enter(&tgm_sc7_gate, (unsigned int)cpu_number());"));
	EXPECT_TRUE(enter.Before("tgm_gate_try_enter(", "_enable_preemption();"));
	EXPECT_TRUE(leave.Before("_disable_preemption();",
				 "tgm_gate_leave(&tgm_sc7_gate, (unsigned int)cpu_number());"));
	EXPECT_TRUE(leave.Before("tgm_gate_leave(", "_enable_preemption();"));
	EXPECT_TRUE(leave.Before("_enable_preemption();", "gate_wake_closer();"));
	EXPECT_EQ(CodeText(inl + unit("gpu_bringup.c")).Count("tgm_gate_try_enter("), 1u);
	EXPECT_TRUE(Block(unit("gpu_bringup.c"), "void tgm_gate_enter(void)")
			    .Has("while ((entry = gate_try_enter()) != TGM_GATE_IN)"));
}

// IOKit's default locking took this connection's read lock around every method, SubmitJob included. The connection turns
// it off and counts its entries per CPU instead (SwitchGpuMethodGate.h); its close shuts that gate and waits for them
// before it tears anything down.
TEST(GM20BSubmitLocksSource, MethodsRunWithoutIOKitsReadLock)
{
	const std::string client = unit("TegraGM20BUserClient.cpp");
	EXPECT_TRUE(Block(client, "bool AGXDeviceUserClient::setUp(task_t task)")
			    .Has("setProperty(kIOUserClientDefaultLockingKey, kOSBooleanFalse);"));
	const CodeText method = Block(client, "IOReturn AGXDeviceUserClient::externalMethod(");
	EXPECT_TRUE(method.Before("if (methods_.enter(lock_))",
				  "externalCall(selector, arguments, asyncReference)"));
	EXPECT_TRUE(method.Before("externalCall(selector, arguments, asyncReference)",
				  "methods_.leave(lock_);"));
	const CodeText memory = Block(client, "IOReturn AGXDeviceUserClient::clientMemoryForType(");
	EXPECT_TRUE(memory.Before("if (!methods_.enter(lock_))",
				  "memoryForType(type, options, memory)"));
	EXPECT_TRUE(
		memory.Before("memoryForType(type, options, memory)", "methods_.leave(lock_);"));
	const CodeText close = Block(client, "IOReturn AGXDeviceUserClient::clientClose()");
	EXPECT_TRUE(close.Before("methods_.close(lock_);", "switch_gpu_compiler_close(this);"));
	EXPECT_TRUE(close.Before("methods_.close(lock_);", "drainResources();"));
	EXPECT_EQ(Block(unit("TegraGM20BUserClientInternal.h"),
			"class AGXDeviceUserClient : public IOUserClient")
			  .Count("APPLE_KEXT_OVERRIDE"),
		  4u)
		<< "clientClose, free, clientMemoryForType and externalMethod: another IOKit entry "
		   "goes "
		   "through methods_ too";
	const std::string gate = unit("SwitchGpuMethodGate.h");
	for (const char *step : {"bool enter(IOLock *lock)", "void leave(IOLock *lock)"}) {
		const CodeText body = Block(gate, step);
		EXPECT_TRUE(body.Before("_disable_preemption();", "cpu_number()")) << step;
		EXPECT_TRUE(body.Before("cpu_number()", "_enable_preemption();")) << step;
		EXPECT_FALSE(body.Has("_fetch")) << step;
	}
}

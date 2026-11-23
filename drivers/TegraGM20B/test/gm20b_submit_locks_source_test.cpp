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

TEST(GM20BSubmitLocksSource, ProducerChannelsResolveUnderOneLiveLock)
{
	const std::string exec = unit("gpu_exec.c");
	const CodeText status = Block(exec, "static u32 backing_unsettled(");
	EXPECT_EQ(status.count("switch_gpu_backing_token_status(&tokens[i])"), 1u);
	EXPECT_FALSE(status.has("mutex_lock("));
	const CodeText all =
		Block(exec, "static inline __attribute__((always_inline)) int chan_exec_body(");
	EXPECT_EQ(all.count("mutex_lock(&tgm_live_lock)"), 1u);
	EXPECT_TRUE(all.before("backing_unsettled(dependencies, dependency_count, unsettled)",
			       "mutex_lock(&tgm_live_lock)"));
	EXPECT_TRUE(all.before("mutex_lock(&tgm_live_lock)",
			       "backing_waits(chan, dependencies, dependency_count, unsettled)"));
	const CodeText one = Block(exec, "static int backing_dependency(");
	EXPECT_TRUE(one.has("live_by_uid(token->queue)"));
	EXPECT_FALSE(one.has("mutex_lock("));
	EXPECT_FALSE(one.has("switch_gpu_backing_token_status("));
}

TEST(GM20BSubmitLocksSource, SubmitJobTakesNoReferencePerUse)
{
	const CodeText job = Block(unit("TegraGM20BUserClientJobSubmit.cpp"),
				   "IOReturn AGXDeviceUserClient::runJobUses(");
	EXPECT_TRUE(job.has("switch_gpu_backing_begin_held("));
	EXPECT_FALSE(job.has("switch_gpu_backing_begin_checked("));
	EXPECT_FALSE(job.has("switch_gpu_backing_begin_conversion_checked("));
}

// The hardware boundary is GP_PUT; private control words may precede publication while defer_kick holds them back.
TEST(GM20BSubmitLocksSource, AJobsRecordsGoUpBeforeItsKick)
{
	const std::string exec = unit("gpu_exec.c");
	const CodeText body =
		Block(exec, "static inline __attribute__((always_inline)) int chan_exec_body(");
	EXPECT_EQ(body.count("chan->chan.gpfifo.cur = ring_cur;"), 1u);
	EXPECT_TRUE(body.before("chan->chan.defer_kick = true;",
				"backing_waits(chan, dependencies, dependency_count, unsettled)"));
	EXPECT_TRUE(body.afterLast("backing_waits(chan, dependencies, dependency_count, unsettled)")
			    .before("mutex_unlock(&tgm_live_lock);",
				    "switch_gpu_backing_publish(owned);"));
	const CodeText published = body.afterLast("switch_gpu_backing_publish(owned);");
	EXPECT_FALSE(published.has("goto rollback;"));
	EXPECT_TRUE(published.has("*seqno = chan_fence_job("));
	const CodeText job = Block(unit("TegraGM20BUserClientJobSubmit.cpp"),
				   "IOReturn AGXDeviceUserClient::runJobUses(");
	EXPECT_TRUE(job.has("jobVisibility, releases, keptCount, &transaction);"));
	EXPECT_FALSE(job.has("switch_gpu_backing_commit("));
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
			EXPECT_FALSE(body.has(shared)) << step;
	}
	const std::string inl = unit("gpu_inline.h");
	const CodeText enter = Block(inl, "static inline enum tgm_gate_entry gate_try_enter(void)");
	const CodeText leave = Block(inl, "static inline void gate_leave(void)");
	EXPECT_TRUE(enter.before("_disable_preemption();",
				 "tgm_gate_try_enter(&tgm_sc7_gate, (unsigned int)cpu_number());"));
	EXPECT_TRUE(enter.before("tgm_gate_try_enter(", "_enable_preemption();"));
	EXPECT_TRUE(leave.before("_disable_preemption();",
				 "tgm_gate_leave(&tgm_sc7_gate, (unsigned int)cpu_number());"));
	EXPECT_TRUE(leave.before("tgm_gate_leave(", "_enable_preemption();"));
	EXPECT_TRUE(leave.before("_enable_preemption();", "gate_wake_closer();"));
	EXPECT_EQ(CodeText(inl + unit("gpu_bringup.c")).count("tgm_gate_try_enter("), 1u);
	EXPECT_TRUE(Block(unit("gpu_bringup.c"), "void tgm_gate_enter(void)")
			    .has("while ((entry = gate_try_enter()) != TGM_GATE_IN)"));
}

// IOKit's default locking took this connection's read lock around every method, SubmitJob included. The connection turns
// it off and counts its entries per CPU instead (SwitchGpuMethodGate.h); its close shuts that gate and waits for them
// before it tears anything down.
TEST(GM20BSubmitLocksSource, MethodsRunWithoutIOKitsReadLock)
{
	const std::string client = unit("TegraGM20BUserClient.cpp");
	EXPECT_TRUE(Block(client, "bool AGXDeviceUserClient::setUp(task_t task)")
			    .has("setProperty(kIOUserClientDefaultLockingKey, kOSBooleanFalse);"));
	const CodeText method = Block(client, "IOReturn AGXDeviceUserClient::externalMethod(");
	EXPECT_TRUE(method.before("if (methods_.enter(lock_))",
				  "externalCall(selector, arguments, asyncReference)"));
	EXPECT_TRUE(method.before("externalCall(selector, arguments, asyncReference)",
				  "methods_.leave(lock_);"));
	const CodeText memory = Block(client, "IOReturn AGXDeviceUserClient::clientMemoryForType(");
	EXPECT_TRUE(memory.before("if (!methods_.enter(lock_))",
				  "memoryForType(type, options, memory)"));
	EXPECT_TRUE(
		memory.before("memoryForType(type, options, memory)", "methods_.leave(lock_);"));
	const CodeText close = Block(client, "IOReturn AGXDeviceUserClient::clientClose()");
	EXPECT_TRUE(close.before("methods_.close(lock_);", "switch_gpu_compiler_close(this);"));
	EXPECT_TRUE(close.before("methods_.close(lock_);", "drainResources();"));
	EXPECT_EQ(Block(unit("TegraGM20BUserClientInternal.h"),
			"class AGXDeviceUserClient : public IOUserClient")
			  .count("APPLE_KEXT_OVERRIDE"),
		  4u)
		<< "clientClose, free, clientMemoryForType and externalMethod: another IOKit entry "
		   "goes "
		   "through methods_ too";
	const std::string gate = unit("SwitchGpuMethodGate.h");
	for (const char *step : {"bool enter(IOLock *lock)", "void leave(IOLock *lock)"}) {
		const CodeText body = Block(gate, step);
		EXPECT_TRUE(body.before("_disable_preemption();", "cpu_number()")) << step;
		EXPECT_TRUE(body.before("cpu_number()", "_enable_preemption();")) << step;
		EXPECT_FALSE(body.has("_fetch")) << step;
	}
}

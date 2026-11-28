// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "SwitchGpuCompilerScope.h"
#include "SwitchGpuShaderCompiler.h"
#include "SwitchGpuShaderCompilerQueue.h"
#include "TegraGM20BCompilerABI.h"
#include "switch_gpu_async_rights_stubs.h"

extern "C" int proc_selfpid(void)
{
	return 448;
}

// The asking process's scope as a test sets it: what the kernel reads in the caller's context
TegraGM20BAbiCompilerScope gAskingScope = {TegraGM20BCompilerScopeFile, 24, {0xc5}};

void switch_gpu_compiler_scope(TegraGM20BAbiCompilerScope &out)
{
	out = gAskingScope;
}

namespace
{
using switch_gpu_compiler::gBroker;

constexpr uint64_t kStack = 0xc7fe25c8f16ed0bfull;
host_task gClientTask = {false}, gCompilerTask = {true};
host_thread gClientThread = {"client"}, gClientThread2 = {"client2"}, gTakerThread = {"taker"},
	    gTakerThread2 = {"taker2"}, gWorker1 = {"worker1"}, gWorker2 = {"worker2"};

IOReturn syncCall(const void *owner, task_t task, uint32_t method, const void *in, uint32_t inSize,
		  void *out, uint32_t outSize)
{
	IOExternalMethodArguments arguments = {};
	SwitchGpuAsyncReference held(&arguments);
	return held.settle(switch_gpu_compiler_call(owner, task, method, in, inSize, out, outSize,
						    &arguments, held));
}

IOReturn asyncCall(const void *owner, mach_port_t port, uint32_t method, const void *in,
		   uint32_t inSize)
{
	gRights.make(port);
	io_user_reference_t reference[kOSAsyncRef64Count] = {port | kIOUCAsync64Flag, 0x1000,
							     0x2000};
	IOExternalMethodArguments arguments = {};
	arguments.asyncReference = reference;
	arguments.asyncReferenceCount = kOSAsyncRef64Count;
	arguments.asyncWakePort = port;
	SwitchGpuAsyncReference held(&arguments);
	const IOReturn result = held.settle(switch_gpu_compiler_call(
		owner, &gClientTask, method, in, inSize, nullptr, 0, &arguments, held));
	if (result != kIOReturnSuccess)
		gRights.release(port);
	return result;
}

class CompilerTurnstile : public ::testing::Test
{
protected:
	void SetUp() override
	{
		gRights.reset();
		gTurnstiles.reset();
		gCurrentThread = &gDefaultThread;
		IOLock *lock = switch_gpu_compiler::brokerLock();
		ASSERT_NE(lock, nullptr);
		ASSERT_EQ(gBroker.requests, nullptr);
		OSSafeReleaseNULL(gBroker.store);
		gBroker = {};
		gBroker.lock = lock;
	}

	void TearDown() override
	{
		for (const void *owner : {kClientA, kClientB, kCompiler, kCompiler2})
			switch_gpu_compiler_close(owner);
		OSSafeReleaseNULL(gBroker.store);
		EXPECT_EQ(gBroker.requests, nullptr);
		EXPECT_EQ(gRights.underflows, 0u);
		EXPECT_EQ(gRights.orphanSends, 0u);
		EXPECT_EQ(gRights.held(), 0);
		EXPECT_EQ(gTurnstiles.underflows, 0u);
		EXPECT_EQ(gTurnstiles.held(), 0);
	}

	void registerCompiler(const void *compiler = kCompiler, bool restarts = true)
	{
		const TegraGM20BAbiCompilerRegister in = {
			0x4000, 0x4000, kStack,
			restarts ? uint32_t(TegraGM20BCompilerRestarts) : 0u, 0};
		TegraGM20BAbiCompilerRegisterReply reply = {};
		ASSERT_EQ(syncCall(compiler, &gCompilerTask, TegraGM20BMethodCompilerRegister, &in,
				   sizeof in, &reply, sizeof reply),
			  kIOReturnSuccess);
	}

	void registerWorker(uint32_t slot, thread_t thread, const void *compiler = kCompiler)
	{
		gCurrentThread = thread;
		const TegraGM20BAbiCompilerServe in = {0, slot, 0};
		ASSERT_EQ(syncCall(compiler, &gCompilerTask, TegraGM20BMethodCompilerServe, &in,
				   sizeof in, nullptr, 0),
			  kIOReturnSuccess);
	}

	TegraGM20BAbiCompilerTakeReply takeOne(thread_t taker, const void *compiler = kCompiler)
	{
		gCurrentThread = taker;
		std::string buf(request_.size(), 0);
		const TegraGM20BAbiCompilerTake in = {uint64_t(uintptr_t(buf.data())), buf.size()};
		TegraGM20BAbiCompilerTakeReply reply = {};
		EXPECT_EQ(syncCall(compiler, &gCompilerTask, TegraGM20BMethodCompilerTake, &in,
				   sizeof in, &reply, sizeof reply),
			  kIOReturnSuccess);
		return reply;
	}

	void completeOne(uint64_t id, const void *compiler = kCompiler)
	{
		const TegraGM20BAbiCompilerComplete done = {
			id, TegraGM20BCompileMade, TegraGM20BCompileCompiled, {10, 20, 30, 47, 0}};
		ASSERT_EQ(syncCall(compiler, &gCompilerTask, TegraGM20BMethodCompilerComplete,
				   &done, sizeof done, nullptr, 0),
			  kIOReturnSuccess);
	}

	void waitUntilSleeping(uint32_t count)
	{
		std::unique_lock<std::mutex> l(gTurnstiles.lock);
		ASSERT_TRUE(gTurnstiles.cv.wait_for(l, std::chrono::seconds(2), [&] {
			uint32_t total = 0;
			for (const auto &[ut, slot] : gTurnstiles.slots)
				total += slot.waiters;
			return total >= count;
		}));
	}

	thread_t activeInheritor()
	{
		std::lock_guard<std::mutex> l(gTurnstiles.lock);
		for (const auto &[ut, slot] : gTurnstiles.slots)
			if (slot.active)
				return slot.inheritor;
		return THREAD_NULL;
	}

	static inline const void *const kClientA = reinterpret_cast<const void *>(0xa0);
	static inline const void *const kClientB = reinterpret_cast<const void *>(0xb0);
	static inline const void *const kCompiler = reinterpret_cast<const void *>(0xc0);
	static inline const void *const kCompiler2 = reinterpret_cast<const void *>(0xc1);
	static constexpr mach_port_t kPortA = 0x1230, kPortB = 0x4560;
	std::string request_ = std::string(128, 's');
};

TEST_F(CompilerTurnstile, SyncCompileTransitionsFromNullToTakerToWorkerAndClears)
{
	registerCompiler();
	registerWorker(1, &gWorker1);

	TegraGM20BAbiCompileReply reply = {};
	IOReturn rc = kIOReturnError;
	std::thread client([&] {
		gCurrentThread = &gClientThread;
		const TegraGM20BAbiCompile in = {uint64_t(uintptr_t(request_.data())),
						 request_.size(),
						 0,
						 kStack,
						 0,
						 TegraGM20BCompileWaited};
		rc = syncCall(kClientA, &gClientTask, TegraGM20BMethodCompile, &in, sizeof in,
			      &reply, sizeof reply);
	});

	waitUntilSleeping(1);
	EXPECT_EQ(activeInheritor(), THREAD_NULL);

	const auto taken = takeOne(&gTakerThread);
	ASSERT_NE(taken.id, 0u);
	EXPECT_EQ(activeInheritor(), &gTakerThread);

	gCurrentThread = &gWorker1;
	const TegraGM20BAbiCompilerServe serve = {taken.id, 1, 0};
	ASSERT_EQ(syncCall(kCompiler, &gCompilerTask, TegraGM20BMethodCompilerServe, &serve,
			   sizeof serve, nullptr, 0),
		  kIOReturnSuccess);
	EXPECT_EQ(activeInheritor(), &gWorker1);

	completeOne(taken.id);
	client.join();
	EXPECT_EQ(rc, kIOReturnSuccess);
	EXPECT_EQ(reply.result, TegraGM20BCompileMade);
	EXPECT_EQ(reply.times.answer_priority, 47u);
	EXPECT_EQ(activeInheritor(), THREAD_NULL);
}

TEST_F(CompilerTurnstile, AsyncCompileWaitInheritsJoinedWorkerFromTakerServe)
{
	registerCompiler();
	registerWorker(2, &gWorker2);

	const TegraGM20BAbiCompile first = {
		uint64_t(uintptr_t(request_.data())), request_.size(), 101, kStack, 0, 0};
	const TegraGM20BAbiCompile joined = {uint64_t(uintptr_t(request_.data())),
					     request_.size(),
					     202,
					     kStack,
					     0,
					     TegraGM20BCompileWaited};
	ASSERT_EQ(asyncCall(kClientA, kPortA, TegraGM20BMethodCompile, &first, sizeof first),
		  kIOReturnSuccess);
	const auto taken1 = takeOne(&gTakerThread);
	ASSERT_NE(taken1.id, 0u);

	// Worker 2 claims the first request.
	gCurrentThread = &gWorker2;
	const TegraGM20BAbiCompilerServe claim1 = {taken1.id, 2, 0};
	ASSERT_EQ(syncCall(kCompiler, &gCompilerTask, TegraGM20BMethodCompilerServe, &claim1,
			   sizeof claim1, nullptr, 0),
		  kIOReturnSuccess);

	// Second async request arrives and is taken; waiter sleeps in CompileWait before Serve.
	ASSERT_EQ(asyncCall(kClientB, kPortB, TegraGM20BMethodCompile, &joined, sizeof joined),
		  kIOReturnSuccess);
	const auto taken2 = takeOne(&gTakerThread);
	ASSERT_NE(taken2.id, 0u);

	IOReturn waitRc1 = kIOReturnError, waitRc2 = kIOReturnError;
	std::thread waiter1([&] {
		gCurrentThread = &gClientThread;
		const TegraGM20BAbiCompileWait w = {202, 0, 0};
		waitRc1 = syncCall(kClientB, &gClientTask, TegraGM20BMethodCompileWait, &w,
				   sizeof w, nullptr, 0);
	});
	std::thread waiter2([&] {
		gCurrentThread = &gClientThread2;
		const TegraGM20BAbiCompileWait w = {202, 0, 0};
		waitRc2 = syncCall(kClientB, &gClientTask, TegraGM20BMethodCompileWait, &w,
				   sizeof w, nullptr, 0);
	});
	waitUntilSleeping(2);
	EXPECT_EQ(activeInheritor(), &gTakerThread);

	// Taker thread binds the joined request directly to worker slot 2 without interrupting worker 2.
	gCurrentThread = &gTakerThread;
	const TegraGM20BAbiCompilerServe claim2 = {taken2.id, 2, 0};
	ASSERT_EQ(syncCall(kCompiler, &gCompilerTask, TegraGM20BMethodCompilerServe, &claim2,
			   sizeof claim2, nullptr, 0),
		  kIOReturnSuccess);
	EXPECT_EQ(activeInheritor(), &gWorker2);

	completeOne(taken1.id);
	completeOne(taken2.id);
	waiter1.join();
	waiter2.join();
	EXPECT_EQ(waitRc1, kIOReturnSuccess);
	EXPECT_EQ(waitRc2, kIOReturnSuccess);
	EXPECT_EQ(gRights.replies.size(), 2u);
	EXPECT_EQ(gRights.held(), 0);
}

TEST_F(CompilerTurnstile, CompilerCloseDropsDeadInheritorsAndRequeuesForNextCompiler)
{
	registerCompiler(kCompiler, true);
	registerWorker(1, &gWorker1, kCompiler);

	TegraGM20BAbiCompileReply reply = {};
	IOReturn rc = kIOReturnError;
	std::thread client([&] {
		gCurrentThread = &gClientThread;
		const TegraGM20BAbiCompile in = {uint64_t(uintptr_t(request_.data())),
						 request_.size(),
						 0,
						 kStack,
						 0,
						 TegraGM20BCompileWaited};
		rc = syncCall(kClientA, &gClientTask, TegraGM20BMethodCompile, &in, sizeof in,
			      &reply, sizeof reply);
	});
	waitUntilSleeping(1);

	const auto taken1 = takeOne(&gTakerThread, kCompiler);
	gCurrentThread = &gWorker1;
	const TegraGM20BAbiCompilerServe claim1 = {taken1.id, 1, 0};
	ASSERT_EQ(syncCall(kCompiler, &gCompilerTask, TegraGM20BMethodCompilerServe, &claim1,
			   sizeof claim1, nullptr, 0),
		  kIOReturnSuccess);
	EXPECT_EQ(activeInheritor(), &gWorker1);

	// First compiler dies: dead taker and worker refs must drop to 0, inheritor cleared.
	switch_gpu_compiler_close(kCompiler);
	EXPECT_EQ(activeInheritor(), THREAD_NULL);
	EXPECT_EQ(gTurnstiles.refCount(&gTakerThread), 0);
	EXPECT_EQ(gTurnstiles.refCount(&gWorker1), 0);

	// Replacement compiler registers, takes the requeued request, serves on worker 2, completes.
	registerCompiler(kCompiler2, true);
	registerWorker(1, &gWorker2, kCompiler2);
	const auto taken2 = takeOne(&gTakerThread2, kCompiler2);
	EXPECT_EQ(taken2.id, taken1.id);
	EXPECT_EQ(activeInheritor(), &gTakerThread2);

	gCurrentThread = &gWorker2;
	const TegraGM20BAbiCompilerServe claim2 = {taken2.id, 1, 0};
	ASSERT_EQ(syncCall(kCompiler2, &gCompilerTask, TegraGM20BMethodCompilerServe, &claim2,
			   sizeof claim2, nullptr, 0),
		  kIOReturnSuccess);
	EXPECT_EQ(activeInheritor(), &gWorker2);

	completeOne(taken2.id, kCompiler2);
	client.join();
	EXPECT_EQ(rc, kIOReturnSuccess);
	EXPECT_EQ(reply.result, TegraGM20BCompileMade);
}

TEST_F(CompilerTurnstile, ClientCloseWhileCompileWaitSleepsWakesCleanlyWithoutUaf)
{
	registerCompiler();
	const TegraGM20BAbiCompile in = {uint64_t(uintptr_t(request_.data())),
					 request_.size(),
					 77,
					 kStack,
					 0,
					 TegraGM20BCompileWaited};
	ASSERT_EQ(asyncCall(kClientA, kPortA, TegraGM20BMethodCompile, &in, sizeof in),
		  kIOReturnSuccess);

	IOReturn waitRc = kIOReturnError;
	std::thread waiter([&] {
		gCurrentThread = &gClientThread;
		const TegraGM20BAbiCompileWait w = {77, 0, 0};
		waitRc = syncCall(kClientA, &gClientTask, TegraGM20BMethodCompileWait, &w, sizeof w,
				  nullptr, 0);
	});
	waitUntilSleeping(1);

	switch_gpu_compiler_close(kClientA);
	waiter.join();
	EXPECT_EQ(waitRc, kIOReturnSuccess);
	EXPECT_EQ(gRights.held(), 0);
	EXPECT_TRUE(gRights.replies.empty());
}

TEST_F(CompilerTurnstile, ServeAndCompileWaitValidateArgumentsAndOwnership)
{
	registerCompiler();
	const TegraGM20BAbiCompilerServe badZero = {0, 0, 0};
	const TegraGM20BAbiCompilerServe badSlot = {0, TegraGM20BCompilerMaxWorkers + 1, 0};
	const TegraGM20BAbiCompilerServe badFlags = {0, 1, 1};
	const TegraGM20BAbiCompilerServe unregistered = {1, 1, 0};
	EXPECT_EQ(syncCall(kCompiler, &gCompilerTask, TegraGM20BMethodCompilerServe, &badZero,
			   sizeof badZero, nullptr, 0),
		  kIOReturnBadArgument);
	EXPECT_EQ(syncCall(kCompiler, &gCompilerTask, TegraGM20BMethodCompilerServe, &badSlot,
			   sizeof badSlot, nullptr, 0),
		  kIOReturnBadArgument);
	EXPECT_EQ(syncCall(kCompiler, &gCompilerTask, TegraGM20BMethodCompilerServe, &badFlags,
			   sizeof badFlags, nullptr, 0),
		  kIOReturnBadArgument);
	EXPECT_EQ(syncCall(kClientA, &gClientTask, TegraGM20BMethodCompilerServe, &unregistered,
			   sizeof unregistered, nullptr, 0),
		  kIOReturnNotPermitted);
	EXPECT_EQ(syncCall(kCompiler, &gCompilerTask, TegraGM20BMethodCompilerServe, &unregistered,
			   sizeof unregistered, nullptr, 0),
		  kIOReturnNotReady);

	const TegraGM20BAbiCompileWait badCookie = {0, 0, 0};
	const TegraGM20BAbiCompileWait badWaitFlags = {1, 1, 0};
	const TegraGM20BAbiCompileWait missing = {999, 0, 0};
	EXPECT_EQ(syncCall(kClientA, &gClientTask, TegraGM20BMethodCompileWait, &badCookie,
			   sizeof badCookie, nullptr, 0),
		  kIOReturnBadArgument);
	EXPECT_EQ(syncCall(kClientA, &gClientTask, TegraGM20BMethodCompileWait, &badWaitFlags,
			   sizeof badWaitFlags, nullptr, 0),
		  kIOReturnBadArgument);
	EXPECT_EQ(syncCall(kClientA, &gClientTask, TegraGM20BMethodCompileWait, &missing,
			   sizeof missing, nullptr, 0),
		  kIOReturnNotFound);

	// CompileWait isolates cookies by owner connection.
	const TegraGM20BAbiCompile in = {
		uint64_t(uintptr_t(request_.data())), request_.size(), 42, kStack, 0, 0};
	ASSERT_EQ(asyncCall(kClientA, kPortA, TegraGM20BMethodCompile, &in, sizeof in),
		  kIOReturnSuccess);
	const TegraGM20BAbiCompileWait wrongOwner = {42, 0, 0};
	EXPECT_EQ(syncCall(kClientB, &gClientTask, TegraGM20BMethodCompileWait, &wrongOwner,
			   sizeof wrongOwner, nullptr, 0),
		  kIOReturnNotFound);
	EXPECT_EQ(asyncCall(kClientA, kPortA, TegraGM20BMethodCompileWait, &wrongOwner,
			    sizeof wrongOwner),
		  kIOReturnBadArgument);
}

}  // namespace

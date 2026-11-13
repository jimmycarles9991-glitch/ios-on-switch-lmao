// SPDX-License-Identifier: MIT
// The shader compiler broker's asynchronous Compile through the MIG server's rules for the wake port's send right: the
// call brings one right, an error return has MIG release it, a success leaves it to the broker, which releases it once
// the answer is sent or its asker closed. Boot 11 of T-v3 (iOS 27): a compiler jetsam-killed for the third time leaves
// `declined` set until the next registers, and an asynchronous Compile in that window returned kIOReturnNotReady after
// the user client had released the right too: "send right count overflow (delta: -1)".
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "SwitchGpuShaderCompiler.h"
#include "SwitchGpuShaderCompilerQueue.h"
#include "TegraGM20BCompilerABI.h"
#include "switch_gpu_async_rights_stubs.h"

extern "C" int proc_selfpid(void)
{
	return 448;
}

namespace
{
using switch_gpu_compiler::gBroker;

constexpr uint64_t kStack = 0xc7fe25c8f16ed0bfull;
host_task gClientTask = {false}, gCompilerTask = {true};

// io_connect_async_method: the request's wake port right, the user client's externalMethod (settle() its result),
// and MIG's release of the right when that result is an error
IOReturn asyncCall(const void *owner, mach_port_t port, uint32_t method, const void *in,
		   uint32_t inSize, uint32_t outSize = 0,
		   uint32_t referenceCount = kOSAsyncRef64Count)
{
	gRights.make(port);
	io_user_reference_t reference[kOSAsyncRef64Count] = {port | kIOUCAsync64Flag, 0x1000,
							     0x2000};
	IOExternalMethodArguments arguments = {};
	arguments.asyncReference = reference;
	arguments.asyncReferenceCount = referenceCount;
	arguments.asyncWakePort = port;
	SwitchGpuAsyncReference held(&arguments);
	uint8_t out[64] = {};
	const IOReturn result = held.settle(
		switch_gpu_compiler_call(owner, &gClientTask, method, in, inSize,
					 outSize ? out : nullptr, outSize, &arguments, held));
	if (result != kIOReturnSuccess)
		gRights.release(port);
	return result;
}

IOReturn syncCall(const void *owner, task_t task, uint32_t method, const void *in, uint32_t inSize,
		  void *out, uint32_t outSize)
{
	IOExternalMethodArguments arguments = {};
	SwitchGpuAsyncReference held(&arguments);
	return held.settle(switch_gpu_compiler_call(owner, task, method, in, inSize, out, outSize,
						    &arguments, held));
}

class CompilerRights : public ::testing::Test
{
protected:
	void SetUp() override
	{
		gRights.reset();
		IOLock *lock = switch_gpu_compiler::brokerLock();
		ASSERT_NE(lock, nullptr);
		ASSERT_EQ(gBroker.requests, nullptr);
		OSSafeReleaseNULL(gBroker.store);
		gBroker = {};
		gBroker.lock = lock;
	}

	void TearDown() override
	{
		for (const void *owner : {kClientA, kClientB, kCompiler})
			switch_gpu_compiler_close(owner);
		OSSafeReleaseNULL(gBroker.store);
		EXPECT_EQ(gBroker.requests, nullptr);
		EXPECT_EQ(gRights.underflows, 0u) << "a send right was released twice";
		EXPECT_EQ(gRights.orphanSends, 0u)
			<< "an answer went to a port the kernel held no right to";
		EXPECT_EQ(gRights.held(), 0) << "a send right was never released";
	}

	void registerCompiler(bool restarts = true)
	{
		const TegraGM20BAbiCompilerRegister in = {
			0x4000, 0x4000, kStack,
			restarts ? uint32_t(TegraGM20BCompilerRestarts) : 0u, 0};
		TegraGM20BAbiCompilerRegisterReply reply = {};
		ASSERT_EQ(syncCall(kCompiler, &gCompilerTask, TegraGM20BMethodCompilerRegister, &in,
				   sizeof in, &reply, sizeof reply),
			  kIOReturnSuccess);
	}

	// A crash loop: a compiler lost a third time in a row before any registers, so every later close declines until one
	// registers
	void loseThreeCompilers()
	{
		const TegraGM20BAbiCompilerAnnounce in = {TegraGM20BCompilerRestarts, 0};
		for (int lost = 0; lost < 3; ++lost) {
			ASSERT_EQ(syncCall(kCompiler, &gCompilerTask,
					   TegraGM20BMethodCompilerAnnounce, &in, sizeof in,
					   nullptr, 0),
				  kIOReturnSuccess);
			switch_gpu_compiler_close(kCompiler);
		}
		ASSERT_TRUE(gBroker.declined);
	}

	IOReturn compile(const void *owner, mach_port_t port, uint64_t cookie, uint32_t outSize = 0,
			 uint32_t referenceCount = kOSAsyncRef64Count)
	{
		const TegraGM20BAbiCompile in = {uint64_t(uintptr_t(request_.data())),
						 request_.size(),
						 cookie,
						 kStack,
						 0,
						 TegraGM20BCompileWaited};
		return asyncCall(owner, port, TegraGM20BMethodCompile, &in, sizeof in, outSize,
				 referenceCount);
	}

	static inline const void *const kClientA = reinterpret_cast<const void *>(0xa0);
	static inline const void *const kClientB = reinterpret_cast<const void *>(0xb0);
	static inline const void *const kCompiler = reinterpret_cast<const void *>(0xc0);
	static constexpr mach_port_t kPortA = 0x1230, kPortB = 0x4560;
	std::string request_ = std::string(256, 'r');
};

TEST_F(CompilerRights, ACompileInTheWindowAfterADeclinedCloseLeavesTheRightToMig)
{
	loseThreeCompilers();
	EXPECT_EQ(compile(kClientA, kPortA, 1), kIOReturnNotReady);
	EXPECT_TRUE(gRights.replies.empty());
}

TEST_F(CompilerRights, AQueuedCompileAnsweredAtTheCompilersCloseReleasesItsRightOnce)
{
	loseThreeCompilers();
	registerCompiler(false);
	ASSERT_EQ(compile(kClientA, kPortA, 7), kIOReturnSuccess);
	ASSERT_EQ(compile(kClientA, kPortA, 8), kIOReturnSuccess);
	EXPECT_EQ(gRights.rights[kPortA], 2);
	switch_gpu_compiler_close(kCompiler);
	ASSERT_EQ(gRights.replies.size(), 2u);
	EXPECT_EQ(gRights.replies[0].arguments[1] & 0xffffffffu,
		  uint64_t(TegraGM20BCompileUnavailable));
	EXPECT_EQ(gRights.rights[kPortA], 0);
	EXPECT_EQ(compile(kClientA, kPortA, 9), kIOReturnNotReady);
}

TEST_F(CompilerRights, ACompileQueuedForTheNextCompilerIsReleasedWhenItsAskerCloses)
{
	registerCompiler();
	switch_gpu_compiler_close(kCompiler);
	ASSERT_FALSE(gBroker.declined);
	ASSERT_EQ(compile(kClientA, kPortA, 1), kIOReturnSuccess);
	EXPECT_EQ(gRights.rights[kPortA], 1);
	switch_gpu_compiler_close(kClientA);
	EXPECT_EQ(gRights.rights[kPortA], 0);
	EXPECT_TRUE(gRights.replies.empty());
}

TEST_F(CompilerRights, ACompilePastTheConnectionsBoundLeavesTheRightToMig)
{
	registerCompiler();
	for (uint64_t cookie = 1; cookie <= TegraGM20BCompilePendingPerClient; ++cookie)
		ASSERT_EQ(compile(kClientB, kPortB, cookie), kIOReturnSuccess);
	EXPECT_EQ(compile(kClientB, kPortB, 99), kIOReturnNoResources);
	EXPECT_EQ(gRights.rights[kPortB], int(TegraGM20BCompilePendingPerClient));
	EXPECT_EQ(compile(kClientA, kPortA, 1), kIOReturnSuccess);
	switch_gpu_compiler_close(kClientB);
	EXPECT_EQ(gRights.rights[kPortB], 0);
	EXPECT_EQ(gRights.rights[kPortA], 1);
}

TEST_F(CompilerRights, AMalformedAsynchronousCallLeavesTheRightToMig)
{
	registerCompiler();
	EXPECT_EQ(compile(kClientA, kPortA, 1, sizeof(TegraGM20BAbiCompileReply)),
		  kIOReturnBadArgument);
	EXPECT_EQ(compile(kClientA, kPortA, 1, 0, 2), kIOReturnBadArgument);
	const TegraGM20BAbiCompilerInfoReply none = {};
	EXPECT_EQ(asyncCall(kClientA, kPortA, TegraGM20BMethodCompilerInfo, &none, 0, sizeof none),
		  kIOReturnBadArgument);
	EXPECT_EQ(asyncCall(kClientA, kPortA, 0x53ff, nullptr, 0), kIOReturnBadArgument);
}

// kern.switch_shader_compiler, which system.compiler_alive reads: the compiler's pid while it is registered, and a
// generation that counts every registration this boot
TEST_F(CompilerRights, TheBrokerLineNamesTheRegisteredCompilerAndItsGeneration)
{
	char line[96];
	switch_gpu_compiler_line(line, sizeof line);
	EXPECT_STREQ(line, "pid=0 generation=0 losses=0 declined=0");
	registerCompiler();
	switch_gpu_compiler_line(line, sizeof line);
	EXPECT_STREQ(line, "pid=448 generation=1 losses=0 declined=0");
	switch_gpu_compiler_close(kCompiler);
	switch_gpu_compiler_line(line, sizeof line);
	EXPECT_STREQ(line, "pid=0 generation=1 losses=1 declined=0");
	registerCompiler();
	switch_gpu_compiler_line(line, sizeof line);
	EXPECT_STREQ(line, "pid=448 generation=2 losses=1 declined=0");
}

TEST_F(CompilerRights, AStackOtherThanTheCompilersLeavesTheRightToMig)
{
	registerCompiler();
	const TegraGM20BAbiCompile in = {
		uint64_t(uintptr_t(request_.data())), request_.size(), 1, kStack + 1, 0, 0};
	EXPECT_EQ(asyncCall(kClientA, kPortA, TegraGM20BMethodCompile, &in, sizeof in),
		  kIOReturnNotReady);
}

}  // namespace

// SPDX-License-Identifier: MIT
// The shader compiler broker's asynchronous Compile through the MIG server's rules for the wake port's send right: the
// call brings one right, an error return has MIG release it, a success leaves it to the broker, which releases it once
// the answer is sent or its asker closed. Boot 11 of T-v3 (iOS 27): a compiler jetsam-killed for the third time leaves
// `declined` set until the next registers, and an asynchronous Compile in that window returned kIOReturnNotReady after
// the user client had released the right too: "send right count overflow (delta: -1)".
#include <gtest/gtest.h>

#include <memory>
#include <string>
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
		gTurnstiles.reset();
		gMemoryStats.reset();
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
		for (const void *owner : {kClientA, kClientB, kCompiler})
			switch_gpu_compiler_close(owner);
		OSSafeReleaseNULL(gBroker.store);
		EXPECT_EQ(gBroker.requests, nullptr);
		EXPECT_EQ(gBroker.libraries, nullptr);
		EXPECT_EQ(gBroker.libraryMaps, nullptr);
		EXPECT_EQ(gBroker.libraryBytesTotal, 0u);
		EXPECT_EQ(gMemoryStats.liveMaps, 0) << "a library mapping was leaked";
		EXPECT_EQ(gMemoryStats.liveDescriptors, 0) << "a descriptor was leaked";
		EXPECT_EQ(gRights.underflows, 0u) << "a send right was released twice";
		EXPECT_EQ(gRights.orphanSends, 0u)
			<< "an answer went to a port the kernel held no right to";
		EXPECT_EQ(gRights.held(), 0) << "a send right was never released";
		EXPECT_EQ(gTurnstiles.underflows, 0u) << "a thread reference was released twice";
		EXPECT_EQ(gTurnstiles.held(), 0) << "a thread reference was leaked";
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

// A debug compiler's account of a request's wait (TegraGM20BAbiCompileTimes) reaches the asker unread, after the result
TEST_F(CompilerRights, AnAnswerCarriesTheCompilersTimesToItsAsker)
{
	registerCompiler();
	ASSERT_EQ(compile(kClientA, kPortA, 5), kIOReturnSuccess);
	std::string buffer(request_.size(), 0);
	const TegraGM20BAbiCompilerTake take = {uint64_t(uintptr_t(buffer.data())), buffer.size()};
	TegraGM20BAbiCompilerTakeReply taken = {};
	ASSERT_EQ(syncCall(kCompiler, &gCompilerTask, TegraGM20BMethodCompilerTake, &take,
			   sizeof take, &taken, sizeof taken),
		  kIOReturnSuccess);
	ASSERT_NE(taken.id, 0u);
	const TegraGM20BAbiCompilerComplete done = {
		taken.id, TegraGM20BCompileMade, TegraGM20BCompileCompiled, {11, 22, 33, 37, 0}};
	ASSERT_EQ(syncCall(kCompiler, &gCompilerTask, TegraGM20BMethodCompilerComplete, &done,
			   sizeof done, nullptr, 0),
		  kIOReturnSuccess);
	ASSERT_EQ(gRights.replies.size(), 1u);
	const std::vector<io_user_reference_t> &words = gRights.replies[0].arguments;
	ASSERT_EQ(words.size(), size_t(TegraGM20BCompileAnswerWords));
	EXPECT_EQ(words[0], 5u);
	EXPECT_EQ(words[1] & 0xffffffffu, uint64_t(TegraGM20BCompileMade));
	EXPECT_EQ(words[2], 11u);
	EXPECT_EQ(words[3], 22u);
	EXPECT_EQ(words[4], 33u);
	EXPECT_EQ(words[5], 37u);
	EXPECT_EQ(gRights.rights[kPortA], 0);
}

// The compiler is told the scope of the process that asked, as the kernel read it when it asked (a later caller's is not
// the request's), and any caller its own through CompilerInfo
TEST_F(CompilerRights, ARequestCarriesItsAskersScope)
{
	registerCompiler();
	gAskingScope = {TegraGM20BCompilerScopeFile, 14, {}};
	memcpy(gAskingScope.identity, "volume-file-01", 14);
	ASSERT_EQ(compile(kClientA, kPortA, 5), kIOReturnSuccess);
	TegraGM20BAbiCompilerInfoReply info = {};
	ASSERT_EQ(syncCall(kClientA, &gClientTask, TegraGM20BMethodCompilerInfo, nullptr, 0, &info,
			   sizeof info),
		  kIOReturnSuccess);
	EXPECT_EQ(info.scope.kind, uint32_t(TegraGM20BCompilerScopeFile));
	EXPECT_EQ(memcmp(info.scope.identity, "volume-file-01", 14), 0);
	gAskingScope = {TegraGM20BCompilerScopeProcess, 16, {0x77}};
	std::string buffer(request_.size(), 0);
	const TegraGM20BAbiCompilerTake take = {uint64_t(uintptr_t(buffer.data())), buffer.size()};
	TegraGM20BAbiCompilerTakeReply taken = {};
	ASSERT_EQ(syncCall(kCompiler, &gCompilerTask, TegraGM20BMethodCompilerTake, &take,
			   sizeof take, &taken, sizeof taken),
		  kIOReturnSuccess);
	ASSERT_NE(taken.id, 0u);
	EXPECT_EQ(taken.scope.kind, uint32_t(TegraGM20BCompilerScopeFile));
	EXPECT_EQ(taken.scope.bytes, 14u);
	EXPECT_EQ(memcmp(taken.scope.identity, "volume-file-01", 14), 0);
	const TegraGM20BAbiCompilerComplete done = {
		taken.id, TegraGM20BCompileMade, TegraGM20BCompileCompiled, {}};
	EXPECT_EQ(syncCall(kCompiler, &gCompilerTask, TegraGM20BMethodCompilerComplete, &done,
			   sizeof done, nullptr, 0),
		  kIOReturnSuccess);
}

TEST_F(CompilerRights, LibraryRegisterMapUnmapIsCowAndCompilerOnly)
{
	registerCompiler();
	TegraGM20BAbiCompilerInfoReply info = {};
	ASSERT_EQ(syncCall(kClientA, &gClientTask, TegraGM20BMethodCompilerInfo, nullptr, 0, &info,
			   sizeof info),
		  kIOReturnSuccess);
	EXPECT_NE(info.flags & TegraGM20BCompilerLibraries, 0u);

	std::string library = "MTLB-original-library-payload";
	const std::string expected = library;
	const TegraGM20BAbiCompilerRegisterLibrary reg = {uint64_t(uintptr_t(library.data())),
							  library.size(), 0, 0};
	TegraGM20BAbiCompilerRegisterLibraryReply regReply = {};
	ASSERT_EQ(syncCall(kClientA, &gClientTask, TegraGM20BMethodCompilerRegisterLibrary, &reg,
			   sizeof reg, &regReply, sizeof regReply),
		  kIOReturnSuccess);
	ASSERT_NE(regReply.id, 0u);
	library.assign(library.size(), 'X');

	ASSERT_EQ(compile(kClientA, kPortA, 11), kIOReturnSuccess);
	std::string buffer(request_.size(), 0);
	const TegraGM20BAbiCompilerTake take = {uint64_t(uintptr_t(buffer.data())), buffer.size()};
	TegraGM20BAbiCompilerTakeReply taken = {};
	ASSERT_EQ(syncCall(kCompiler, &gCompilerTask, TegraGM20BMethodCompilerTake, &take,
			   sizeof take, &taken, sizeof taken),
		  kIOReturnSuccess);
	ASSERT_NE(taken.client, 0u);

	const TegraGM20BAbiCompilerMapLibrary mapIn = {regReply.id, taken.client, 0, 0};
	TegraGM20BAbiCompilerMapLibraryReply mapReply = {};
	EXPECT_EQ(syncCall(kClientB, &gClientTask, TegraGM20BMethodCompilerMapLibrary, &mapIn,
			   sizeof mapIn, &mapReply, sizeof mapReply),
		  kIOReturnNotPermitted);
	EXPECT_EQ(syncCall(kClientA, &gClientTask, TegraGM20BMethodCompilerMapLibrary, &mapIn,
			   sizeof mapIn, &mapReply, sizeof mapReply),
		  kIOReturnNotPermitted);

	ASSERT_EQ(syncCall(kCompiler, &gCompilerTask, TegraGM20BMethodCompilerMapLibrary, &mapIn,
			   sizeof mapIn, &mapReply, sizeof mapReply),
		  kIOReturnSuccess);
	EXPECT_EQ(mapReply.bytes, expected.size());
	EXPECT_EQ(gMemoryStats.lastMapOptions, IOOptionBits(kIOMapAnywhere | kIOMapReadOnly));
	EXPECT_EQ(gMemoryStats.lastMapTask, &gCompilerTask);
	EXPECT_EQ(std::string(reinterpret_cast<const char *>(uintptr_t(mapReply.address)),
			      mapReply.bytes),
		  expected);

	const int mapsBefore = gMemoryStats.mappingsCreated;
	TegraGM20BAbiCompilerMapLibraryReply mapAgain = {};
	ASSERT_EQ(syncCall(kCompiler, &gCompilerTask, TegraGM20BMethodCompilerMapLibrary, &mapIn,
			   sizeof mapIn, &mapAgain, sizeof mapAgain),
		  kIOReturnSuccess);
	EXPECT_EQ(mapAgain.address, mapReply.address);
	EXPECT_EQ(gMemoryStats.mappingsCreated, mapsBefore);

	const TegraGM20BAbiCompilerMapLibrary wrongClient = {regReply.id, taken.client + 99, 0, 0};
	EXPECT_EQ(syncCall(kCompiler, &gCompilerTask, TegraGM20BMethodCompilerMapLibrary,
			   &wrongClient, sizeof wrongClient, &mapAgain, sizeof mapAgain),
		  kIOReturnNotFound);

	const TegraGM20BAbiCompilerUnmapLibrary unmapIn = {regReply.id, taken.client, 0, 0};
	EXPECT_EQ(syncCall(kClientB, &gClientTask, TegraGM20BMethodCompilerUnmapLibrary, &unmapIn,
			   sizeof unmapIn, nullptr, 0),
		  kIOReturnNotPermitted);
	ASSERT_EQ(syncCall(kCompiler, &gCompilerTask, TegraGM20BMethodCompilerUnmapLibrary,
			   &unmapIn, sizeof unmapIn, nullptr, 0),
		  kIOReturnSuccess);
	EXPECT_EQ(gMemoryStats.liveMaps, 0);
	EXPECT_EQ(syncCall(kCompiler, &gCompilerTask, TegraGM20BMethodCompilerUnmapLibrary,
			   &unmapIn, sizeof unmapIn, nullptr, 0),
		  kIOReturnNotFound);

	const TegraGM20BAbiCompilerComplete done = {
		taken.id, TegraGM20BCompileMade, TegraGM20BCompileCompiled, {}};
	ASSERT_EQ(syncCall(kCompiler, &gCompilerTask, TegraGM20BMethodCompilerComplete, &done,
			   sizeof done, nullptr, 0),
		  kIOReturnSuccess);
}

TEST_F(CompilerRights, ClientCloseDropsDescriptorAndCompilerRestartRemaps)
{
	registerCompiler();
	std::string libA = "library-A-bytes";
	std::string libB = "library-B-bytes";
	const TegraGM20BAbiCompilerRegisterLibrary regA = {uint64_t(uintptr_t(libA.data())),
							   libA.size(), 0, 0};
	const TegraGM20BAbiCompilerRegisterLibrary regB = {uint64_t(uintptr_t(libB.data())),
							   libB.size(), 0, 0};
	TegraGM20BAbiCompilerRegisterLibraryReply idA = {}, idB = {};
	ASSERT_EQ(syncCall(kClientA, &gClientTask, TegraGM20BMethodCompilerRegisterLibrary, &regA,
			   sizeof regA, &idA, sizeof idA),
		  kIOReturnSuccess);
	ASSERT_EQ(syncCall(kClientB, &gClientTask, TegraGM20BMethodCompilerRegisterLibrary, &regB,
			   sizeof regB, &idB, sizeof idB),
		  kIOReturnSuccess);
	const uint64_t clientA = switch_gpu_compiler::clientId(kClientA);
	const uint64_t clientB = switch_gpu_compiler::clientId(kClientB);

	TegraGM20BAbiCompilerMapLibraryReply mappedA = {}, mappedB = {};
	const TegraGM20BAbiCompilerMapLibrary mapA = {idA.id, clientA, 0, 0};
	const TegraGM20BAbiCompilerMapLibrary mapB = {idB.id, clientB, 0, 0};
	ASSERT_EQ(syncCall(kCompiler, &gCompilerTask, TegraGM20BMethodCompilerMapLibrary, &mapA,
			   sizeof mapA, &mappedA, sizeof mappedA),
		  kIOReturnSuccess);
	ASSERT_EQ(syncCall(kCompiler, &gCompilerTask, TegraGM20BMethodCompilerMapLibrary, &mapB,
			   sizeof mapB, &mappedB, sizeof mappedB),
		  kIOReturnSuccess);
	EXPECT_EQ(gMemoryStats.liveMaps, 2);

	switch_gpu_compiler_close(kClientA);
	EXPECT_EQ(gBroker.libraryBytesTotal, libB.size());
	EXPECT_EQ(std::string(reinterpret_cast<const char *>(uintptr_t(mappedA.address)),
			      mappedA.bytes),
		  libA);
	const TegraGM20BAbiCompilerUnmapLibrary unmapA = {idA.id, clientA, 0, 0};
	ASSERT_EQ(syncCall(kCompiler, &gCompilerTask, TegraGM20BMethodCompilerUnmapLibrary, &unmapA,
			   sizeof unmapA, nullptr, 0),
		  kIOReturnSuccess);
	EXPECT_EQ(syncCall(kCompiler, &gCompilerTask, TegraGM20BMethodCompilerMapLibrary, &mapA,
			   sizeof mapA, &mappedA, sizeof mappedA),
		  kIOReturnNotFound);

	switch_gpu_compiler_close(kCompiler);
	EXPECT_EQ(gMemoryStats.liveMaps, 0);
	EXPECT_EQ(gBroker.libraryBytesTotal, libB.size());
	registerCompiler();
	TegraGM20BAbiCompilerMapLibraryReply remappedB = {};
	ASSERT_EQ(syncCall(kCompiler, &gCompilerTask, TegraGM20BMethodCompilerMapLibrary, &mapB,
			   sizeof mapB, &remappedB, sizeof remappedB),
		  kIOReturnSuccess);
	EXPECT_EQ(std::string(reinterpret_cast<const char *>(uintptr_t(remappedB.address)),
			      remappedB.bytes),
		  libB);
}

TEST_F(CompilerRights, LibraryBoundsAndMalformedCallsAreRejected)
{
	registerCompiler();
	std::string small = "abcd";
	const TegraGM20BAbiCompilerRegisterLibrary badZero = {0, 4, 0, 0};
	const TegraGM20BAbiCompilerRegisterLibrary badLen = {uint64_t(uintptr_t(small.data())), 0,
							     0, 0};
	const TegraGM20BAbiCompilerRegisterLibrary badFlags = {uint64_t(uintptr_t(small.data())), 4,
							       1, 0};
	TegraGM20BAbiCompilerRegisterLibraryReply reply = {};
	EXPECT_EQ(syncCall(kClientA, &gClientTask, TegraGM20BMethodCompilerRegisterLibrary,
			   &badZero, sizeof badZero, &reply, sizeof reply),
		  kIOReturnBadArgument);
	EXPECT_EQ(syncCall(kClientA, &gClientTask, TegraGM20BMethodCompilerRegisterLibrary, &badLen,
			   sizeof badLen, &reply, sizeof reply),
		  kIOReturnBadArgument);
	EXPECT_EQ(syncCall(kClientA, &gClientTask, TegraGM20BMethodCompilerRegisterLibrary,
			   &badFlags, sizeof badFlags, &reply, sizeof reply),
		  kIOReturnBadArgument);

	const TegraGM20BAbiCompilerRegisterLibrary ok = {uint64_t(uintptr_t(small.data())),
							 small.size(), 0, 0};
	for (uint32_t i = 0; i < TegraGM20BCompilerLibrariesPerClient; ++i)
		ASSERT_EQ(syncCall(kClientA, &gClientTask, TegraGM20BMethodCompilerRegisterLibrary,
				   &ok, sizeof ok, &reply, sizeof reply),
			  kIOReturnSuccess);
	EXPECT_EQ(syncCall(kClientA, &gClientTask, TegraGM20BMethodCompilerRegisterLibrary, &ok,
			   sizeof ok, &reply, sizeof reply),
		  kIOReturnNoResources);
	EXPECT_EQ(syncCall(kClientB, &gClientTask, TegraGM20BMethodCompilerRegisterLibrary, &ok,
			   sizeof ok, &reply, sizeof reply),
		  kIOReturnSuccess);
}

}  // namespace

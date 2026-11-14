// SPDX-License-Identifier: MIT
// BoImport wires the task's pages (and takes their zero-fill faults), walks them, makes nvkm's memory, registers and
// pins the surface and cleans it, and a cached BoAlloc takes or makes its backing, before the connection lock and with
// the submit gate open; under the lock only the BO's publication runs, so a SubmitJob of the connection never parks in
// enterSlow behind an import's wiring or an allocation's zero-fill. No host model runs the user client's methods, so
// this guards where each step is.
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::Body;
using switch_ios::test::Count;
using switch_ios::test::SourceText;

namespace
{
std::string run()
{
	return Body(SourceText("../TegraGM20BUserClientDispatch.cpp"),
		    "IOReturn AGXDeviceUserClient::run(");
}

size_t gateLock(const std::string &run)
{
	return run.rfind("IOLockLock(lock_);", run.find("submitGate_.beginMutation(lock_)"));
}
}  // namespace

TEST(GM20BImportLockSource, TheLockedPartOfAnImportOnlyInsertsIt)
{
	const std::string import = Body(SourceText("../TegraGM20BUserClientImport.cpp"),
					"IOReturn AGXDeviceUserClient::boImport(");
	ASSERT_EQ(Count(import, "IOLockLock(lock_);"), 1u);
	const std::string locked = Between(import, "IOLockLock(lock_);", "IOLockUnlock(lock_);");
	EXPECT_NE(locked.find("publishBo(bo, false)"), std::string::npos);
	size_t steps = 0;
	for (const char *step :
	     {"->prepare()", "collectPages(", "tgm_mem_new_waited(", "tgm_cpu_clean_pages(",
	      "tgm_coherency_add(", "switch_gpu_pin_memory("})
		steps += Count(locked, step);
	// base 6: every one of them under lock_ with the submit gate closed
	EXPECT_EQ(steps, 0u);
}

TEST(GM20BImportLockSource, AnImportIsPreparedBeforeTheConnectionLockAndReleasedAfterIt)
{
	const std::string dispatch = run();
	const size_t imported = dispatch.find("return boImport(");
	ASSERT_NE(imported, std::string::npos);
	EXPECT_LT(imported, gateLock(dispatch));
	const std::string import = Body(SourceText("../TegraGM20BUserClientImport.cpp"),
					"IOReturn AGXDeviceUserClient::boImport(");
	EXPECT_LT(import.find("->prepare()"), import.find("IOLockLock(lock_);"));
	EXPECT_GT(import.rfind("releaseBo(bo);"), import.rfind("IOLockUnlock(lock_);"));
}

// A cached BoAlloc's backing (a ready spare, or one made with its zero-fill and clean) is taken between the pool charge
// and the BO's publication, with lock_ let go; under it the BO only goes into the connection's tables.
TEST(GM20BImportLockSource, ACachedBoAllocMakesItsBackingBeforeTheConnectionLock)
{
	const std::string dispatch = run();
	const size_t allocated = dispatch.find("return boAlloc(");
	// base: no backing is made before lock_; the spare or the new buffer comes inside boAlloc, under it
	ASSERT_NE(allocated, std::string::npos);
	EXPECT_LT(allocated, gateLock(dispatch));
	const std::string alloc = Body(SourceText("../TegraGM20BUserClientBo.cpp"),
				       "IOReturn AGXDeviceUserClient::boAlloc(");
	ASSERT_EQ(Count(alloc, "IOLockLock(lock_);"), 2u);
	const size_t charged = alloc.find("IOLockUnlock(lock_);"),
		     settled = alloc.rfind("IOLockLock(lock_);");
	for (const char *made : {"spareTake(in->bytes)", "cleanZeros(va, backing);"}) {
		EXPECT_GT(alloc.find(made), charged) << made;
		EXPECT_LT(alloc.find(made), settled) << made;
	}
	EXPECT_GT(alloc.rfind("releaseBo(bo);"), alloc.rfind("IOLockUnlock(lock_);"));
}

// SPDX-License-Identifier: MIT
// An ownership transaction's lifetime (SwitchGpuBacking.h, begin): it keeps the caller's use array until commit or
// abort; a begin that fails leaves it empty; commit and abort of an empty one do nothing; and a debug kernel refuses a
// commit or abort whose use array no longer holds what begin left there, with one line, touching nothing.
#include "switch_gpu_backing_fixture.h"
#include <cstring>

namespace
{
constexpr uint32_t kRead = SWITCH_GPU_BACKING_READ, kWrite = SWITCH_GPU_BACKING_WRITE;
constexpr bool kDebugChecks =
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
	true;
#else
	false;
#endif

// What a GM20B job on queue 9 would wait for before writing `allocation`.
uint32_t writer_waits(switch_gpu_backing *allocation)
{
	switch_gpu_backing_use use = {allocation, kWrite, 0};
	const auto completion = token(9, 1);
	switch_gpu_backing_transaction transaction = {};
	EXPECT_EQ(switch_gpu_backing_begin(&use, 1, &completion, &transaction), 0);
	switch_gpu_backing_token waits[4] = {};
	uint32_t count = 0;
	EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, waits, 4, &count), 0);
	switch_gpu_backing_abort(&transaction);
	return count;
}
}  // namespace

TEST_F(Backing, ARefusedBeginLeavesTheTransactionEmptyAndItsAbortDoesNothing)
{
	auto *allocation = backing();
	switch_gpu_backing_use use = {allocation, kRead, 0};
	switch_gpu_backing_transaction transaction;
	std::memset(&transaction, 0xa5, sizeof(transaction));
	EXPECT_EQ(switch_gpu_backing_begin(&use, 1, nullptr, &transaction), -22);
	EXPECT_EQ(transaction.uses, nullptr);
	EXPECT_EQ(transaction.count, 0u);
	switch_gpu_backing_abort(&transaction);
	switch_gpu_backing_commit(&transaction);
	EXPECT_EQ(publish(allocation, kWrite, token(1)), 0) << "nothing left the backing locked";
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, CommitAndAbortOfACommittedOrAbortedTransactionDoNothing)
{
	auto *allocation = backing();
	switch_gpu_backing_use use = {allocation, kRead, 0};
	const auto completion = token(2);
	switch_gpu_backing_transaction transaction = {};
	ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &completion, &transaction), 0);
	switch_gpu_backing_abort(&transaction);
	switch_gpu_backing_abort(&transaction);
	switch_gpu_backing_commit(&transaction);
	EXPECT_EQ(writer_waits(allocation), 0u) << "the aborted read is not recorded";
	ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &completion, &transaction), 0);
	switch_gpu_backing_commit(&transaction);
	switch_gpu_backing_commit(&transaction);
	switch_gpu_backing_abort(&transaction);
	EXPECT_EQ(writer_waits(allocation), 1u) << "the read recorded once";
	switch_gpu_backing_release(allocation);
}

// Boot 18 (T-v4b): TegraNVENCDirect's begin gave the transaction an array in its own frame and returned; the commit
// read the backings from whatever the stack held by then. A debug kernel finds the array changed and refuses.
TEST_F(Backing, ACommitWhoseUseArrayChangedSinceBeginIsRefusedWithOneLine)
{
	if (!kDebugChecks)
		GTEST_SKIP() << "the check is built into debug kernels only";
	auto *picture = backing(), *other = backing();
	switch_gpu_backing_use uses[2] = {{picture, kRead, 0}, {other, kRead, 0}};
	const auto completion = token(3);
	switch_gpu_backing_transaction transaction = {};
	ASSERT_EQ(switch_gpu_backing_begin(uses, 2, &completion, &transaction), 0);
	const switch_gpu_backing_use kept[2] = {uses[0], uses[1]};
	std::memset(uses, 0, sizeof(uses));
	const int lines = ioLogLines();
	switch_gpu_backing_commit(&transaction);
	EXPECT_EQ(ioLogLines(), lines + 1);
	EXPECT_EQ(transaction.count, 2u)
		<< "nothing is unlocked or recorded through the changed array";
	switch_gpu_backing_abort(&transaction);
	EXPECT_EQ(ioLogLines(), lines + 2);
	std::memcpy(uses, kept, sizeof(uses));
	switch_gpu_backing_abort(&transaction);
	EXPECT_EQ(transaction.count, 0u);
	EXPECT_EQ(writer_waits(picture), 0u);
	switch_gpu_backing_release(picture);
	switch_gpu_backing_release(other);
}

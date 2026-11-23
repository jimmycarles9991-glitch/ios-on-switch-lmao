// SPDX-License-Identifier: MIT
// MapSparse's bindings (SwitchGpuClientBinds.h, SwitchGpuClientRetirement.h): bindings of no BO whose pages read 0 and
// drop writes, cut, replaced, retired and closed as any binding, over switch_gpu_client_fixture.h's page tables. Each
// test's teardown closes the connection with what it left bound.
#include "switch_gpu_client_fixture.h"

namespace
{
constexpr uint64_t kVa = 0x40000, kRange = 0x4000;

std::vector<uint8_t> bytesOf(uint8_t value, size_t size = kRange)
{
	return std::vector<uint8_t>(size, value);
}

// What a job running now reads at va; -1: a fault.
int gpuRead(uint64_t va)
{
	const uint8_t *at = gpuMemory.at(va);
	return at ? *at : -1;
}
}  // namespace

class ClientSparse : public testing::Test
{
protected:
	AGXDeviceUserClient client;
	tgm_chan chan;

	void SetUp() override
	{
		bindWord = nullptr;
		bindTarget = 0;
		onBindPublished = {};
		gpuMemory = {};
		unmapped.clear();
		onUnmap = {};
		onCallEnter = {};
		installError = 0;
		logs.clear();
		client.chans_[0] = &chan;
		chan.completed = chan.submitted;
	}

	void TearDown() override
	{
		client.drainResources();
	}

	IOReturn sparse(uint64_t va = kVa, uint64_t range = kRange)
	{
		return client.bind(va, range, nullptr, TegraGM20BBindMapSparse);
	}
};

TEST_F(ClientSparse, ASparseBindingReadsZeroAndDropsWrites)
{
	ASSERT_EQ(sparse(), 0);
	ASSERT_EQ(client.nbinds_, 1u);
	EXPECT_EQ(client.binds_[0].bo, nullptr);
	EXPECT_EQ(gpuRead(kVa), 0);
	const uint8_t written = 0x5a;
	EXPECT_TRUE(gpuMemory.store(kVa + 0x1234, &written, 1));
	EXPECT_EQ(gpuRead(kVa + 0x1234), 0);
	EXPECT_EQ(gpuRead(kVa + kRange - 1), 0);
	EXPECT_EQ(gpuRead(kVa + kRange), -1);
}

// Work queued before the Map still reads 0 there and new work waits for the bind epoch; once the old work is done the
// range reads the BO.
TEST_F(ClientSparse, AMapReplacesASparseRangeBehindQueuedWork)
{
	auto bytes = bytesOf(0xb2);
	auto *fresh = client.add(50, 0x100000, kRange, &bytes);
	ASSERT_EQ(sparse(), 0);
	chan.submitted = 5;
	chan.completed = 4;
	ASSERT_EQ(client.bind(kVa, kRange, fresh), 0);
	EXPECT_EQ(gpuRead(kVa), 0);
	EXPECT_TRUE(newWorkBlocked());
	chan.completed = chan.submitted;
	client.retireBatch();
	EXPECT_FALSE(newWorkBlocked());
	EXPECT_EQ(gpuRead(kVa), 0xb2);
	EXPECT_EQ(gpuRead(kVa + kRange - 1), 0xb2);
	EXPECT_EQ(fresh->bindings, 2u);
}

TEST_F(ClientSparse, AnUnmapLeavesASparseRangeFaulting)
{
	ASSERT_EQ(sparse(), 0);
	ASSERT_EQ(client.bind(kVa, kRange, nullptr), 0);
	EXPECT_EQ(client.nbinds_, 0u);
	EXPECT_EQ(gpuRead(kVa), -1);
	EXPECT_EQ(unmapped, (std::vector<unsigned>{9000}));
	client.retireBatch();
	EXPECT_EQ(client.retired_, nullptr);
	EXPECT_EQ(client.releases, 0u);
}

TEST_F(ClientSparse, AMapOverTheMiddleKeepsBothSidesSparse)
{
	auto bytes = bytesOf(0xc3, 0x1000);
	auto *fresh = client.add(51, 0x100000, 0x1000, &bytes);
	ASSERT_EQ(sparse(), 0);
	ASSERT_EQ(client.bind(kVa + 0x1000, 0x1000, fresh), 0);
	ASSERT_EQ(client.nbinds_, 4u);
	EXPECT_EQ(client.binds_[0].bo, nullptr);
	EXPECT_EQ(client.binds_[1].bo, fresh);
	EXPECT_EQ(client.binds_[2].bo, nullptr);
	EXPECT_EQ(client.binds_[2].va, kVa + 0x2000);
	EXPECT_EQ(gpuRead(kVa), 0);
	EXPECT_EQ(gpuRead(kVa + 0x1000), 0xc3);
	EXPECT_EQ(gpuRead(kVa + 0x2000), 0);
	EXPECT_EQ(gpuRead(kVa + kRange - 1), 0);
	EXPECT_EQ(fresh->bindings, 2u);
}

// The old pages stay for work queued before the MapSparse; its PTEs go in at the retirement that removes them.
TEST_F(ClientSparse, ASparseMapOverQueuedWorkIsInstalledAtRetirement)
{
	auto bytes = bytesOf(0xa1);
	client.add(52, kVa, kRange, &bytes);
	chan.submitted = 5;
	chan.completed = 4;
	ASSERT_EQ(sparse(), 0);
	EXPECT_EQ(gpuRead(kVa), 0xa1);
	EXPECT_TRUE(newWorkBlocked());
	chan.completed = chan.submitted;
	client.retireBatch();
	EXPECT_FALSE(newWorkBlocked());
	EXPECT_EQ(gpuRead(kVa), 0);
	EXPECT_EQ(gpuRead(kVa + kRange - 1), 0);
}

TEST_F(ClientSparse, AFaultOverASparseBindingIsLoggedWithoutABo)
{
	ASSERT_EQ(sparse(), 0);
	chan.error = 1;
	chan.fault = kVa + 0x800;
	IOLockLock(client.lock_);
	client.logFaultBindings();
	IOLockUnlock(client.lock_);
	chan.error = 0;
	ASSERT_EQ(logs.size(), 1u);
	EXPECT_NE(logs[0].find("fault at 0x40800: binding 0x40000+0x4000 (sparse, pages"),
		  std::string::npos);
}

// The same range bound sparse again is no change (VmBind closes no gate for it), whatever its handle, offset and kind;
// any other op over it is a cut.
TEST_F(ClientSparse, OnlyTheSameSparseRangeIsTheSameBinding)
{
	auto bytes = bytesOf(0xd4);
	auto *bo = client.add(53, 0x100000, kRange, &bytes);
	ASSERT_EQ(sparse(), 0);
	const AGXDeviceUserClient::Binding &held = client.binds_[0], &mapped = client.binds_[1];
	ASSERT_EQ(held.bo, nullptr);
	ASSERT_EQ(mapped.bo, bo);
	TegraGM20BAbiBindOp op = {TegraGM20BBindMapSparse, 0x42, 53, 0x1000, kVa, kRange};
	EXPECT_TRUE(AGXDeviceUserClient::bindingIs(held, op, nullptr));
	op.range = 0x2000;
	EXPECT_FALSE(AGXDeviceUserClient::bindingIs(held, op, nullptr));
	op = {TegraGM20BBindMap, 0, 53, 0, kVa, kRange};
	EXPECT_FALSE(AGXDeviceUserClient::bindingIs(held, op, bo));
	EXPECT_FALSE(AGXDeviceUserClient::bindingIs(held, op, nullptr));
	op.op = TegraGM20BBindUnmap;
	EXPECT_FALSE(AGXDeviceUserClient::bindingIs(held, op, nullptr));
	op = {TegraGM20BBindMapSparse, 0, 53, 0, 0x100000, kRange};
	EXPECT_FALSE(AGXDeviceUserClient::bindingIs(mapped, op, nullptr));
	op.op = TegraGM20BBindMap;
	EXPECT_TRUE(AGXDeviceUserClient::bindingIs(mapped, op, bo));
}

// The driver's window (sparse_window.cpp): an import's BO bound again at the window's start, the tail sparse, and
// one Unmap of the whole window, behind queued work: that work keeps both, then both retire; the import's own binding
// and the BO stay.
TEST_F(ClientSparse, OneUnmapRetiresAWindowsAliasAndItsSparseTail)
{
	auto bytes = bytesOf(0xe5, 0x2000);
	auto *import = client.add(54, 0x100000, 0x2000, &bytes);
	ASSERT_EQ(client.bind(kVa, 0x2000, import), 0);
	ASSERT_EQ(sparse(kVa + 0x2000, 0x2000), 0);
	EXPECT_EQ(gpuRead(kVa + 0x1fff), 0xe5);
	EXPECT_EQ(gpuRead(kVa + 0x2000), 0);
	EXPECT_EQ(import->bindings, 2u);
	chan.submitted = 5;
	chan.completed = 4;
	ASSERT_EQ(client.bind(kVa, kRange, nullptr), 0);
	EXPECT_EQ(client.nbinds_, 1u);
	EXPECT_EQ(gpuRead(kVa), 0xe5);
	EXPECT_EQ(gpuRead(kVa + 0x3000), 0);
	chan.completed = chan.submitted;
	client.retireBatch();
	EXPECT_EQ(gpuRead(kVa), -1);
	EXPECT_EQ(gpuRead(kVa + 0x3000), -1);
	EXPECT_EQ(gpuRead(0x100000), 0xe5);
	EXPECT_EQ(import->bindings, 1u);
	EXPECT_EQ(client.releases, 0u);
}

// A window opened over VA whose old PTEs queued work still uses: alias and tail both wait, and new work waits for the
// last bind epoch, so no job sees the alias without its tail.
TEST_F(ClientSparse, AWindowOverRetiringPtesHoldsNewWorkUntilAliasAndTailAreIn)
{
	auto old = bytesOf(0xa1), bytes = bytesOf(0xe5, 0x2000);
	client.add(55, kVa, kRange, &old);
	auto *import = client.add(56, 0x100000, 0x2000, &bytes);
	chan.submitted = 5;
	chan.completed = 4;
	ASSERT_EQ(client.bind(kVa, kRange, nullptr), 0);
	ASSERT_EQ(client.bind(kVa, 0x2000, import), 0);
	ASSERT_EQ(sparse(kVa + 0x2000, 0x2000), 0);
	EXPECT_TRUE(newWorkBlocked());
	EXPECT_EQ(bindTarget, client.bindIssued_);
	EXPECT_EQ(gpuRead(kVa + 0x3000), 0xa1);
	chan.completed = chan.submitted;
	client.retireBatch();
	EXPECT_FALSE(newWorkBlocked());
	EXPECT_EQ(gpuRead(kVa), 0xe5);
	EXPECT_EQ(gpuRead(kVa + 0x3000), 0);
}

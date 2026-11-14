// SPDX-License-Identifier: MIT
#include "switch_gpu_client_fixture.h"

class ClientRetirement : public testing::Test
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
	}

	void TearDown() override
	{
		onBindPublished = {};
		onUnmap = {};
		onCallEnter = {};
		client.onRelease = {};
		client.drainResources();
	}

	void free(unsigned id)
	{
		IOLockLock(client.lock_);
		const TegraGM20BAbiBoFree request{id};
		ASSERT_EQ(client.boFree(&request), 0);
		IOLockUnlock(client.lock_);
	}
};

TEST_F(ClientRetirement, FreeKeepsMappingAndPagesUntilExactFenceCompletes)
{
	client.add(1);
	const unsigned freed = membersFreed;
	free(1);
	EXPECT_EQ(membersFreed, freed + 1);
	EXPECT_EQ(client.nbinds_, 0u);
	EXPECT_TRUE(unmapped.empty());
	client.retireBatch();
	EXPECT_TRUE(unmapped.empty());
	EXPECT_EQ(client.releases, 0u);
	chan.completed = chan.submitted;
	AGXDeviceUserClient::retirementFence(&client);
	client.retireBatch();
	EXPECT_EQ(unmapped, (std::vector<unsigned>{1}));
	EXPECT_EQ(client.releases, 1u);
}

// A quiet job may be the last one a binding waits for: its retirement asks for that job's interrupt, so the bind epoch
// behind it is never left waiting for an interrupt nothing raises.
TEST_F(ClientRetirement, ARetiredBindingAsksForItsLastJobsInterrupt)
{
	client.add(1);
	asked.clear();
	free(1);
	EXPECT_EQ(asked, (std::vector<uint32_t>{chan.submitted}));
}

TEST_F(ClientRetirement, ChannelReplacementCannotHoldAnOldMappingForever)
{
	client.add(2);
	free(2);
	++chan.generation;
	client.retireBatch();
	EXPECT_EQ(unmapped.size(), 1u);
	EXPECT_EQ(client.releases, 1u);
}

TEST_F(ClientRetirement, QuiescedFailureReleasesTheMapping)
{
	client.add(3);
	free(3);
	chan.error = 3;
	client.retireBatch();
	EXPECT_EQ(client.releases, 1u);
}

TEST_F(ClientRetirement, MultipleBindingsKeepTheirBoUntilLastRetirement)
{
	auto *bo = client.add(4);
	auto *record = new AGXDeviceUserClient::RetiredBinding{
		nullptr, 0x20000, 4096, bo, new switch_gpu_mapping{5, 0x20000, 4096},
		{},      0,       0,    0,  false};
	client.binds_[client.nbinds_++] = {0x20000, 4096, bo, 0, record};
	++bo->bindings;
	free(4);
	chan.completed = 1;
	client.retireBatch();
	EXPECT_EQ(unmapped.size(), 2u);
	EXPECT_EQ(client.releases, 1u);
}

TEST_F(ClientRetirement, UnmapAndDestructionReleaseTheConnectionMutex)
{
	client.add(6);
	free(6);
	chan.completed = 1;
	unsigned checked = 0;
	const auto check = [&] {
		REQUIRE(client.lock_->mutex.try_lock());
		++checked;
		client.lock_->mutex.unlock();
	};
	onUnmap = [&](auto *) {
		check();
	};
	client.onRelease = check;
	client.retireBatch();
	EXPECT_EQ(checked, 2u);
}

TEST_F(ClientRetirement, NonoverlappingVaReuseLeavesOldPtesToRetirement)
{
	client.add(7);
	free(7);
	IOLockLock(client.lock_);
	client.takeRetiredOverlaps(0x20000, 4096);
	IOLockUnlock(client.lock_);
	EXPECT_EQ(client.waits, 0u);
	EXPECT_TRUE(unmapped.empty());
}

// A Map over a range whose old binding's GPU work is still pending takes its PTEs at once and never waits for the GPU
// (VmBind holds the connection's mutation gate); the old pages stay until that work completes.
TEST_F(ClientRetirement, OverlappingVaReuseTakesOldPtesWithoutWaitingForTheGpu)
{
	client.add(8);
	free(8);
	IOLockLock(client.lock_);
	client.takeRetiredOverlaps(0x10000, 4096);
	IOLockUnlock(client.lock_);
	EXPECT_EQ(client.waits, 0u);
	EXPECT_EQ(unmapped, (std::vector<unsigned>{8}));
	client.retireBatch();
	EXPECT_EQ(client.releases, 0u);
	chan.completed = chan.submitted;
	client.retireBatch();
	EXPECT_EQ(client.releases, 1u);
	EXPECT_EQ(unmapped.size(), 1u);
}

TEST_F(ClientRetirement, PhysicalUnmapRetainsVaQuarantineUntilItFinishes)
{
	client.add(9);
	free(9);
	chan.completed = 1;
	std::promise<void> started, resume;
	auto released = resume.get_future();
	onUnmap = [&](auto *) {
		started.set_value();
		released.wait();
	};
	auto worker = std::async(std::launch::async, [&] {
		client.retireBatch();
	});
	started.get_future().wait();
	auto reuse = std::async(std::launch::async, [&] {
		IOLockLock(client.lock_);
		client.takeRetiredOverlaps(0x10000, 4096);
		IOLockUnlock(client.lock_);
		return 0;
	});
	EXPECT_EQ(reuse.wait_for(10ms), std::future_status::timeout);
	resume.set_value();
	worker.get();
	EXPECT_EQ(reuse.get(), 0);
}

TEST_F(ClientRetirement, BoundedScanRotatesPendingHeadPastReadyTail)
{
	for (unsigned i = 0; i < 130; ++i) {
		client.add(10 + i, 0x10000 + i * 4096);
		free(10 + i);
	}
	unsigned at = 0;
	for (auto *record = client.retired_; record; record = record->next)
		if (++at == 130)
			record->snapshots[0].value = 0;
	client.retireBatch();
	EXPECT_EQ(client.retireRemaining_, 66u);
	EXPECT_EQ(client.releases, 0u);
	client.retireBatch();
	EXPECT_EQ(client.retireRemaining_, 2u);
	client.retireBatch();
	EXPECT_EQ(client.releases, 1u);
	EXPECT_EQ(client.retiredCount_, 129u);
}

TEST_F(ClientRetirement, CloseStopsCallbacksBeforeCancellingWorkAndReleasingMappings)
{
	client.add(150);
	free(150);
	client.drainResources();
	EXPECT_TRUE(client.call.cancelled);
	EXPECT_EQ(client.releases, 1u);
	const auto scheduled = client.call.entries.load();
	AGXDeviceUserClient::retirementFence(&client);
	EXPECT_EQ(client.call.entries.load(), scheduled);
}

TEST_F(ClientRetirement, CloseCannotPassTheWorkerReentryPublication)
{
	for (unsigned i = 0; i < 65; ++i) {
		client.add(200 + i, 0x10000 + i * 4096);
		free(200 + i);
	}
	std::promise<void> entering, resume;
	auto resumed = resume.get_future();
	onCallEnter = [&] {
		entering.set_value();
		resumed.wait();
	};
	auto worker = std::async(std::launch::async, [&] {
		client.retireBatch();
	});
	entering.get_future().wait();
	auto close = std::async(std::launch::async, [&] {
		client.drainResources();
	});
	EXPECT_EQ(close.wait_for(10ms), std::future_status::timeout);
	resume.set_value();
	worker.get();
	close.get();
	const auto entries = client.call.entries.load();
	client.retireBatch();
	EXPECT_EQ(client.call.entries.load(), entries);
	EXPECT_TRUE(client.call.cancelled);
}

TEST_F(ClientRetirement, CloseJoinsAnUnmapOutsideTheConnectionLock)
{
	client.add(31);
	free(31);
	chan.completed = chan.submitted;
	std::promise<void> entered, leave;
	auto mayLeave = leave.get_future().share();
	onUnmap = [&](auto *) {
		entered.set_value();
		mayLeave.wait();
	};
	auto worker = std::async(std::launch::async, [&] {
		{
			std::lock_guard held(client.call.completionLock);
			++client.call.running;
		}
		client.retireBatch();
		{
			std::lock_guard held(client.call.completionLock);
			--client.call.running;
		}
		client.call.completion.notify_all();
	});
	entered.get_future().wait();
	auto closing = std::async(std::launch::async, [&] {
		client.drainResources();
	});
	EXPECT_EQ(closing.wait_for(20ms), std::future_status::timeout);
	EXPECT_EQ(client.releases.load(), 0u);
	leave.set_value();
	worker.get();
	closing.get();
	EXPECT_EQ(client.releases.load(), 1u);
	EXPECT_EQ(unmapped, (std::vector<unsigned>{31}));
	EXPECT_EQ(client.unmapping_, nullptr);
	EXPECT_TRUE(client.call.cancelled);
}

TEST(ClientControlRetirement, ControlQuotaRetiresWithActualPoolOwnership)
{
	AGXDeviceUserClient client;
	AGXDeviceUserClient::Bo bo;
	bo.pool_va = reinterpret_cast<void *>(1);
	bo.flags = TegraGM20BBoControl;
	client.pool_bytes_ = bo.bytes;
	client.control_bytes_ = bo.controlCharge = 16384;
	client.accountRelease(&bo);
	EXPECT_EQ(client.pool_bytes_, 0u);
	EXPECT_EQ(client.control_bytes_, 0u);
}

// The debug "memory by process" line's counts (SwitchGpuOwnerMemory.h): a BO leaves its kind's count when it is ready to
// release, and one that never got a handle never entered it
TEST(ClientControlRetirement, AReadyBoLeavesItsOwnersCount)
{
	AGXDeviceUserClient client;
	AGXDeviceUserClient::Bo pool, import, unmade;
	pool.pool_va = reinterpret_cast<void *>(1);
	pool.ownerKind = SWITCH_GPU_OWNER_POOL;
	client.pool_bytes_ = pool.bytes;
	client.ownerMemory_.change(SWITCH_GPU_OWNER_POOL, pool.bytes, true);
	client.ownerMemory_.change(SWITCH_GPU_OWNER_IMPORTED, import.bytes, true);
	unmade.handle = 0;
	client.accountRelease(&unmade);
	EXPECT_EQ(client.ownerMemory_.total(), pool.bytes + import.bytes);
	client.accountRelease(&pool);
	client.accountRelease(&import);
	EXPECT_EQ(client.ownerMemory_.total(), 0u);
	EXPECT_EQ(client.ownerMemory_.count[SWITCH_GPU_OWNER_POOL], 0u);
	EXPECT_EQ(client.ownerMemory_.count[SWITCH_GPU_OWNER_IMPORTED], 0u);
}

namespace
{
using RetiredBinding = AGXDeviceUserClient::RetiredBinding;

RetiredBinding *spareRecord()
{
	return new RetiredBinding{};
}
}  // namespace

// Unmapping a page out of a three-page binding leaves the two others bound and mapped.
TEST_F(ClientRetirement, SubRangeUnmapKeepsTheRestOfTheBindingMapped)
{
	auto *bo = client.add(20, 0x10000, 0x3000);
	RetiredBinding *spare[2] = {spareRecord(), spareRecord()};
	IOLockLock(client.lock_);
	EXPECT_EQ(client.cutOverlaps(0x11000, 0x1000, false, false, spare), 0);
	IOLockUnlock(client.lock_);
	for (auto *each : spare)
		delete each;
	ASSERT_EQ(client.nbinds_, 2u);
	EXPECT_EQ(client.binds_[0].va, 0x10000u);
	EXPECT_EQ(client.binds_[0].range, 0x1000u);
	EXPECT_EQ(client.binds_[1].va, 0x12000u);
	EXPECT_EQ(client.binds_[1].range, 0x1000u);
	EXPECT_EQ(client.binds_[1].bo_offset, 0x2000u);
	EXPECT_EQ(bo->bindings, 3u);
	EXPECT_TRUE(unmapped.empty());
	ASSERT_NE(client.retired_, nullptr);
	EXPECT_EQ(client.retired_->va, 0x11000u);
	EXPECT_EQ(client.retired_->range, 0x1000u);
	chan.completed = chan.submitted;
	client.retireBatch();
	EXPECT_EQ(unmapped.size(), 1u);
	EXPECT_EQ(bo->bindings, 2u);
	EXPECT_EQ(client.releases, 0u);
}

// A Map over the middle of a binding takes that part's PTEs now, keeps both sides, and needs both spare records.
TEST_F(ClientRetirement, MapOverTheMiddleOfABindingTakesOnlyThatPart)
{
	client.add(21, 0x10000, 0x4000);
	RetiredBinding *spare[2] = {spareRecord(), spareRecord()};
	IOLockLock(client.lock_);
	EXPECT_EQ(client.cutOverlaps(0x11000, 0x2000, true, true, spare), 0);
	IOLockUnlock(client.lock_);
	EXPECT_EQ(spare[0], nullptr);
	EXPECT_EQ(spare[1], nullptr);
	EXPECT_EQ(client.nbinds_, 2u);
	EXPECT_EQ(unmapped.size(), 1u);
	EXPECT_EQ(client.waits, 0u);
}

TEST_F(ClientRetirement, FailedCutChangesNothing)
{
	client.add(22, 0x10000, 0x3000);
	RetiredBinding *spare[2] = {spareRecord(), spareRecord()};
	splitError = -22;
	IOLockLock(client.lock_);
	EXPECT_EQ(client.cutOverlaps(0x11000, 0x1000, true, true, spare), -22);
	IOLockUnlock(client.lock_);
	splitError = 0;
	for (auto *each : spare)
		delete each;
	ASSERT_EQ(client.nbinds_, 1u);
	EXPECT_EQ(client.binds_[0].range, 0x3000u);
	EXPECT_EQ(client.retired_, nullptr);
}

TEST_F(ClientRetirement, KilledChannelLogsTheBindingOverItsFaultOnce)
{
	client.add(23, 0x10000, 0x3000);
	chan.error = 1;
	chan.fault = 0x11800;
	logs.clear();
	IOLockLock(client.lock_);
	client.logFaultBindings();
	client.logFaultBindings();
	IOLockUnlock(client.lock_);
	ASSERT_EQ(logs.size(), 1u);
	EXPECT_NE(logs[0].find("fault at 0x11800: binding 0x10000+0x3000 (handle 23, pages "
			       "0x10000+0x3000/12)"),
		  std::string::npos);
	chan.error = 0;
}

namespace
{
std::vector<uint8_t> bytesOf(uint8_t value, size_t size = 0x4000)
{
	return std::vector<uint8_t>(size, value);
}

// A job that runs now through the page table: what it reads at va, 0 for a fault.
uint8_t gpuRead(uint64_t va)
{
	const uint8_t *at = gpuMemory.at(va);
	return at ? *at : 0;
}

uint32_t hostFetch(uint64_t va)
{
	std::vector<uint32_t> words;
	return gpuMemory.fetch(va, 1, words) ? words[0] : 0;
}
}  // namespace

// JobVisibility's control BOs (the push and upload rings, the control tables) are bound volatile: the host fetches
// what the CPU wrote there since, with no L2 invalidate between jobs. A cacheable binding keeps its first fetch's line
// until one, which a host-written job pays at its start.
TEST_F(ClientRetirement, AVolatileMapIsFetchedPastTheL2)
{
	auto ring = bytesOf(0x11), table = bytesOf(0x22);
	auto *rings = client.add(42, 0x200000, 0x4000, &ring);
	auto *tables = client.add(43, 0x300000, 0x4000, &table);
	chan.completed = chan.submitted;
	ASSERT_EQ(client.bind(0x40000, 0x4000, rings, TegraGM20BBindMapVolatile), 0);
	ASSERT_EQ(client.bind(0x80000, 0x4000, tables), 0);
	EXPECT_EQ(hostFetch(0x40000), 0x11111111u);
	EXPECT_EQ(hostFetch(0x80000), 0x22222222u);
	std::fill_n(ring.begin(), 4, 0x33);
	std::fill_n(table.begin(), 4, 0x44);
	EXPECT_EQ(hostFetch(0x40000), 0x33333333u);
	EXPECT_EQ(hostFetch(0x80000), 0x22222222u);
	gpuMemory.invalidateL2();
	EXPECT_EQ(hostFetch(0x80000), 0x44444444u);
}

// A Map over a range work queued earlier still uses: that work keeps the old pages even when it runs after the Map, work
// submitted after it waits on the GPU for the bind epoch, and retirement switches the PTEs only once the old work is
// done, so no job ever meets the range unmapped.
TEST_F(ClientRetirement, MapOverQueuedWorkSwitchesPtesBehindItAndGatesNewWork)
{
	auto oldBytes = bytesOf(0xa1), newBytes = bytesOf(0xb2);
	client.add(30, 0x40000, 0x4000, &oldBytes);
	auto *fresh = client.add(31, 0x80000, 0x4000, &newBytes);
	chan.submitted = 5;
	chan.completed = 4;
	ASSERT_EQ(client.bind(0x40000, 0x4000, fresh), 0);
	EXPECT_EQ(gpuRead(0x40000), 0xa1);
	EXPECT_EQ(gpuRead(0x43fff), 0xa1);
	EXPECT_TRUE(newWorkBlocked());
	onUnmap = [&](auto *) {
		EXPECT_EQ(chan.completed, chan.submitted);
		EXPECT_TRUE(newWorkBlocked());
	};
	client.retireBatch();
	EXPECT_EQ(gpuRead(0x40000), 0xa1);
	chan.completed = chan.submitted;
	client.retireBatch();
	EXPECT_FALSE(newWorkBlocked());
	EXPECT_EQ(gpuRead(0x40000), 0xb2);
	EXPECT_EQ(gpuRead(0x43fff), 0xb2);
	onUnmap = {};
}

TEST_F(ClientRetirement, MapWithNothingQueuedIsImmediate)
{
	auto oldBytes = bytesOf(0xa1), newBytes = bytesOf(0xb2);
	client.add(32, 0x40000, 0x4000, &oldBytes);
	auto *fresh = client.add(33, 0x80000, 0x4000, &newBytes);
	chan.completed = chan.submitted;
	ASSERT_EQ(client.bind(0x40000, 0x4000, fresh), 0);
	EXPECT_EQ(gpuRead(0x40000), 0xb2);
	EXPECT_EQ(client.bindIssued_, 0u);
}

TEST_F(ClientRetirement, ASecondMapOverAPendingMapEndsWithTheLast)
{
	auto a = bytesOf(0xa1), b = bytesOf(0xb2), c = bytesOf(0xc3);
	client.add(34, 0x40000, 0x4000, &a);
	auto *second = client.add(35, 0x80000, 0x4000, &b);
	auto *third = client.add(36, 0xc0000, 0x4000, &c);
	chan.completed = 0;
	ASSERT_EQ(client.bind(0x40000, 0x4000, second), 0);
	ASSERT_EQ(client.bind(0x40000, 0x4000, third), 0);
	EXPECT_EQ(gpuRead(0x40000), 0xa1);
	chan.completed = chan.submitted;
	client.retireBatch();
	EXPECT_FALSE(newWorkBlocked());
	EXPECT_EQ(gpuRead(0x40000), 0xc3);
}

TEST_F(ClientRetirement, UnmapOrBoFreeOfAPendingMapInstallsNothing)
{
	auto a = bytesOf(0xa1), b = bytesOf(0xb2);
	client.add(37, 0x40000, 0x4000, &a);
	auto *second = client.add(38, 0x80000, 0x4000, &b);
	chan.completed = 0;
	ASSERT_EQ(client.bind(0x40000, 0x4000, second), 0);
	ASSERT_EQ(client.bind(0x40000, 0x4000, nullptr), 0);
	chan.completed = chan.submitted;
	client.retireBatch();
	EXPECT_FALSE(newWorkBlocked());
	EXPECT_EQ(gpuRead(0x40000), 0);
	chan.completed = 0;
	auto c = bytesOf(0xc3);
	auto *third = client.add(39, 0xc0000, 0x4000, &c);
	ASSERT_EQ(client.bind(0x40000, 0x4000, third), 0);
	IOLockLock(client.lock_);
	const TegraGM20BAbiBoFree request{39};
	ASSERT_EQ(client.boFree(&request), 0);
	IOLockUnlock(client.lock_);
	chan.completed = chan.submitted;
	client.retireBatch();
	client.retireBatch();
	EXPECT_FALSE(newWorkBlocked());
	EXPECT_EQ(gpuRead(0x40000), 0);
}

TEST_F(ClientRetirement, AKilledChannelReleasesThePendingBind)
{
	auto a = bytesOf(0xa1), b = bytesOf(0xb2);
	client.add(40, 0x40000, 0x4000, &a);
	auto *second = client.add(41, 0x80000, 0x4000, &b);
	chan.completed = 0;
	ASSERT_EQ(client.bind(0x40000, 0x4000, second), 0);
	EXPECT_TRUE(newWorkBlocked());
	chan.error = 1;
	client.retireBatch();
	EXPECT_FALSE(newWorkBlocked());
	EXPECT_EQ(gpuRead(0x40000), 0xb2);
	chan.error = 0;
}

// A 128 KiB page cut by an Unmap's edge: until the work queued before the cut is done, that work still reaches both the
// cut pages and the ones that stay; then the cut pages go and the rest are mapped again with 4 KiB pages, while new
// work waits for the bind epoch.
TEST_F(ClientRetirement, ACutBigPageKeepsItsPtesUntilRetirementThenItsRestIsMappedAgain)
{
	auto big = bytesOf(0x5a, 0x20000);
	client.add(42, 0x200000, 0x20000, &big, true);
	chan.completed = 0;
	ASSERT_EQ(client.bind(0x208000, 0x1000, nullptr), 0);
	EXPECT_EQ(gpuRead(0x200000), 0x5a);
	EXPECT_EQ(gpuRead(0x208000), 0x5a);
	EXPECT_EQ(gpuRead(0x21f000), 0x5a);
	EXPECT_TRUE(newWorkBlocked());
	chan.completed = chan.submitted;
	client.retireBatch();
	EXPECT_FALSE(newWorkBlocked());
	EXPECT_EQ(gpuRead(0x200000), 0x5a);
	EXPECT_EQ(gpuRead(0x208000), 0);
	EXPECT_EQ(gpuRead(0x209000), 0x5a);
	EXPECT_EQ(gpuRead(0x21ffff), 0x5a);
}

// NVK maps new BOs all the time with work queued: a range nothing held maps at once and holds no submission back.
TEST_F(ClientRetirement, AMapOverNothingHeldIsImmediateEvenWithWorkQueued)
{
	auto bytes = bytesOf(0xb2);
	auto *fresh = client.add(43, 0x80000, 0x4000, &bytes);
	chan.completed = 0;
	ASSERT_EQ(client.bind(0x300000, 0x4000, fresh), 0);
	EXPECT_EQ(gpuRead(0x300000), 0xb2);
	EXPECT_EQ(client.bindIssued_, 0u);
	EXPECT_FALSE(newWorkBlocked());
}

// x1-glassmem audit: a Map over a VA queued work still holds is installed at a later retirement; if that install fails
// (no page tables, no comptag lines), the work waiting for the bind must not be let go onto a VA with no PTEs.
TEST_F(ClientRetirement, ABindThatCouldNotBeMappedHoldsItsWorkUntilItIs)
{
	auto oldBytes = bytesOf(0xc1), newBytes = bytesOf(0xc2);
	client.add(60, 0x40000, 0x4000, &oldBytes);
	auto *next = client.add(61, 0x100000, 0x4000, &newBytes);
	chan.completed = 4;
	chan.submitted = 5;
	free(60);
	ASSERT_EQ(client.bind(0x40000, 0x4000, next), 0);
	ASSERT_TRUE(newWorkBlocked());
	const uint32_t waitedFor = bindTarget;
	installError = -12;
	chan.completed = 5;
	client.retireBatch();
	EXPECT_LT(int32_t(client.fencePage[AGXDeviceUserClient::kBindEpochWord] - waitedFor), 0)
		<< "the epoch stays short of the unmapped bind";
	EXPECT_TRUE(newWorkBlocked());
	ASSERT_EQ(logs.size(), 1u);
	EXPECT_NE(logs[0].find("could not be mapped yet"), std::string::npos);
	client.retireBatch();
	EXPECT_EQ(logs.size(), 1u) << "logged once";
	installError = 0;
	client.retireBatch();
	EXPECT_GE(int32_t(client.fencePage[AGXDeviceUserClient::kBindEpochWord] - waitedFor), 0);
	EXPECT_EQ(gpuRead(0x40000), 0xc2);
	EXPECT_FALSE(newWorkBlocked());
}

// sliceV boot 2, HW Test ui.glass_motion: backboardd frees a snapshot surface's BO and imports the next one at the same
// VA (first fit) while its work is queued, submits, then does the same for the next surface. The submission waits on the
// GPU for the first surface's bind; the second bind waits for that submission to complete. The first bind's epoch must
// not wait for the second, or backboardd's channel stops at that acquire until the hang rule kills it.
TEST_F(ClientRetirement, ASubmissionWaitsOnlyForBindsIssuedBeforeIt)
{
	auto firstOld = bytesOf(0xa1), firstNew = bytesOf(0xa2), secondOld = bytesOf(0xb1),
	     secondNew = bytesOf(0xb2);
	client.add(50, 0x40000, 0x4000, &firstOld);
	client.add(51, 0x80000, 0x4000, &secondOld);
	auto *firstNext = client.add(52, 0x100000, 0x4000, &firstNew);
	auto *secondNext = client.add(53, 0x140000, 0x4000, &secondNew);
	chan.completed = 4;
	chan.submitted = 5;
	free(50);
	ASSERT_EQ(client.bind(0x40000, 0x4000, firstNext), 0);
	ASSERT_TRUE(newWorkBlocked());
	const uint32_t waitedFor = bindTarget;
	chan.submitted = 6;
	free(51);
	ASSERT_EQ(client.bind(0x80000, 0x4000, secondNext), 0);
	chan.completed = 5;
	client.retireBatch();
	EXPECT_GE(int32_t(client.fencePage[AGXDeviceUserClient::kBindEpochWord] - waitedFor), 0);
	EXPECT_EQ(gpuRead(0x40000), 0xa2);
	EXPECT_EQ(gpuRead(0x80000), 0xb1);
	EXPECT_TRUE(newWorkBlocked());
	chan.completed = 6;
	client.retireBatch();
	EXPECT_FALSE(newWorkBlocked());
	EXPECT_EQ(gpuRead(0x80000), 0xb2);
}

// The epoch of a cut's kept pages is published only once the cut's snapshot of queued work is taken: a submission that
// acquires it is never counted among the work that holds those pages back.
TEST_F(ClientRetirement, ACutsSnapshotIsTakenBeforeItsEpochIsPublished)
{
	auto big = bytesOf(0x5a, 0x20000);
	client.add(54, 0x200000, 0x20000, &big, true);
	chan.completed = 0;
	bool published = false;
	onBindPublished = [&] {
		published = true;
		ASSERT_NE(client.retired_, nullptr);
		EXPECT_EQ(client.retired_->snapshots[0].value, chan.submitted);
	};
	ASSERT_EQ(client.bind(0x208000, 0x1000, nullptr), 0);
	EXPECT_TRUE(published);
	chan.completed = chan.submitted;
	client.retireBatch();
	EXPECT_FALSE(newWorkBlocked());
	EXPECT_EQ(gpuRead(0x209000), 0x5a);
}

// A SubmitJob holds its submit lock across a full ring's wait, which waits for the GPU, which may wait for the bind epoch
// retireBatch releases: retireBatch never waits for a submit lock. A freed BO a job may still hold goes back on the
// release call, once that job left.
TEST_F(ClientRetirement, RetirementNeverWaitsForASubmitLock)
{
	client.add(1);
	free(1);
	chan.completed = chan.submitted;
	IOLockLock(client.submitLocks_[3]);
	auto batch = std::async(std::launch::async, [&] {
		client.retireBatch();
	});
	EXPECT_EQ(batch.wait_for(2s), std::future_status::ready)
		<< "retireBatch waited for a submit lock";
	batch.get();
	EXPECT_EQ(client.releases, 0u) << "a BO given back while a job may still hold it";
	EXPECT_GT(client.releaseStorage.entries.load(), 0u);
	IOLockUnlock(client.submitLocks_[3]);
	client.releaseLeft();
	EXPECT_EQ(client.releases, 1u);
}

// A resource set member's free ends the set's generation and waits for the jobs inside it: that BoFree alone closes the
// submit gate; any other leaves it open.
TEST_F(ClientRetirement, OnlyASetMembersFreeClosesTheGate)
{
	auto *member = reinterpret_cast<switch_gpu_backing *>(uintptr_t(0x1000));
	auto *other = reinterpret_cast<switch_gpu_backing *>(uintptr_t(0x2000));
	client.add(1)->backing = member;
	client.add(2, 0x20000)->backing = other;
	setMembers = {member};
	free(2);
	EXPECT_EQ(client.submitGate_.mutations, 0u);
	free(1);
	EXPECT_EQ(client.submitGate_.mutations, 1u);
	EXPECT_FALSE(client.submitGate_.mutating());
	setMembers.clear();
}

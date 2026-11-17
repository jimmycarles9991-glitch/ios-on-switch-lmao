// SPDX-License-Identifier: MIT
// Where the user client's memory work runs, which no host model of it runs: spares of 128 KiB units are made by the
// refill thread alone; BoAlloc and BoImport make their BO without the connection lock and publish it without closing the
// submit gate, a VmBind closes it only to cut, SubmitJob reads only published tables, and retirement frees what left
// them only after taking every submit lock once.
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

TEST(GM20BMemSource, OnlyTheRefillThreadMakesSparesOfUnits)
{
	const std::string pool = unit("TegraGM20BUserClientSpares.cpp");
	const CodeText make = Block(pool, "Spare *spareMake(SparePool *p, uint64_t bytes)");
	EXPECT_TRUE(make.before("switch_gpu_cached_buffer_units(bytes, &buffer)",
				"switch_gpu_cached_buffer_pages(bytes, &buffer)"));
	EXPECT_EQ(CodeText(pool).count("spareMake("), 2u) << "its definition and spareWork's call";
	EXPECT_TRUE(Block(pool, "void spareWork(thread_call_param_t param, thread_call_param_t)")
			    .has("spareMake(p, classBytes(c))"));
	const CodeText alloc =
		Block(unit("TegraGM20BUserClientBo.cpp"), "IOReturn AGXDeviceUserClient::boAlloc(");
	EXPECT_FALSE(alloc.has("switch_gpu_cached_buffer_units"));
	EXPECT_TRUE(alloc.has("if (in->flags & TegraGM20BBoContiguous) options |= "
			      "kIOMemoryPhysicallyContiguous;"));
	EXPECT_EQ(alloc.count("kIOMemoryPhysicallyContiguous"), 1u);
}

TEST(GM20BMemSource, AUnitIsOneAlignedContiguousRun)
{
	const std::string buffer = unit("SwitchGpuCachedBuffer.cpp");
	const CodeText part = Block(buffer, "IOMemoryDescriptor *part(uint64_t bytes, bool unit)");
	EXPECT_TRUE(part.has("unit ? kOptions | kIOMemoryPhysicallyContiguous : kOptions"));
	EXPECT_TRUE(part.has("unit ? kReach & ~(SWITCH_GPU_SPARE_UNIT_BYTES - 1) : kReach"));
	EXPECT_TRUE(Block(buffer, "bool switch_gpu_cached_buffer_alone(")
			    .has("getRetainCount() == (buffer.map ? 2 : 1)"));
}

TEST(GM20BMemSource, BoAllocAndBoImportBuildOutsideTheConnectionLock)
{
	const CodeText run = Block(unit("TegraGM20BUserClientDispatch.cpp"),
				   "IOReturn AGXDeviceUserClient::run(");
	EXPECT_TRUE(run.before("return boAlloc(", "switch_gpu_method_closes_gate(method)"));
	EXPECT_TRUE(run.before("return boImport(", "switch_gpu_method_closes_gate(method)"));
	EXPECT_EQ(run.count("boAlloc("), 1u);
	EXPECT_EQ(run.count("boImport("), 1u);
	const CodeText import = Block(unit("TegraGM20BUserClientImport.cpp"),
				      "IOReturn AGXDeviceUserClient::boImport(");
	EXPECT_EQ(import.count("IOLockLock(lock_);"), 1u);
	EXPECT_TRUE(import.before("bo->wired->prepare()", "IOLockLock(lock_);"));
	EXPECT_TRUE(import.before("tgm_mem_new_waited(", "IOLockLock(lock_);"));
	EXPECT_TRUE(import.before("lap(3);", "IOLockLock(lock_);"));
	EXPECT_TRUE(import.between("IOLockLock(lock_);", "IOLockUnlock(lock_);")
			    .has("publishBo(bo, false)"));
	EXPECT_FALSE(import.has("beginMutation"));
	const std::string bo = unit("TegraGM20BUserClientBo.cpp");
	const CodeText alloc = Block(bo, "IOReturn AGXDeviceUserClient::boAlloc(");
	EXPECT_EQ(alloc.count("IOLockLock(lock_);"), 2u);
	EXPECT_TRUE(alloc.between("IOLockLock(lock_);", "IOLockUnlock(lock_);")
			    .has("const BoCharge charge = chargeBo(in);"));
	EXPECT_TRUE(alloc.before("const BoCharge charge = chargeBo(in);", "spareTake(in->bytes)"));
	EXPECT_TRUE(alloc.before("cleanZeros(va, backing);",
				 "ret = settleBo(in, charge, bo, ret, chunksRefused, system);"));
	EXPECT_FALSE(alloc.has("beginMutation"));
	EXPECT_TRUE(
		Block(bo, "IOReturn AGXDeviceUserClient::settleBo(").has("publishBo(bo, true)"));
	const CodeText publish = Block(bo, "IOReturn AGXDeviceUserClient::publishBo(");
	EXPECT_TRUE(publish.before("submitGate_.closed()", "boIndex_.insert(bo->handle, bo);"));
	EXPECT_FALSE(publish.has("beginMutation"));
}

TEST(GM20BMemSource, AVmBindClosesTheGateOnlyToCut)
{
	const std::string channel = unit("TegraGM20BUserClientChannel.cpp");
	const CodeText bind = Block(channel, "IOReturn AGXDeviceUserClient::vmBind(");
	EXPECT_TRUE(bind.has("if (cuts && !submitGate_.beginMutation(lock_))"));
	EXPECT_TRUE(bind.has("if (cuts) submitGate_.endMutation(lock_);"));
	EXPECT_TRUE(bind.before("BindingTable::prepare(nbinds_ + 2 * count)",
				"bindOp(in->ops[i], changed)"));
	EXPECT_TRUE(bind.before("bindOp(in->ops[i], changed)", "publishBindings(snapshot);"));
	EXPECT_TRUE(Block(channel, "const uint32_t *AGXDeviceUserClient::pushView(")
			    .has("hit = bindTable_.find(va, bytes);"));
}

TEST(GM20BMemSource, SubmitJobReadsOnlyPublishedTables)
{
	const CodeText uses = Block(unit("TegraGM20BUserClientJobSubmit.cpp"),
				    "IOReturn AGXDeviceUserClient::runJobUses(");
	EXPECT_TRUE(uses.has("bindTable_.find(use.handle, bytes ? bytes : 1)"));
	EXPECT_TRUE(uses.has("findBo(use.handle)"));
	EXPECT_FALSE(uses.has("binds_"));
	EXPECT_FALSE(uses.has("bos_"));
	const std::string scan = unit("TegraGM20BPushScanRun.cpp");
	EXPECT_FALSE(CodeText(scan).has("binds_"));
}

TEST(GM20BMemSource, RetirementFreesWhatLeftTheTablesAfterEverySubmitLock)
{
	const std::string retirement = unit("SwitchGpuClientRetirement.h");
	const CodeText batch = Block(retirement, "void AGXDeviceUserClient::retireBatch()");
	EXPECT_TRUE(batch.before(
		"Left left = {release, boIndex_.takeRetired(), bindTable_.takeRetired()};",
		"IOLockUnlock(lock_); if (!left.bos && !left.indexes && !left.bindings)"));
	EXPECT_TRUE(batch.has("const bool mutation = submitGate_.mutating();"));
	EXPECT_TRUE(
		batch.has("if (!mutation && passSubmitters(false)) { dropLeft(left); return; }"));
	EXPECT_TRUE(batch.before("keepLeft(left);", "thread_call_enter(releaseCall_);"));
	EXPECT_FALSE(batch.has("passSubmitters(true)"))
		<< "retireBatch never waits for a submit lock";
	const CodeText left = Block(retirement, "void AGXDeviceUserClient::releaseLeft()");
	EXPECT_TRUE(left.before("left_ = {};", "IOLockUnlock(lock_);"));
	EXPECT_TRUE(left.between("left_ = {};", "IOLockUnlock(lock_);")
			    .has("submitGate_.mutating()) IOLockSleep(lock_, &submitGate_, "
				 "THREAD_UNINT);"));
	EXPECT_TRUE(left.before("passSubmitters(true);", "dropLeft(left);"));
	EXPECT_FALSE(Block(retirement, "bool AGXDeviceUserClient::passSubmitters(bool wait)")
			     .has("lock_"));
	const CodeText free = Block(retirement, "IOReturn AGXDeviceUserClient::freeBo(Bo **p)");
	const CodeText gated = Block(retirement, "IOReturn AGXDeviceUserClient::boFree(");
	EXPECT_TRUE(gated.before("switch_gpu_backing_set((*p)->backing) != nullptr",
				 "submitGate_.beginMutation(lock_)"));
	EXPECT_TRUE(gated.before("submitGate_.beginMutation(lock_)", "freeBo(p)"));
	EXPECT_TRUE(free.before("boIndex_.erase(bo->handle);", "queueZombie(bo);"));
	EXPECT_TRUE(free.before("publishBindings(snapshot);", "queueZombie(bo);"));
	EXPECT_TRUE(free.before("BindingTable::prepare(nbinds_)", "*p = bo->next;"));
	EXPECT_EQ(CodeText(retirement).count("releaseBo("), 2u)
		<< "dropLeft's, after a pass of the submit locks, and reap's at close";
}

// A new surface's zero-filled pages are cleaned by the prefault's thread call, which keeps them wired for the first
// import; that import cleans on the CPU only when tegra_gm20b_import_cleans says so (gm20b_prefault_test).
TEST(GM20BMemSource, TheFirstImportOfASurfaceCleanedAtCreationCleansNothing)
{
	const std::string prefault = unit("TegraGM20BSurfacePrefault.cpp");
	const CodeText fault = Block(prefault, "void faultIn(Prefault *p, const Created &c)");
	EXPECT_TRUE(fault.before("memory->prepare(kIODirectionInOut)", "pmap_clear_modify(page)"));
	EXPECT_TRUE(fault.before("pmap_clear_modify(page)", "clean(memory)"));
	EXPECT_TRUE(
		fault.has("if (record && watched && !pressure() && clean(memory)) hold(p, memory, "
			  "record); else memory->complete(kIODirectionInOut);"));
	const CodeText hold = Block(prefault, "void hold(Prefault *p, IOMemoryDescriptor *memory,");
	EXPECT_TRUE(hold.before("p->held.hold(memory, deadline)", "tgm_record_imported(record)"));
	const CodeText take =
		Block(prefault, "extern \"C\" IOMemoryDescriptor *tegra_gm20b_prefault_take(");
	EXPECT_TRUE(take.before("p->held.take(memory)", "pmap_is_modified(page)"));
	EXPECT_TRUE(take.has("if (!written) return held; giveBack(held); return nullptr;"));
	const CodeText import = Block(unit("TegraGM20BUserClientImport.cpp"),
				      "IOReturn AGXDeviceUserClient::boImport(");
	EXPECT_TRUE(import.before("const CreationWiring creation(in->surface_id, task_);",
				  "bo->wired->prepare()"));
	EXPECT_TRUE(import.has(
		"const bool cleans = tegra_gm20b_import_cleans(covered, creation.held(), "
		"fresh != 0, gpuWrites != 0); if (cleans) tgm_cpu_clean_pages(bo->pas, "
		"bo->npages);"));
	EXPECT_EQ(CodeText(unit("TegraGM20BRecord.cpp")).count("__ATOMIC_SEQ_CST"), 2u);
}

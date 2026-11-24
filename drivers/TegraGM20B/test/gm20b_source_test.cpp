// SPDX-License-Identifier: MIT
// Guards on how TegraGM20B's kernel code uses what gm20b_pool_policy_test and gm20b_push_scan_test exercise, and on the
// pool's zeroing, which no host model runs (it lives in nvkm's shim and gpu_scrub.c's channel):
//  - the user client decides a coherent BO's memory through TegraGM20BPoolPolicy.h, serves a BO the pool does not from
//    a write-combined buffer of its own, never refuses a coherent BO for want of pool room, and the stats line names
//    the pool's holders;
//  - the CPU does not clear the reserved pool at boot; every page an allocation hands out is zero whether the CPU or
//    the copy engine zeroed it; the scrubber gives a range back clean only from its fence's interrupt pass, never waits
//    on the CPU, and a range not back in time has the hang rule reset its engines and give the range back dirty;
//  - the user client reads pushes only through TegraGM20BPushScan.h and profiles only what it read;
//  - a fence-only present (direct scanout) copies nothing, and the debug read-back rides the frame's own present.
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::Configured;
using switch_ios::test::Occurrences;
using switch_ios::test::SourceText;

namespace
{
const std::string &client()
{
	static const std::string source = [] {
		std::string units;
		for (const char *unit : {"", "Pool", "Spares", "Bo", "Park", "Import", "Channel",
					 "Submit", "Dispatch", "Present", "Job", "JobSubmit"})
			units += SourceText(std::string("../TegraGM20BUserClient") + unit + ".cpp");
		return units + SourceText("../SwitchGpuParkedPool.cpp") +
		       SourceText("../TegraGM20BPushScanRun.cpp") +
		       SourceText("../TegraGM20BUserClientInternal.h") +
		       SourceText("../SwitchGpuClientRetirement.h") +
		       SourceText("../SwitchGpuClientBinds.h");
	}();
	return source;
}

const std::string &gpu()
{
	static const std::string source = [] {
		std::string units;
		for (const char *unit :
		     {"bringup", "clock", "mem", "hang", "describe", "acquire", "stats",
		      "stats_tick", "chan", "chan_level", "sync", "exec", "present",
		      "present_deadline", "present_redo", "reload", "scrub", "power"})
			units += SourceText(std::string("../gpu_") + unit + ".c");
		return units + SourceText("../gpu_internal.h") + SourceText("../gpu_inline.h") +
		       SourceText("../gpu_fenced.h") + SourceText("../SwitchGpuVmMap.h") +
		       SourceText("../SwitchGpuChannelCompletion.h");
	}();
	return source;
}

const CodeText &clientCode()
{
	static const CodeText code(client());
	return code;
}

const CodeText &gpuCode()
{
	static const CodeText code(gpu());
	return code;
}
}  // namespace

TEST(GM20BPoolPolicySource, BoAllocNeverRefusesACoherentBoForPoolRoom)
{
	EXPECT_TRUE(clientCode().has("#include \"TegraGM20BPoolPolicy.h\""));
	const CodeText refusal =
		Block(client(),
		      "const char *poolRefusal(uint64_t bytes, uint64_t held, bool compositor)");
	EXPECT_TRUE(refusal.has("tgm_pool::refusal("));
	EXPECT_TRUE(refusal.has("compositor);"));  // the compositor keeps nvkm's floor
	const CodeText alloc = Block(client(), "IOReturn AGXDeviceUserClient::boAlloc(");
	EXPECT_TRUE(
		Block(client(), "AGXDeviceUserClient::BoCharge AGXDeviceUserClient::chargeBo(")
			.has("c.refused = control ? nullptr : poolRefusal(in->bytes, pool_bytes_, "
			     "critical_);"));
	EXPECT_TRUE(alloc.has("tgm_pool::coherentSource(charge.refused, bo->pool_va != nullptr)"));
	// A refusal does not fail the BO: no NoMemory between the tier check and the pool allocation.
	EXPECT_FALSE(alloc.between("if (!ret && (in->flags & TegraGM20BBoCoherent)) {",
				   "if (!ret && bo->pool_va)")
			     .has("kIOReturnNoMemory"));
	EXPECT_FALSE(clientCode().has("GPU pool BoAlloc of %llu KiB refused"));
	// A system BO: a write-combined buffer of its own, mapped as itself. A physical view over its managed pages would set
	// their cache attribute through a device pager whose private pages reset it to cacheable whenever they are freed.
	EXPECT_TRUE(alloc.between("} else if (!ret && !compressible) {",
				  "if (!ret && bo->buffer.memory) {")
			    .has("if (system) options |= kIOMapWriteCombineCache;"));
	EXPECT_FALSE(alloc.has(
		"kIOMemoryTypePhysical64) | kPoolCacheMode, TASK_NULL); IODelete(ranges"));
	EXPECT_TRUE(alloc.has(
		"if (!ret && bo->buffer.memory) { bo->view = bo->backing_pages == bo->npages"));
}

TEST(GM20BPoolPolicySource, SystemBosGoAsCachedBosDo)
{
	const CodeText release = Block(client(), "void AGXDeviceUserClient::releaseBo(Bo *bo)");
	EXPECT_TRUE(release.has("if (bo->pool_va && bo->view) orphan = IONew(PoolOrphan, "
				"1); if (orphan) {"));
	EXPECT_TRUE(release.has("!bo->system && !(bo->flags & TegraGM20BBoContiguous) &&"));
	EXPECT_TRUE(release.before("spareKeep(", "switch_gpu_cached_buffer_free(&bo->buffer);"));
	EXPECT_FALSE(Block(client(), "void releaseOrphans()").has("spareKeep"));
	// The kernel's mapping of a system BO is write-combined as its task's is: pushView reads it as a cached BO's.
	EXPECT_TRUE(Block(client(), "const uint32_t *AGXDeviceUserClient::pushView(")
			    .has("if (hit && hit->bo) { if (hit->bo->pool_va) {"));
	EXPECT_TRUE(Block(client(), "void AGXDeviceUserClient::scanPushes(")
			    .has("pushView(pushes[i].va, static_cast<uint64_t>(pushes[i].dwords) * "
				 "4, hit)"));
	EXPECT_TRUE(Block(client(), "int AGXDeviceUserClient::hungInterlocks(")
			    .has("pushView(va[i], static_cast<uint64_t>(dwords[i]) * 4, hit)"));
}

// A system BO neither takes a spare nor gives its pages to the spares, which are cacheable: it is made at its own size.
// A cached BO's pages are in one state from every source: zero in DRAM, no line in the cacheable kernel alias
// (cleanZeros publishes a missed BoAlloc's KMA_ZERO; every spare, made or recycled, becomes ready only after CE
// completion, so no CPU thread zeroes a spare).
TEST(GM20BPoolPolicySource, SystemBosNeverUseSpares)
{
	const CodeText alloc = Block(client(), "IOReturn AGXDeviceUserClient::boAlloc(");
	EXPECT_TRUE(
		alloc.has("if (!ret && !bo->pool_va && !compressible && !system && "
			  "!(in->flags & TegraGM20BBoContiguous)) spare = spareTake(in->bytes);"));
	EXPECT_TRUE(alloc.has("(in->flags & TegraGM20BBoContiguous) || system ? kClasses : "
			      "classUp(in->bytes);"));
	EXPECT_TRUE(alloc.has("cleanZeros(va, backing);"));
	EXPECT_FALSE(Block(client(), "Spare *spareMake(SparePool *p, uint64_t bytes)")
			     .has("cleanZeros"));
	const CodeText work =
		Block(client(), "void spareWork(thread_call_param_t param, thread_call_param_t)");
	EXPECT_TRUE(work.has("switch_gpu_cached_zero(s->pas"));
	EXPECT_FALSE(work.has("cleanZeros(s->buffer"));
	EXPECT_FALSE(work.has("p->ready[c] = s;"));
	EXPECT_EQ(work.count("p->dirty = s;"), 2u)
		<< "a busy scrub's spare put back, and the refill's new one";
	const CodeText completed = Block(client(), "void spareZeroed(void *context, int result)");
	EXPECT_TRUE(
		completed.before("const bool keep = !result && spareRoom();", "p->ready[c] = s;"));
	const CodeText zeros = Block(client(), "void cleanZeros(void *va, uint64_t bytes)");
	EXPECT_TRUE(zeros.has("switch_dcache_clean_invalidate(va, bytes);"));
	EXPECT_FALSE(zeros.has("bzero"));
	// A pool view's pages go back only once no task maps the view; a cached BO's buffer is a spare only once nothing
	// else holds it.
	const CodeText orphans = Block(client(), "void releaseOrphans()");
	EXPECT_TRUE(orphans.before("if (orphan->view->getRetainCount() > 1) {",
				   "orphan->view->release();"));
	EXPECT_TRUE(orphans.has("orphan->zero &&"));
	EXPECT_TRUE(Block(client(), "bool spareKeep(const SwitchGpuCachedBuffer &buffer, "
				    "uint64_t *pas, uint64_t bytes,")
			    .has("switch_gpu_cached_buffer_alone(buffer)"));
	const CodeText free = Block(client(), "IOReturn AGXDeviceUserClient::freeBo(Bo **p)");
	EXPECT_TRUE(free.before("unbindOverlaps(binds_[i].va, binds_[i].range, bo);",
				"queueZombie(bo);"));
}

// The import costs (the "imports" and "slow import" lines, the refused import's region) are the debug kernel's alone:
// a release kernel neither reads the thread's CPU time nor walks the task's map.
TEST(GM20BImportCostSource, ImportCostsAreDebugOnly)
{
	EXPECT_TRUE(
		CodeText(Configured(gpu(), "TGM_STATS", true)).has("thread_get_runtime_self()"));
	EXPECT_EQ(CodeText(Configured(gpu(), "TGM_STATS", false)).count("thread_get_runtime_self"),
		  0u);
	EXPECT_TRUE(Block(Configured(SourceText("../TegraGM20BGpu.h"), "TGM_STATS", false),
			  "static inline uint64_t tgm_stats_cpu_ns(void)")
			    .has("return 0;"));
	const CodeText import = Block(client(), "IOReturn AGXDeviceUserClient::boImport(");
	EXPECT_TRUE(import.has("#if TGM_STATS describeRegion(task_, in->address, region, "
			       "sizeof(region)); #endif"));
	EXPECT_TRUE(import.has("tgm_mem_new_waited(vm_, bo->pas, bo->npages, &bo->mem, "
			       "&cost.lock_ns)"));
	EXPECT_TRUE(Block(gpu(), "static int mem_new(")
			    .has("asked = stats_on() && lock_wait_ns ? nvxnu_nanotime() : 0;"));
}

TEST(GM20BPoolPolicySource, StatsNameThePoolHolders)
{
	const CodeText &stats = gpuCode();
	EXPECT_TRUE(
		stats.has("tgm_pool_breakdown(&pool_clients, &pool_orphan_bytes, &pool_orphans, "
			  "&pool_comp_bytes, &pool_comps, &pool_system_bytes, &pool_systems);"));
	EXPECT_TRUE(stats.has(
		"pool %lu/%llu MiB (compositor %llu, clients \" \"%llu, orphans %llu in"));
	EXPECT_TRUE(stats.has("compressible %llu MiB in %llu;"));
}

// A Compressible BO is pool chunks under the pool's tiers, never a view a task could map, and its chunks go back only once
// its GPU memory object (and with it its comptags) is gone.
TEST(GM20BPoolPolicySource, CompressibleBosArePoolChunksNoTaskMaps)
{
	const CodeText alloc = Block(client(), "IOReturn AGXDeviceUserClient::boAlloc(");
	const CodeText chunks = alloc.between("if (!ret && compressible) {",
					      "if (!ret && (in->flags & TegraGM20BBoCoherent)) {");
	EXPECT_TRUE(chunks.has("ret = charge.refused ? kIOReturnNoMemory : takeChunks("));
	EXPECT_TRUE(Block(client(), "AGXDeviceUserClient::BoCharge AGXDeviceUserClient::chargeBo(")
			    .has("c.refused = compressibleRefusal(in->bytes);"));
	EXPECT_TRUE(Block(client(), "const char *compressibleRefusal(uint64_t bytes)")
			    .has("tgm_pool::compressibleRefusal(bytes, tgm_pool_avail(), limits)"));
	EXPECT_TRUE(chunks.has("takeChunks(in->bytes, bo->pas)"));
	EXPECT_TRUE(alloc.has("} else if (!ret && !compressible) {"));
	EXPECT_FALSE(Block(client(), "IOReturn takeChunks(uint64_t bytes, uint64_t *pas)")
			     .has("poolZeroNow()"));
	const CodeText release = Block(client(), "void AGXDeviceUserClient::releaseBo(Bo *bo)");
	EXPECT_TRUE(release.before("tgm_mem_del(&bo->mem);",
				   "tgm_pool_free_chunks(bo->pas, bo->npages);"));
	EXPECT_TRUE(Block(client(), "IOReturn AGXDeviceUserClient::memoryForType(")
			    .has("bo && bo->view"));
	EXPECT_TRUE(Block(SourceText("../shim/linux.c"),
			  "int nvxnu_pool_alloc_chunks(unsigned long count, unsigned "
			  "long long *pas)")
			    .has("tgm_comp_pick_chunks(pool_map, pool_dirty, pool_npages,"));
	EXPECT_TRUE(Block(client(), "IOReturn AGXDeviceUserClient::cpuAccess(")
			    .has("bo->chunked || bo->smmuCompressible"));
	EXPECT_TRUE(Block(SourceText("../SwitchGpuUserCpuComplete.h"),
			  "IOReturn AGXDeviceUserClient::cpuComplete(")
			    .has("bo->chunked || bo->smmuCompressible"));
	EXPECT_TRUE(Block(client(), "void AGXDeviceUserClient::noteBind(")
			    .has("else if (bo->chunked || bo->smmuCompressible) "
				 "n.origin = TGM_BIND_COMPRESSIBLE;"));
	EXPECT_TRUE(Block(client(), "IOReturn AGXDeviceUserClient::boPark(")
			    .has("bo->chunked || bo->smmuCompressible,"));
}

TEST(GM20BPoolScrubSource, NoCpuClearOfTheReservedPool)
{
	const CodeText init = Block(SourceText("../shim/xnu_glue.cpp"),
				    "extern \"C\" int nvxnu_pool_init(int *zeroed)");
	EXPECT_FALSE(
		init.between("switch_ram_gpu_pool(&start, &end)", "IOBufferMemoryDescriptor *md")
			.has("memset"));
	EXPECT_TRUE(init.has("*zeroed = 0;"));
	EXPECT_TRUE(init.has("*zeroed = 1;"));  // the fallback still zeroes its own pages
	EXPECT_TRUE(Block(SourceText("../shim/linux.c"), "int nvxnu_pool_setup(void)")
			    .has("bitmap_set(pool_dirty, 0, pool_npages)"));
}

TEST(GM20BPoolScrubSource, EveryAllocationZeroesWhatAFreeLeftDirty)
{
	const std::string shim = SourceText("../shim/linux.c");
	for (const char *signature :
	     {"struct page *alloc_pages(gfp_t gfp, unsigned int order)",
	      "void *dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *handle, gfp_t "
	      "gfp,",
	      "void *nvxnu_pool_alloc_clean(unsigned long size, unsigned long align, unsigned long "
	      "long *pa)"})
		EXPECT_TRUE(Block(shim, signature).has("pool_zero_dirty(")) << signature;
	const CodeText chunks = Block(
		shim, "int nvxnu_pool_alloc_chunks(unsigned long count, unsigned long long *pas)");
	EXPECT_TRUE(chunks.has("dirty > 0"));
	EXPECT_FALSE(chunks.has("pool_zero_dirty("));
	EXPECT_FALSE(Block(shim, "void *nvxnu_pool_alloc_ready(").has("pool_zero_dirty("));
	const CodeText zero = Block(
		shim, "static void pool_zero_dirty(unsigned long first, unsigned long n, int all)");
	EXPECT_TRUE(zero.has("test_bit(first + k, pool_dirty)"));
	EXPECT_TRUE(zero.has("bitmap_clear(pool_dirty, first, n)"));
	// A plain free marks pages dirty; only a zeroed free (CPU or scrubber) marks them clean.
	EXPECT_TRUE(Block(shim, "static void pool_free(unsigned long first, unsigned long n)")
			    .has("bitmap_set(pool_dirty, first, n)"));
	const CodeText claim = Block(shim, "unsigned long nvxnu_pool_claim_dirty(unsigned "
					   "long max_bytes, unsigned long long *pa)");
	EXPECT_TRUE(claim.has("pool_dirty[w] & ~pool_map[w]"));
	EXPECT_TRUE(claim.has(
		"bitmap_set(pool_map, first, n)"));  // claimed ranges are allocated: nobody takes them
}

// A process maps whole VM pages (16 KiB, four pool pages): every pool BO and fence slot it can map is whole VM pages,
// so its mapping never reaches another BO's, another connection's fence or nvkm's own memory.
TEST(GM20BPoolIsolationSource, CpuMappedPoolMemoryIsWholeVmPages)
{
	const CodeText alloc =
		Block(gpu(), "void *tgm_pool_alloc(uint64_t bytes, uint64_t align, uint64_t *pa)");
	EXPECT_TRUE(alloc.has("nvxnu_pool_alloc_clean(pool_bo_bytes(bytes), "
			      "max_t(u64, align, nvxnu_vm_page_size()), &h)"));
	EXPECT_TRUE(Block(gpu(), "void tgm_pool_free(void *va, uint64_t bytes, uint64_t pa)")
			    .has("pool_bo_bytes(bytes)"));
	EXPECT_TRUE(Block(gpu(), "void tgm_pool_free_zeroed(void *va, uint64_t bytes, uint64_t pa)")
			    .has("bytes = pool_bo_bytes(bytes)"));
	EXPECT_TRUE(Block(SourceText("../shim/linux.c"),
			  "void *nvxnu_pool_alloc_clean(unsigned long size, unsigned long "
			  "align, unsigned long long *pa)")
			    .has("align >> PAGE_SHIFT"));
	EXPECT_TRUE(Block(gpu(), "int tgm_fence_page_get(volatile uint32_t **va, uint64_t *pa)")
			    .has("i * TGM_FENCE_SLOT"));
	EXPECT_TRUE(clientCode().has("IOAddressRange range = {fence_pa_, "
				     "static_cast<IOByteCount>(PAGE_SIZE)};"));
}

TEST(GM20BPoolScrubSource, ScrubberFreesOnlyFromItsFence)
{
	const CodeText fenced = Block(gpu(), "void scrub_fenced(struct tgm_chan *chan)");
	EXPECT_TRUE(fenced.has("nvxnu_pool_free_zeroed("));
	EXPECT_TRUE(fenced.has("tgm_chan_completed(chan)"));
	EXPECT_EQ(gpuCode().count("nvxnu_pool_free_zeroed("), 2u)
		<< "tgm_pool_free_zeroed's and the scrubber's";
	const CodeText submit = Block(gpu(), "static int scrub_submit(u64 pa, u64 bytes)");
	EXPECT_TRUE(submit.has("tgm_fill_setup(push->cur, 0)"));
	EXPECT_TRUE(submit.has(
		"chan_fence(chan)"));  // WFI, FB flush, L2 flush, release, nonstall interrupt
	EXPECT_TRUE(submit.has("nvxnu_work_schedule_after(tgm_scrub.deadline, SCRUB_NS)"));
	EXPECT_FALSE(submit.has("nvxnu_idle"));  // no CPU wait for the copy engine
	EXPECT_TRUE(Block(gpu(), "static void scrub_deadline(void *arg)").has("nvxnu_panic("));
	EXPECT_TRUE(Block(gpu(), "static int chan_completion(struct tgm_chan *chan)")
			    .has("chan->kernel == CHAN_SCRUB"));
}

// T-v5a: the scrubber stuck on PBDMA0 with every engine idle went to a last resort that reset only busy or switching
// engines, and the next check panicked. A hang check that finds a kernel channel stuck marks its cut work to be redone
// (the scrubber's ranges go back dirty) before any reset, resets its own engines, then every engine with the client
// channels on them killed; only a reset of every engine with no client context resident that did not free it panics.
TEST(GM20BPoolScrubSource, AStuckKernelChannelHasItsEnginesResetAndItsWorkRedone)
{
	EXPECT_TRUE(Block(gpu(), "enum tgm_hang tgm_hang_recover(")
			    .has("if (verdict == TGM_HANG_KERNEL) return "
				 "tgm_kernel_hang_locked(chans[b], late, who, seqno, waited_ns, "
				 "moved_ns);"));
	EXPECT_FALSE(gpuCode().has("tgm_hang_last_resort"));
	const CodeText hang = Block(gpu(), "enum tgm_hang tgm_kernel_hang_locked(");
	EXPECT_EQ(hang.count("nvxnu_panic("), 1u);
	EXPECT_TRUE(hang.has("hardware failure: the engines of %s's channel %u "));
	EXPECT_TRUE(hang.has("did not come back after a reset with no client context resident: "));
	EXPECT_TRUE(
		hang.before("if (step == TGM_KERNEL_HANG_FATAL) {", "tgm_ctxsw_redo_mark(stuck);"));
	EXPECT_TRUE(hang.before("if (step == TGM_KERNEL_HANG_WAIT) return TGM_HANG_RECOVERING;",
				"tgm_recover_kernel(tgm_gpu.device,"))
		<< "a waiter within a step's gap takes no step";
	EXPECT_TRUE(hang.before("if (step == TGM_KERNEL_HANG_ALL) {", "kill_clients(stuck,"));
	EXPECT_TRUE(
		hang.before("tgm_ctxsw_redo_mark(stuck);", "tgm_recover_kernel(tgm_gpu.device,"));
	EXPECT_TRUE(
		Block(gpu(), "void tgm_ctxsw_redo_mark(").has("tgm_scrub_redo(chan->submitted)"));
	const CodeText resets = Block(SourceText("../recover_held.c"),
				      "static int reset_for(struct nvkm_device *device,");
	EXPECT_TRUE(resets.before("tgm_chid_cut(held[i]);",
				  "tgm_engine_reset_locked(runl, engn, chid, why);"))
		<< "another kernel channel's context on a reset engine is redone too";
	EXPECT_TRUE(resets.has("if (!all && !tgm_kernel_hang_resets("));
	EXPECT_FALSE(resets.has("PFIFO_ENG_BUSY")) << "an idle engine is reset as a busy one is";
}

TEST(GM20BPoolScrubSource, FreedPoolBosGoToTheGpuFirst)
{
	EXPECT_TRUE(Block(client(), "bool poolZeroLater(void *va, uint64_t pa, uint64_t bytes)")
			    .before("tgm_pool_scrub(pa, bytes)", "spares()"));
}

// The user client reads pushes only through the header gm20b_push_scan_test tests, and profiles only what it read.
TEST(GM20BPushScanSource, KernelUsesTheHeader)
{
	EXPECT_TRUE(clientCode().has("#include \"TegraGM20BPushScan.h\""));
	EXPECT_FALSE(clientCode().has("void pushMethod("));
	EXPECT_FALSE(clientCode().has("void pushParse("));
	EXPECT_EQ(clientCode().count("tgm_scan::readSubmission("), 2u)
		<< "the profiled read and the thread call's";
	const CodeText scan = Block(SourceText("../TegraGM20BPushScan.h"),
				    "inline SubmissionRead readSubmission(");
	EXPECT_TRUE(scan.has("scanPush(r.c, p, whole, nullptr, 0)"));
	EXPECT_TRUE(scan.has("prof->cls[i] = cls;"));
	// A stamp only after a push the profile's rule left a class (switch_gpu_prof_stamped), only on a profiled submission.
	EXPECT_TRUE(Block(gpu(), "void chan_job_entry(")
			    .has("if (prof && chan->prof_cls[i] != TGM_PC_SKIP) {"));
	// A job with internal fences is profiled whole: read at its last seqno, against its first's start stamp.
	EXPECT_TRUE(Block(gpu(), "static void chan_submission_room(")
			    .has("|| count > TGM_PROF_MAX || profiled))"));
	EXPECT_TRUE(Block(gpu(), "int chan_exec_body(").has("chan->submitted + 1 + fences); /*"));
	EXPECT_TRUE(Block(gpu(), "static inline void stats_fences(")
			    .has("prof_collect(chan, first, fs->start.ns, st->end.ns)"));
}

// Direct scanout's fence-only present (NativeMobileDisplay, TEGRA_SCANOUT_COPY_FENCE_ONLY) launches no copy.
TEST(GM20BPresentSource, FenceOnlyPresentHasNoCopy)
{
	EXPECT_TRUE(Block(gpu(), "static int present_copy(")
			    .has("if (dst_va) tgm_present_push_copy(push);"));
}

// gpu.poster_targets reads each process's largest colour target, which the stats' push scan records (debug kernels).
// SubmitJob takes the scan before its kick and hands its profile to the kick; the scan's counts are atomic, as SubmitJob
// runs without the connection lock.
TEST(GM20BPushScanSource, EverySubmissionPathScansItsPushes)
{
	const CodeText job =
		Block(client(),
		      "IOReturn AGXDeviceUserClient::runJobUses(const SwitchGpuJobRecord &record,");
	EXPECT_TRUE(job.before("const tgm_prof *profile = scanSubmission(slot, fences, pushes, "
			       "count, prof, true);",
			       "switch_gpu_chan_exec_events("));
	EXPECT_TRUE(job.has("&seqno, profile, jobVisibility, releases, keptCount, &transaction);"));
	EXPECT_EQ(clientCode().count("scanPushes(pushes, count,"), 1u);
	EXPECT_TRUE(Block(client(), "const tgm_prof *AGXDeviceUserClient::scanSubmission(")
			    .has("__atomic_fetch_add(&pushSubmits_, 1u, __ATOMIC_RELAXED) % "
				 "kScanEvery"));
}

// The render server waits for its submissions' thread (waitUntilScheduled), so only a profiled submission, whose kick
// stamps what the read classed, is read on it; the others are read on a thread call, and only where their words lie in
// the pool, whose kernel mapping outlives every BO (boot 21: 36-57 KiB of write-combined reads every 4th compositor job).
TEST(GM20BPushScanSource, OnlyAProfiledSubmissionIsReadOnItsThread)
{
	const CodeText submission =
		Block(client(), "const tgm_prof *AGXDeviceUserClient::scanSubmission(");
	EXPECT_TRUE(submission.before("deferScan(slot, fences, pushes, count, metalMacros);",
				      "scanPushes(pushes, count, &prof, metalMacros);"));
	const CodeText defer = Block(client(), "void AGXDeviceUserClient::deferScan(");
	EXPECT_TRUE(defer.has("hit->bo->pool_va"));
	EXPECT_TRUE(defer.has("thread_call_enter(d->call);"));
	EXPECT_FALSE(defer.has("readSubmission("));
	EXPECT_TRUE(defer.has("r->seqno = tgm_chan_submitted(chans_[slot]) + fences + 1;"));
	const CodeText work = Block(client(), "void deferredWork(");
	EXPECT_TRUE(work.has("switch_gpu_scan_record("));
	EXPECT_TRUE(work.has("submissionPassed"));
	EXPECT_TRUE(Block(client(), "Deferral *deferral()").has("THREAD_CALL_OPTIONS_ONCE"));
	EXPECT_TRUE(Block(client(), "IOReturn AGXDeviceUserClient::boImport(")
			    .has("if (critical) switch_gpu_compositor_import(cost.total_ns);"));
}

// A copying present flushes the L2 once, before the copy reads the frame; its copy writes around the L2, so its fence
// does not flush again. A fence-only present (direct scanout of a frame the GPU drew through the L2) keeps the fence's.
TEST(GM20BPresentSource, OneL2FlushPerPresent)
{
	const CodeText copy = Block(gpu(), "static int present_copy(");
	EXPECT_TRUE(
		Block(gpu(), "void tgm_present_push_copy(")
			.before("NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY", "tgm_copy_texels("));
	EXPECT_TRUE(copy.has("switch_gpu_present_tokens_need_l2_flush("));
	EXPECT_TRUE(copy.has(
		"switch_gpu_chan_fence_sealed(chan, need_flush ? 1 : TGM_FENCE_VOLATILE_WRITES,"));
	const CodeText fence = Block(gpu(), "static u32 chan_fence_job(");
	EXPECT_TRUE(
		fence.before("NVB06F_FB_FLUSH, 0); if (external != TGM_FENCE_VOLATILE_WRITES) {",
			     "NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY"));
}

// A present waits for exactly the producers its surfaces' backings name (TegraScanoutCopy alsoWritten: window B's too),
// never for every channel whose address space maps them; window B's own fence maps nothing and copies nothing.
TEST(GM20BPresentSource, PresentWaitsForTheSurfacesProducersOnly)
{
	const CodeText copy = Block(gpu(), "static int present_copy(");
	EXPECT_FALSE(copy.has("tgm_live_chans"));
	EXPECT_FALSE(gpuCode().has("int tgm_present_copy("));
	EXPECT_FALSE(gpuCode().has("int tgm_present_copy_read("));
	EXPECT_TRUE(Block(client(), "extern \"C\" int tegra_gm20b_scanout_copy(")
			    .has("switch_gpu_backing_find_pa(copy->alsoWritten & ~0xfffULL)"));
	const CodeText fence = Block(client(), "extern \"C\" int tegra_gm20b_scanout_fence(");
	EXPECT_TRUE(fence.before("switch_gpu_backing_find_pa(pa & ~0xfffULL)",
				 "gPresentSnapshot.add(backing)"));
	EXPECT_TRUE(fence.before("gPresentSnapshot.add(backing)",
				 "tgm_present_copy_dependencies(0, 0, -1, 1, 1, 0, 0, 0, "
				 "SwitchGpuScanoutSnapshot::complete, &gPresentSnapshot, "
				 "gPresentSnapshot.tokens, gPresentSnapshot.count)"));
	EXPECT_TRUE(fence.has("if (ret && !presentBusy) gPresentSnapshot.cancel();"));
	EXPECT_TRUE(fence.has("switch_gpu_backing_release(backing);"));
	EXPECT_FALSE(fence.has("presentSurface("));
}

// The scanout read-back (TegraGM20BReadbackABI.h) rides the frame's own present: a submission of its own would hold the
// present channel, which then refuses the display's next offer, and the display copies that frame on the CPU. Its copies
// run after the frame's copy, each after the copies before it, into pitch-linear buffers, and before the fence; a refused
// present gives the read-back back; its completion, in the GPU interrupt pass, takes no lock and touches no cache; and a
// release kernel has none of it.
TEST(GM20BPresentSource, ReadbackRidesTheFramesPresent)
{
	const CodeText copy = Block(gpu(), "static int present_copy(");
	EXPECT_TRUE(copy.before("if (dst_va) tgm_present_push_copy(push);",
				"present_push_reads(push, reads, nreads);"));
	EXPECT_TRUE(copy.before("present_push_reads(push, reads, nreads);",
				"switch_gpu_chan_fence_sealed("));
	EXPECT_TRUE(copy.has("nreads * TGM_COPY_RECT_DWORDS"));
	const CodeText reads = Block(gpu(), "static void present_push_reads(");
	EXPECT_TRUE(reads.has("dst = {r->dst_va, r->dst_pitch, -1};"));  // pitch-linear
	EXPECT_TRUE(reads.has("tgm_copy_texels(push->cur, &src, &dst, r->width, r->height)"));
	EXPECT_TRUE(CodeText(Configured(gpu(), "TGM_STATS", true))
			    .has("static void present_push_reads("));
	EXPECT_FALSE(CodeText(Configured(gpu(), "TGM_STATS", false)).has("present_push_reads("));

	const CodeText scanout = Block(client(), "extern \"C\" int tegra_gm20b_scanout_copy(");
	EXPECT_TRUE(scanout.before("#if TGM_STATS tgm_present_read reads",
				   "#else gPresentSnapshot.done = presentDone; ret = watch ? "
				   "presentWatched() : 1; if (ret "
				   "== 1) ret = tgm_present_copy_dependencies("));
	EXPECT_TRUE(scanout.has("nreads ? tgm_readback_done : presentDone"));
	EXPECT_TRUE(scanout.has("if (ret && nreads) tgm_readback_refused();"));

	const std::string readback = SourceText("../TegraGM20BReadback.cpp");
	const CodeText done = Block(readback, "void tgm_readback_done(void *display, int status)");
	EXPECT_TRUE(done.has("native_legacy_gpu_complete(display, status);"));
	EXPECT_FALSE(done.has("IOLock"));
	EXPECT_FALSE(done.has("tgm_cpu_clean_pages"));
	EXPECT_TRUE(CodeText(Configured(readback, "TGM_STATS", false))
			    .endsWith("#include \"TegraGM20BReadback.h\""));
}

// Live (ui.glass_live): every present, armed or not, records what window A will scan for it, so the next armed present
// can read the frame shown before it again, as it is at the end of its time on screen; only an Arm takes the live flag.
TEST(GM20BPresentSource, LiveReadbackRereadsTheFrameShownBefore)
{
	const std::string readback = SourceText("../TegraGM20BReadback.cpp");
	const CodeText take = Block(readback, "uint32_t tgm_readback_take(");
	EXPECT_TRUE(take.before("const Shown before = gShown;",
				"int armed = TegraGM20BReadbackArmed;"));
	EXPECT_TRUE(take.before("gShown.sequence = copy->sequence;",
				"int armed = TegraGM20BReadbackArmed;"));
	EXPECT_TRUE(take.has("gShown.read = direct ? tgm_present_read{source,"));
	EXPECT_TRUE(take.has(": tgm_present_read{target,"));
	EXPECT_TRUE(take.before("reads[1] = before.read;", "reads[1].dst_va = bufferVa(1);"));
	EXPECT_TRUE(take.has("r.live && before.read.src_va && before.read.width == copy->width"));
	const CodeText call = Block(readback, "IOReturn tgm_readback_call(");
	EXPECT_TRUE(
		call.has("in->reserved > 1 || (in->reserved && in->op != TegraGM20BReadbackArm)"));
	EXPECT_TRUE(call.has("arm(owner, in->reserved == 1)"));
}

// A tiled (block-linear) source: the copy engine reads it block-linear with the tiled block height, never as pitch, through
// a mapping of the kind its texels were written under; a surface's layout comes from its decision (SurfaceLayout), never
// from how one client bound it.
TEST(GM20BPresentSource, TiledSourceIsCopiedBlockLinear)
{
	const CodeText copy = Block(gpu(), "void tgm_present_push_copy(");
	EXPECT_TRUE(copy.has("const u32 src_pitch = tgm_present.copy.src_block_log2 >= 0 ? "
			     "tgm_present.copy.width * 4 : (u32)tgm_present.copy.stride;"));
	EXPECT_TRUE(copy.has("{tgm_present.copy.src_va, src_pitch, "
			     "tgm_present.copy.src_block_log2}"));
	const CodeText scanout = Block(client(), "extern \"C\" int tegra_gm20b_scanout_copy(");
	EXPECT_TRUE(scanout.has(
		"sourceBlock ? static_cast<int32_t>(copy->sourceBlockHeightLog2) : -1;"));
	EXPECT_TRUE(scanout.has("tgm_readback_take(copy, source, blockLog2, target, reads)"));
	EXPECT_FALSE(Block(SourceText("../TegraGM20BReadback.cpp"), "uint32_t tgm_readback_take(")
			     .has("TEGRA_SCANOUT_TILED"));
	const CodeText surface = Block(client(), "uint64_t presentSurface(");
	EXPECT_TRUE(surface.has("uint32_t kind = compressed ? compKind : "
				"(blockLinear ? TGM_LAYOUT_KIND : 0);"));
	EXPECT_TRUE(surface.has("const uint32_t preparedKind = presentCompressedKind(memory);"));
	EXPECT_TRUE(surface.has("switch_gpu_vm_map(vm, va, mapBytes, s.mem, 0, "
				"static_cast<uint8_t>(kind), 0, &s.mapping)"));
	EXPECT_TRUE(surface.has("switch_gpu_vm_map_volatile(vm, va, mapBytes, s.mem, 0, "
				"static_cast<uint8_t>(kind), 0, 1, &s.mapping)"));
	const CodeText drop = Block(client(), "void dropSurface(PresentSurface &s)");
	EXPECT_TRUE(drop.has("switch_gpu_vm_unmap(s.vm, &s.mapping);"));
	EXPECT_FALSE(drop.has("tgm_present_unmap("));
	EXPECT_TRUE(Block(gpu(), "int tgm_present_open(struct tgm_vm **pvm)")
			    .has("tgm_vm_claim(tgm_present.vm, &tgm_present);"));
	EXPECT_TRUE(Block(gpu(), "static int present_copy(")
			    .has("if (tgm_tlb_take(&chan->tlb, &chan->vm->tlb))"));
	EXPECT_FALSE(Block(client(), "IOReturn AGXDeviceUserClient::vmBind(").has("tiled"));
	EXPECT_TRUE(Block(client(), "IOReturn AGXDeviceUserClient::surfaceLayoutInfo(")
			    .has("tgm_coherency_layout(coh,"));
	const CodeText info = Block(client(), "IOReturn AGXDeviceUserClient::surfaceLayoutInfo(");
	EXPECT_TRUE(info.has("? tgm_coherency_layout_fitted(coh, width, height, "
			     "surface.bpp, pitch, writes)"));
	EXPECT_TRUE(info.has("canonical || (same && tgm_layout_nil_height(&tiled))"));
	EXPECT_TRUE(info.has("current && current->backingMemory && current->mem && "
			     "tgm_mem_can_compress(current->mem)"));
	EXPECT_TRUE(info.has("switch_refusal(\"iosurface-layout\", \"uncompressible-bo\","));
}

// System sleep (gpu_power.c "system sleep"): every path that touches the GPU outside tgm_gpu.lock goes through the gate
// gm20b_sleep_test runs, and sleep and wake keep the orders the hardware needs.
TEST(GM20BSleepSource, GpuTouchingPathsGoThroughTheGate)
{
	for (const char *waits : {"int tgm_chan_exec(", "int tgm_chan_new("}) {
		const CodeText body = Block(gpu(), waits);
		EXPECT_TRUE(body.before("gate_enter();", "gate_leave();"));
	}
	for (const char *refused :
	     {"int tgm_present_copy_dependencies(", "int tgm_present_copy_read_dependencies(",
	      "int tgm_pool_scrub(uint64_t pa, uint64_t bytes)",
	      "static void scrub_work(void *arg)"}) {
		const CodeText body = Block(gpu(), refused);
		EXPECT_TRUE(body.before("if (!gate_try())", "gate_leave();"));
	}
	// the drain's and the wake's fence waits are woken by the scrubber's fences too
	EXPECT_TRUE(Block(gpu(), "static int chan_completion(struct tgm_chan *chan)")
			    .has("if (tgm_gate_is_shut(&tgm_sc7_gate)) nvxnu_wake();"));
	// a pass for an interrupt taken before the GPU's power went reads nothing
	EXPECT_TRUE(Block(SourceText("../shim/xnu_glue.cpp"),
			  "void gpuInterrupt(OSObject *, IOInterruptEventSource *source, int)")
			    .has("if (__atomic_load_n(&gGpuOff, __ATOMIC_ACQUIRE)) return;"));
}

// Fence waits sleep on their channel's word: a fence interrupt wakes only the waiters of a channel whose fence reached
// the lowest seqno one of them published, a kill or a teardown wakes that channel's, and the interrupt pass's own wake is
// left to the kernel's waits.
TEST(GM20BFenceWakeSource, WaitersWakeByChannel)
{
	const CodeText nonstall = Block(gpu(), "static int chan_completion(struct tgm_chan *chan)");
	EXPECT_TRUE(nonstall.before("WRITE_ONCE(chan->progress_ns, now); chan_wake_waiters(chan);",
				    "if (chan->kernel == CHAN_SCRUB) {"));
	EXPECT_TRUE(nonstall.has("} else { present_watch_fenced(chan, 0); }"));
	EXPECT_EQ(nonstall.count("nvxnu_wake();"),
		  1u);  // the scrubber's, while the sleep gate is shut
	const CodeText wake = Block(gpu(), "static void chan_wake_waiters(struct tgm_chan *chan)");
	EXPECT_TRUE(wake.has("if (done == chan->woken_done) return;"));
	EXPECT_TRUE(wake.has("if (switch_gpu_waiter_reached(&chan->wanted, done)) "
			     "chan_wake_all_waiters(chan, 1);"));
	EXPECT_TRUE(Block(gpu(), "int tgm_chan_killed(struct nvif_event *event, void "
				 "*repv, u32 repc)")
			    .has("chan_wake_all_waiters(chan, 0);"));
	EXPECT_TRUE(Block(gpu(), "void tgm_chan_del(struct tgm_chan **pchan)")
			    .before("*pchan = NULL;",
				    "nvxnu_wword_wake(&tgm_waiter_words[waiter], 0);"));
	EXPECT_TRUE(Block(SourceText("../shim/xnu_glue.cpp"),
			  "void gpuInterrupt(OSObject *, IOInterruptEventSource *source, int)")
			    .has("wakeAfterPass();"));
	const CodeText wait = Block(client(), "IOReturn AGXDeviceUserClient::waitFence(");
	EXPECT_TRUE(
		wait.before("seq = tgm_waiter_seq(waiter);", "tgm_chan_want(chan, in->seqno);"));
	EXPECT_TRUE(wait.before("tgm_chan_want(chan, in->seqno);",
				"tgm_chan_completed(chan) - in->seqno"));
	EXPECT_TRUE(wait.has("tgm_waiter_idle(waiter, seq, deadline)"));
	// a seqno passed silently (an internal fence) is announced only by its job's end
	EXPECT_TRUE(wait.before("tgm_chan_announced(chan, in->seqno);",
				"tgm_waiter_unwoken(waiter, seq, in->seqno);"));
	EXPECT_FALSE(wait.has("tgm_idle("));
}

// A killed channel is dead alone (client fault isolation): what names it refuses, the connection's memory, binds and other
// channels go on, a new channel can be made in it, and nothing another channel waits for stays on the dead one.
TEST(GM20BKillSource, AKilledChannelLeavesItsConnectionWorking)
{
	const CodeText run = Block(client(), "IOReturn AGXDeviceUserClient::run(uint32_t method,");
	EXPECT_FALSE(clientCode().has("dead_"));
	EXPECT_FALSE(run.has("tgm_chan_error("));
	EXPECT_TRUE(Block(gpu(), "int chan_exec_body(struct tgm_chan *chan,")
			    .has("if (chan->error) return -EIO;"));
	const CodeText killed =
		Block(gpu(), "int tgm_chan_killed(struct nvif_event *event, void *repv, u32 repc)");
	EXPECT_TRUE(killed.has(
		"__atomic_store_n(chan->fence_cpu, chan->submitted, __ATOMIC_RELEASE);"));
	EXPECT_TRUE(killed.has("chan->kernel || !tgm_all_clients()"));
	EXPECT_TRUE(
		killed.before("switch_gpu_timeline_failed(", "__atomic_store_n(chan->fence_cpu"));
	const CodeText freed = Block(gpu(), "static void chan_free(struct tgm_chan *chan)");
	EXPECT_TRUE(freed.before("nvif_event_dtor(&chan->kill)", "switch_gpu_timeline_close("));
	EXPECT_TRUE(Block(client(), "bool AGXDeviceUserClient::bindingIdle(")
			    .has("!tgm_chan_error(chans_[slot])"));
}

// The driver's reopen after its channel is killed: the shader heap's BO and VA outlive the channel, so every program
// placed in it (Device::place) keeps its slot and every state that baked its offset stays drawable; the new channel gets
// what lives in a channel's context again, the program region at the heap, the texture and sampler pools and the root
// constant buffer (emitContext, from the pack's relocations) and the 2D state, and the first command buffer's prologue
// invalidates the shader caches before any draw.
TEST(GM20BRawReopenSource, ReopenKeepsTheHeapAndReplaysTheContext)
{
	const std::string raw = SourceText("../../TegraGM20BMetal/gpu.cpp");
	const std::string channel = SourceText("../../TegraGM20BMetal/device_channel.cpp");
	// channel_recovery.h makeChannel makes the channel, then runs newContext on it
	const CodeText reopen =
		Block(channel, "bool Device::reopen()") +
		Block(channel, "bool Device::newContext(uint32_t channel, uint32_t &seq)");
	EXPECT_FALSE(reopen.has("placed_"));
	EXPECT_FALSE(reopen.has("heapPlaced_"));
	EXPECT_FALSE(reopen.has("pipelines_"));
	EXPECT_FALSE(reopen.has("heap_"));
	EXPECT_TRUE(reopen.before("makeChannel(k_, kEngines, channel_, fence_", "emitContext(p);"));
	EXPECT_TRUE(reopen.before("emitContext(p);", "fx::emitTwodInit(p);"));
	// the interlock's ticket buffer zeroed by the copy engine in the new channel, after the context names it and before
	// the 3D engine runs again
	EXPECT_TRUE(
		reopen.before("emitContext(p);",
			      "uint32_t(tickets_.va >> 32), uint32_t(tickets_.va)}, SUBC_COPY);"));
	EXPECT_TRUE(reopen.before("uint32_t(tickets_.va)}, SUBC_COPY);",
				  "p.m(CE_LAUNCH_DMA, kLaunchPitch, SUBC_COPY);"));
	EXPECT_TRUE(reopen.before("p.m(CE_LAUNCH_DMA, kLaunchPitch, SUBC_COPY);",
				  "p.m(WAIT_FOR_IDLE, 0);"));
	const CodeText context = Block(raw, "void Device::emitContext(Push &p)");
	// the init template relocated to this device's own addresses (pack.cpp relocated, shared with the runtime's capture),
	// the interlock's ticket buffer among them
	EXPECT_TRUE(context.has("const InitAddresses here{heap_.va, zero_.va, root_.va, "
				"tic_.va, tsc_.va, kTicCount - 1, kTscCount - 1, tickets_.va};"));
	EXPECT_TRUE(context.has("relocated(im[i], here)"));
	const CodeText relocated =
		Block(SourceText("../../TegraGM20BMetal/pack.cpp"),
		      "uint32_t relocated(const PackInitMethod &m, const InitAddresses &a)");
	for (const char *reloc :
	     {"case RelocHeapHi:", "case RelocHeapLo:", "case RelocTicHi:", "case RelocTicLo:",
	      "case RelocTscHi:", "case RelocTscLo:", "case RelocRootHi:", "case RelocRootLo:",
	      "case RelocTicketsLo:", "case RelocTicketsHi:"})
		EXPECT_TRUE(relocated.has(reloc));
	EXPECT_TRUE(context.has("sampleWords(p, 1, root_.va);"));
	EXPECT_TRUE(Block(SourceText("../../TegraGM20BMetal/gm20b_gpu_internal.h"),
			  "void sampleWords(Push &p, uint32_t samples, uint64_t root,")
			    .has("sampleLayoutMethods(p, l, kRootBytes, root);"));
	EXPECT_TRUE(Block(SourceText("../../TegraGM20BMetal/multisample.h"),
			  "inline void sampleLayoutMethods(")
			    .has("p.inc(NVB197_SET_CONSTANT_BUFFER_SELECTOR_A,"));
	const CodeText begin = Block(SourceText("../../TegraGM20BMetal/command_buffer.cpp"),
				     "CommandBuffer *Device::begin()");
	EXPECT_TRUE(begin.before("!reopen())", "p.m(INVALIDATE_SHADER_CACHES_NO_WFI,"));
}

// No spin for GPFIFO or push room: every wait for room sleeps until a fence's interrupt, and the fence reports GET to nvif
// before that interrupt, so the room the GPU freed is there when the waiter looks.
TEST(GM20BRoomSource, RoomIsWaitedForByInterrupt)
{
	EXPECT_EQ(gpuCode().count("PUSH_WAIT("), 0u);
	EXPECT_EQ(gpuCode().count("nvif_chan_gpfifo_wait("), 3u);
	const CodeText wait =
		Block(gpu(),
		      "int tgm_chan_wait_ready(struct tgm_chan *chan, u32 gpfifo_nr, u32 push_nr)");
	EXPECT_TRUE(wait.has("if (!nvxnu_idle(seq, end)"));
	const CodeText reserve = Block(gpu(), "static inline int chan_wait_room(");
	EXPECT_TRUE(reserve.before("chan_wait_ready(chan, gpfifo_nr, push_nr)",
				   "nvif_chan_gpfifo_wait(&chan->chan, gpfifo_nr, push_nr)"));
	EXPECT_TRUE(
		Block(gpu(), "static u32 chan_fence_job(struct tgm_chan *chan, int "
			     "external, u32 first, u32 next, int quiet,")
			.before("nvif_chan_gpfifo_post(&chan->chan);", "NON_STALL_INTERRUPT, 0);"));
}

// SubmitJob holds no connection lock and never sleeps for room with its uses' backing locks held: chan_exec_body
// refuses with -EAGAIN instead, and SubmitJob aborts the transaction before it waits.
TEST(GM20BRoomSource, SubmitJobNeverSleepsWithBackingLocksHeld)
{
	const CodeText job = Block(client(), "IOReturn AGXDeviceUserClient::runJobUses(");
	EXPECT_FALSE(job.has("IOLockLock(lock_)"));
	EXPECT_TRUE(job.before("switch_gpu_chan_submit_enter(chan);",
			       "switch_gpu_backing_begin_held("));
	EXPECT_TRUE(job.before("switch_gpu_backing_abort(&transaction);",
			       "switch_gpu_chan_wait_submission_room("));
	EXPECT_FALSE(job.has("switch_gpu_backing_commit("));
	EXPECT_TRUE(job.before("if (error) switch_gpu_backing_abort(&transaction);",
			       "switch_gpu_chan_submit_leave();"));
	EXPECT_TRUE(job.before("switch_gpu_set_note_job(set, slot, &completion);",
			       "switch_gpu_chan_submit_leave();"));
	const CodeText submit = Block(client(), "IOReturn AGXDeviceUserClient::takeJob(");
	EXPECT_TRUE(submit.before("submitGate_.enterFast();", "runJob(record, job, out, why)"));
	EXPECT_TRUE(
		submit.before("runJob(record, job, out, why)", "submitGate_.leaveFast(lock_);"));
	const CodeText emit = Block(gpu(), "int chan_exec_body(");
	EXPECT_TRUE(emit.has("if (!may_wait && !chan_room(chan, entries, words)) return -EAGAIN;"));
	EXPECT_TRUE(emit.before("chan->chan.defer_kick = true;",
				"nvif_chan_gpfifo_wait(&chan->chan, entries, words)"));
	EXPECT_TRUE(emit.has("chan->chan.gpfifo.cur = ring_cur;"));
}

// The HW Test's gpu.path.submit_job reads these per process: a job SubmitJob ran, whether it named its set at the current
// generation, and a refusal for an ended generation.
TEST(GM20BRoomSource, SubmissionMethodsCountTheirPaths)
{
	const CodeText run = Block(client(), "IOReturn AGXDeviceUserClient::runJob(");
	EXPECT_TRUE(run.before("switch_gpu_path_taken(SWITCH_GPU_PATH_STALE_SET, 1);",
			       "SwitchGpuRefusal::StaleSet, kIOReturnNotReady"));
	const CodeText job = Block(client(), "IOReturn AGXDeviceUserClient::runJobUses(");
	EXPECT_TRUE(job.before("switch_gpu_path_taken(SWITCH_GPU_PATH_SUBMIT_JOB, 1);",
			       "return kIOReturnSuccess;"));
	EXPECT_TRUE(job.before("switch_gpu_path_taken(SWITCH_GPU_PATH_SET_JOB, set ? 1 : 0);",
			       "return kIOReturnSuccess;"));
	EXPECT_TRUE(
		job.before("switch_gpu_path_taken(SWITCH_GPU_PATH_SHARED_USE, head.shared_count);",
			   "return kIOReturnSuccess;"));
	const std::string paths = SourceText("../SwitchGpuPathStats.cpp");
	for (const char *name :
	     {"\"submit_jobs\"", "\"set_jobs\"", "\"stale_sets\"", "\"shared_uses\""})
		EXPECT_TRUE(CodeText(paths).has(name));
}

// A SubmitJob command buffer of several jobs holds its shared uses' CPU-lock admission from Begin to End: a Continue job
// takes the admission's guards instead of refreshing its surfaces, and a refused job ends it.
TEST(GM20BRoomSource, SubmitJobHoldsACommandBuffersAdmission)
{
	const CodeText job = Block(client(), "IOReturn AGXDeviceUserClient::runJobUses(");
	EXPECT_TRUE(job.has("switch_gpu_claim_flags(admissionBegin || admissionContinue)"));
	EXPECT_TRUE(job.before("if (admissionContinue && !admission.covers(job.uses, useTotal))",
			       "refresh:"));
	EXPECT_TRUE(job.before("error = admission.begin(job.uses, useTotal, job.guards, "
			       "guardCount, job.leases, leaseCount);",
			       "switch_gpu_chan_submit_enter(chan);"));
	EXPECT_TRUE(job.before("switch_gpu_chan_submit_leave();", "admission.end();"));
	const CodeText submit = Block(client(), "IOReturn AGXDeviceUserClient::takeJob(");
	EXPECT_TRUE(submit.before("(record.job->flags & TegraGM20BJobAdmissionCancel)",
				  "runJob(record, job, out, why)"));
}

// A fence wait woken at its seqno decides from the fence page alone; the connection lock is for publishing, errors and
// timeouts.
TEST(GM20BRoomSource, AWokenFenceWaitTakesNoConnectionLock)
{
	const CodeText wait = Block(client(), "IOReturn AGXDeviceUserClient::waitFence(");
	EXPECT_TRUE(wait.before("switch_gpu_fence_passed(record, in->seqno, waitedOn)",
				"IOLockLock(lock_);"));
	EXPECT_TRUE(wait.before("if (ret != kIOReturnSuccess) { IOLockLock(lock_);",
				"tgm_chan_want(chan, in->seqno);"));
}

// The waiter a submitter sleeps in after -EAGAIN waits for exactly the room chan_exec then checks, computed in one place:
// a waiter that returned with less left the retry loops (TegraGM20BCopy.cpp submit, the user client's SubmitJob)
// spinning.
TEST(GM20BRoomSource, WaiterAndSubmissionReserveOneRoom)
{
	EXPECT_EQ(gpuCode().count("5 * (dependency_count"), 1u);
	const CodeText room = Block(gpu(), "static void chan_submission_room(");
	EXPECT_TRUE(room.has("5 * (dependency_count + 1)"));
	// SubmitJob's sibling acquires take a dependency's room; its retry asks the waiter for both together.
	EXPECT_TRUE(Block(gpu(), "int chan_exec_body(")
			    .before("chan_submission_room(chan, pushes, count, dependency_count + "
				    "sibling_count + word_wait_count, "
				    "fences, &prof, &entries, &words);",
				    "if (!may_wait && !chan_room(chan, entries, words)) return "
				    "-EAGAIN;"));
	EXPECT_TRUE(Block(gpu(), "int switch_gpu_chan_wait_submission_room(")
			    .before("chan_submission_room(chan, pushes, count, dependency_count, "
				    "fences, &prof, &entries, &words);",
				    "ret = tgm_chan_wait_ready(chan, entries, words);"));
}

// The L2 invalidate that makes the CPU's writes visible is ordered on the GPU, in the submission's own stream, ahead of
// a first push that is not prefetched; the CPU does not wait for it. A channel behind its space's GPU-ordered maps
// invalidates its TLB after the L2 (the walk reads page tables through it) in the same entry.
TEST(GM20BL2Source, InvalidateOnTheGpu)
{
	EXPECT_EQ(gpuCode().count("FLUSH_L2_SYSTEM_INVALIDATE"), 0u);
	const CodeText exec = Block(gpu(), "int chan_exec_body(");
	EXPECT_TRUE(exec.before("tlb = tgm_tlb_take(&chan->tlb, &chan->vm->tlb) || bind;",
				"NVB06F_MEM_OP_D_OPERATION_L2_SYSMEM_INVALIDATE);"));
	// a pending bind's acquire comes before the TLB invalidate that makes its new PTEs seen
	EXPECT_TRUE(exec.before(
		"bind = !chan->kernel && bind_pending(chan->vm, &bind_target, &bind_va);",
		"tlb = tgm_tlb_take(&chan->tlb, &chan->vm->tlb) || bind;"));
	EXPECT_TRUE(exec.before(
		"NVB06F_MEM_OP_D_OPERATION_L2_SYSMEM_INVALIDATE);",
		"NVB06F_MEM_OP_D_OPERATION_MMU_TLB_INVALIDATE); } PUSH_KICK(&chan->chan.push);"));
	EXPECT_TRUE(exec.before(
		"NVB06F_MEM_OP_D_OPERATION_MMU_TLB_INVALIDATE); } PUSH_KICK(&chan->chan.push);",
		"job.unprefetched = (l2_on() && host_written) || tlb;"));
	EXPECT_TRUE(Block(gpu(), "void chan_job_entry(")
			    .has("pushes[i].no_prefetch || (job->unprefetched && !i));"));
}

// A CPU IOSurfaceLock waits for the submissions that may write the surface: SubmitJob names each one once it has its
// seqno.
TEST(GM20BCoherencySource, AJobNamesTheWriterOnceTheSubmissionHasItsSeqno)
{
	const CodeText job = Block(client(), "IOReturn AGXDeviceUserClient::runJobUses(");
	EXPECT_TRUE(job.before("error = switch_gpu_chan_exec_events(",
			       "tgm_coherency_wrote(job.written[i], tgm_chan_uid(chan), seqno);"));
	EXPECT_TRUE(job.has("jobVisibility, releases, keptCount, &transaction);"));
}

TEST(GM20BInfoSource, GetInfoAnswersTheConnectionsCapabilities)
{
	const CodeText capabilities = Block(client(), "uint32_t switch_gpu_info_flags(void)");
	for (const char *capability : {"TegraGM20BInfoSubmitAddress", "TegraGM20BInfoCpuAccess"})
		EXPECT_TRUE(capabilities.has(capability));
	EXPECT_TRUE(Block(client(), "IOReturn AGXDeviceUserClient::getInfo(")
			    .has("out->flags = switch_gpu_info_flags();"));
	EXPECT_FALSE(clientCode().has("kInfoAwaitingUserspace"));
}

// The flags SubmitJob hands chan_exec are SwitchGpuJobVisibility.h's (switch_gpu_job_visibility_test), with a write into
// an IOSurface over its creator's memory, which IOSurface's own field names at the import, as one that ends External.
TEST(GM20BVisibilitySource, AJobWritingCreatorMemoryIsExternal)
{
	const CodeText imported = Block(client(), "IOReturn AGXDeviceUserClient::boImport(");
	EXPECT_TRUE(imported.before(
		"bo->surface = switch_gpu_surface_hold(",
		"bo->unlockedReads = switch_gpu_surface_client_memory(bo->surface) != 0;"));
	const CodeText job = Block(client(), "IOReturn AGXDeviceUserClient::runJobUses(");
	EXPECT_TRUE(job.before("if (access & TegraGM20BSurfaceWrite) {",
			       "unlockedWrites |= bo->unlockedReads;"));
	EXPECT_TRUE(Block(SourceText("../TegraGM20BSurfaceClient.cpp"),
			  "int switch_gpu_surface_client_memory(const OSObject *surface)")
			    .has("at<uint64_t>(surface, OriginalIOSurface::clientMemory) != 0"));
}

// FRAMECPU C6.1: a job writing a backing the display scans in place (a direct scanout slot) ends External, so its
// completion means its writes are in DRAM and the display may flip with no present job.
TEST(GM20BVisibilitySource, AJobWritingAScannedBackingIsExternal)
{
	const CodeText job(SourceText("../TegraGM20BUserClientJobSubmit.cpp"));
	EXPECT_TRUE(job.before("if (access & TegraGM20BSurfaceWrite) {",
			       "scanoutWrites |= switch_gpu_backing_scanned(bo->backing) != 0;"));
	EXPECT_TRUE(job.has(
		"switch_gpu_job_visibility(head.flags, events->signals(), unlockedWrites || "
		"scanoutWrites, hostWritten || transaction.host_written)"));
	EXPECT_TRUE(
		Block(SourceText("../SwitchGpuBacking.cpp"),
		      "extern \"C\" int switch_gpu_backing_scanned(")
			.has("__atomic_load_n(&backing->scanout_holds, __ATOMIC_SEQ_CST) != 0"));
}

// Only a connection's maps skip the CPU's L2 and TLB maintenance; every removal keeps it (TegraGM20BTlb.h).
TEST(GM20BTlbSource, MapsOrderedOnTheGpuRemovalsSynchronous)
{
	EXPECT_TRUE(clientCode().has("switch_gpu_vm_map_volatile(vm_, op.va"));
	EXPECT_EQ(clientCode().count("tgm_vm_map("), 0u);
	EXPECT_EQ(gpuCode().count("nvxnu_gpu_ordered_begin();"), 3u);
	const CodeText map = Block(gpu(), "int tgm_vm_map_raw(");
	EXPECT_TRUE(map.before("nvxnu_gpu_ordered_end(ordered_scope);", "nvif_vmm_raw_put("));
	const CodeText mixed = Block(gpu(), "int switch_gpu_vm_map_volatile(");
	EXPECT_TRUE(mixed.before("nvxnu_gpu_ordered_end(ordered_scope);", "nvif_vmm_raw_unmap("));
	EXPECT_TRUE(mixed.has("entry->va, entry->bytes, entry->shift"));
	const CodeText unmap = Block(gpu(), "void switch_gpu_vm_unmap(");
	EXPECT_FALSE(unmap.has("gpu_ordered"));
	EXPECT_TRUE(unmap.has("entry->va, entry->bytes, entry->shift"));
	EXPECT_TRUE(Block(gpu(), "void tgm_vm_del(").has("mmu_invalidate_all();"));
	// a cut changes no PTE; what it leaves pending is installed GPU-ordered, its failures put outside that scope
	EXPECT_FALSE(Block(gpu(), "int switch_gpu_vm_split(").has("nvif_vmm_raw"));
	const CodeText install = Block(gpu(), "int switch_gpu_vm_install(");
	EXPECT_TRUE(install.before("nvxnu_gpu_ordered_end(ordered_scope);", "nvif_vmm_raw_put("));
}

// Rail-gating (TegraGM20BRail.h): every other tgm_gpu.lock taker goes through tgm_gpu_lock(), which wakes a rail-gated GPU in
// the caller under the lock it took; the rail-up opens the gate before it clears `railed`, so a closer that has seen the
// GPU up never has its close undone (gm20b_rail_test's model).
TEST(GM20BRailSource, GpuLockWakesARailGatedGpu)
{
	EXPECT_EQ(gpuCode().count("mutex_lock(&tgm_gpu.lock);"), 6u)
		<< "gpu_lock, tgm_timestamp_ns, tgm_sleep, tgm_rail_down, tgm_rail_up, "
		   "tgm_rail_sc7_hold";
	EXPECT_TRUE(Block(gpu(), "noinline void tgm_gpu_lock(void)")
			    .has("if (unlikely(tgm_gpu.railed)) "
				 "tgm_rail_up_locked(__builtin_return_address(0));"));
	EXPECT_TRUE(Block(gpu(), "void tgm_rail_up_locked(const void *site)")
			    .before("gate_open();",
				    "__atomic_store_n(&tgm_gpu.railed, 0, __ATOMIC_SEQ_CST);"));
	// tgm_rail_up_locked takes these under the caller's tgm_gpu.lock: no tgm_gpu_lock() caller may hold one
	const auto calls = Occurrences(gpu(), "tgm_gpu_lock();");
	EXPECT_FALSE(calls.empty());
	for (const auto &call : calls)
		for (const char *lock : {"(&tgm_live_lock)", "(tgm_idle.clk)", "(&tgm_gate_lock)"})
			EXPECT_EQ(call.before.count(std::string("mutex_lock") + lock),
				  call.before.count(std::string("mutex_unlock") + lock))
				<< lock << " held at a tgm_gpu_lock() in " << call.head;
	// GPU time never backwards (gm20b_rail_test's model): the floor read before every power-down, PTIMER reseeded to it
	// after nvkm_timer_init, every time handed out clamped to it
	EXPECT_TRUE(Block(gpu(), "static void sleep_nvkm(void)")
			    .before("tgm_stamp_last = tgm_stamp_monotonic(tgm_stamp_last, "
				    "nvkm_timer_read(device->timer));",
				    "nvkm_device_fini(device, NVKM_SUSPEND)"));
	EXPECT_TRUE(Block(gpu(), "static void wake_m1(void)")
			    .before("ret = nvkm_subdev_init(&device->timer->subdev);",
				    "stamp_reseed(device);"));
	// a power-up's clock anchor (TegraGM20BClockABI.h) after its PTIMER is set, at boot and at every wake, before a channel
	// runs; a fence page handed out holds them all once zeroed
	EXPECT_TRUE(Block(gpu(), "static int m1(char *line, unsigned int len)")
			    .before("ret = nvkm_subdev_init(&device->timer->subdev);",
				    "tgm_clock_anchor_publish(device);"));
	EXPECT_TRUE(Block(gpu(), "static void stamp_reseed(struct nvkm_device *device)")
			    .before("nvkm_timer_alarm_trigger(device->timer);",
				    "tgm_clock_anchor_publish(device);"));
	EXPECT_TRUE(Block(gpu(), "static void wake_m1(void)")
			    .before("stamp_reseed(device);",
				    "nvkm_subdev_init(&device->clk->subdev);"));
	EXPECT_TRUE(Block(gpu(), "int tgm_fence_page_get(volatile uint32_t **va, uint64_t *pa)")
			    .before("memset((u8 *)tgm_fence_pages.va + i * TGM_FENCE_SLOT, 0, "
				    "PAGE_SIZE);",
				    "tgm_clock_anchors_fill(i);"));
	EXPECT_TRUE(Block(gpu(), "uint64_t tgm_timestamp_ns(void)")
			    .has("tgm_stamp_last = t = tgm_stamp_monotonic(tgm_stamp_last, t);"));
	const CodeText down = Block(gpu(), "int tgm_rail_down(void)");
	EXPECT_TRUE(down.before("sleep_gate();", "mutex_lock(&tgm_gpu.lock);"));
	EXPECT_TRUE(down.before("__atomic_store_n(&tgm_gpu.railed, 1, __ATOMIC_SEQ_CST);",
				"nvxnu_wakeup(&tgm_sc7_gate.shut);"));
}

// PMC_ENABLE's lock (TegraGM20BPmcEnable.h) is in nvkm's mc functions from M1, before nvkm's first init, and the
// kernel's own code never writes the register past it.
TEST(GM20BPmcSource, EveryPmcEnableChangeTakesItsLockFromM1)
{
	const CodeText m1 = Block(gpu(), "static int m1(char *line, unsigned int len)");
	EXPECT_TRUE(m1.before("tgm_gpu.device = device;", "tgm_pmc_own(device);"));
	EXPECT_TRUE(m1.before("tgm_pmc_own(device);", "nvkm_subdev_init(device->privring)"));
	const CodeText own =
		Block(SourceText("../gpu_pmc.c"), "void tgm_pmc_own(struct nvkm_device *device)");
	EXPECT_TRUE(own.has("pmc_device.enable = pmc_enable;"));
	EXPECT_TRUE(own.has("pmc_device.disable = pmc_disable;"));
	EXPECT_TRUE(own.has("if (mc->func->init == nv50_mc_init)"));
	EXPECT_FALSE(gpuCode().has(", 0x000200,"));
	const CodeText ring =
		Block(SourceText("../nouveau/nvkm/subdev/privring/gk20a.c"),
		      "gk20a_privring_init_privring_ring(struct nvkm_subdev *privring)");
	EXPECT_TRUE(ring.before("tgm_pmc_change(device, 0x20, 0);",
				"tgm_pmc_change(device, 0x20, 0x20);"));
	EXPECT_FALSE(ring.has("0x000200"));
}

TEST(GM20BSleepSource, SleepAndWakeOrder)
{
	const CodeText sleep = Block(gpu(), "void tgm_sleep(void)").after("case TGM_SLEEP_FULL:");
	EXPECT_TRUE(sleep.before("sleep_gate();", "sleep_fences();"));
	EXPECT_TRUE(sleep.before("sleep_fences();", "mutex_lock(&tgm_gpu.lock);"));
	EXPECT_TRUE(sleep.before("sleep_idle_off();", "sleep_nvkm();"));
	EXPECT_TRUE(sleep.before("sleep_nvkm();", "sleep_power_down();"));
	const CodeText nvkm = Block(gpu(), "static void sleep_nvkm(void)");
	EXPECT_TRUE(nvkm.before("nvif_client_suspend(&tgm_gpu.client, false)", "nvkm_ltc_flush("));
	// nvgpu_mm_suspend: the L2's comptag lines written back (FLUSH_L2_CLEAN_COMPTAGS) before the data flush
	EXPECT_TRUE(nvkm.before("nvkm_wr32(device, 0x07000c, 0x00000001);", "nvkm_ltc_flush("));
	EXPECT_TRUE(nvkm.before("nvkm_ltc_flush(", "nvkm_device_fini(device, NVKM_SUSPEND)"));
	EXPECT_TRUE(Block(gpu(), "static void sleep_power_down(void)")
			    .before("nvxnu_irq_suspend();", "tegra_gm20b_rail_gate("));
	// the boot's order: the GPCPLL relock (tgm_clk_raise) before ACR, the PMU and GR (tgm_m2_device_init); interrupts on before
	// nvkm's first init that may wait for one
	const CodeText wake = Block(gpu(), "void tgm_wake(void)").after("case TGM_SLEEP_FULL:");
	EXPECT_TRUE(wake.before("wake_power_up();", "wake_m1();"));
	EXPECT_TRUE(wake.before("wake_m1();", "wake_clock();"));
	EXPECT_TRUE(wake.before("wake_clock();", "wake_device();"));
	EXPECT_TRUE(wake.before("wake_device();", "wake_nvkm();"));
	EXPECT_TRUE(wake.before("wake_nvkm();", "wake_fence();"));
	EXPECT_TRUE(wake.before("wake_fence();", "wake_open();"));
	EXPECT_TRUE(Block(gpu(), "static void wake_power_up(void)")
			    .before("tegra_gm20b_rail_ungate(", "nvxnu_irq_resume();"));
	EXPECT_TRUE(
		Block(gpu(), "static void wake_clock(void)").has("clk_raise(line, sizeof(line))"));
	EXPECT_TRUE(
		Block(gpu(), "static void wake_device(void)").has("m2_device_init(device, &at)"));
}

// At M2 the FIFO has no user: nvkm makes its runqueues and runlists and registers its interrupt handler at the first
// channel (nvkm_fifo_oneinit), from its function table. What recover.c and sched.c take over goes into that table.
TEST(GM20BSchedSource, HooksGoWhereTheFifosOneinitReadsThem)
{
	const CodeText recover = Block(SourceText("../recover.c"),
				       "void tgm_recover_init(struct nvkm_device *device)");
	EXPECT_TRUE(recover.has("func->intr = fifo_intr;"));
	EXPECT_TRUE(recover.has("func->runq = &ctxsw.runq;"));
	EXPECT_TRUE(recover.has("func->chan.func = &ctxsw.chan;"));
	EXPECT_TRUE(recover.has("func->intr_ctxsw_timeout = ctxsw_tick;"));
	const std::string sched = SourceText("../sched.c");
	EXPECT_TRUE(Block(sched, "void tgm_sched_init(struct nvkm_device *device)")
			    .has("tgm_fifo_func(fifo)->runl_ctor = sched_runl_ctor;"));
	EXPECT_TRUE(Block(sched, "static int sched_runl_ctor(struct nvkm_fifo *fifo)")
			    .has("interleave(fifo->engine.subdev.device, runl);"));
}

// A submission that writes a block-linear IOSurface under a CPU write lock keeps the lock's rows before it runs, with the
// connection's lock dropped and before the active-submit gate's channel entry (the copy engine's wait sleeps).
// The push profile's stamps (debug kernels): one rule (switch_gpu_prof_stamped) decides the room a profiled job takes, the
// pushes that stamp and the classes read back, so the compositor stamps only where its job already waits for idle; and
// every window says per owner what the stamps cost.
TEST(GM20BPushScanSource, OneRuleDecidesAProfiledJobsStamps)
{
	EXPECT_TRUE(Block(gpu(), "static void chan_submission_room(")
			    .has("stamps += switch_gpu_prof_stamped(chan->critical, &pushes[i], i, "
				 "count, (*prof)->cls[i]);"));
	EXPECT_TRUE(
		Block(gpu(), "static inline __attribute__((always_inline)) void chan_job_entry(")
			.has("if (prof && chan->prof_cls[i] != TGM_PC_SKIP) {"));
	const CodeText &body = gpuCode();
	EXPECT_TRUE(body.before(
		"const int stamped = switch_gpu_prof_stamped(chan->critical, &pushes[i], i, count, "
		"prof->cls[i]);",
		"last = switch_gpu_job_emit(pushes, count, chan->submitted, &emitter);"));
	EXPECT_TRUE(
		body.has("__atomic_fetch_add(&chan->prof_span, end - start, __ATOMIC_RELAXED);"));
	EXPECT_TRUE(body.has("nvxnu_stats(\"tegra-gm20b: gpuprof cost %s:%s\", o[i].name, line);"));
}

// gpu.path.resource_ready: a GPU write under a CPU write lock keeps the lock's rows for the merge only when its
// submission asks before its job is kicked (TegraGM20BViews.h tgm_views_job_writes). SubmitJob asks once its uses are
// resolved; nothing else calls the per-surface hook, so no path can skip the list, as SubmitJob did once raw moved onto
// it (57b71739d).
TEST(GM20BViewsSource, AGpuWriteUnderACpuWriteLockIsMergedNotHeldBack)
{
	const CodeText job =
		Block(client(),
		      "IOReturn AGXDeviceUserClient::runJobUses(const SwitchGpuJobRecord &record,");
	EXPECT_TRUE(job.before("job.written[writtenCount++] = bo->coh;",
			       "tgm_views_job_writes(job.written, writtenCount);"));
	EXPECT_TRUE(job.before("tgm_views_job_writes(job.written, writtenCount);",
			       "switch_gpu_chan_submit_enter(chan);"));
	EXPECT_TRUE(job.before("tgm_views_job_writes(job.written, writtenCount);",
			       "switch_gpu_chan_exec_events("));
	EXPECT_EQ(clientCode().count("tgm_views_gpu_writes("), 0u);
}

// The PBDMA's ACQUIRE interrupt is gpu_acquire.c's: the timeout goes off in the PBDMA before the bit is cleared (so the
// same acquire cannot raise it again), and nvkm's handler, which would kill the waiter, never sees it.
TEST(GM20BAcquireSource, TheAcquireInterruptIsTakenBeforeNvkmsHandler)
{
	const CodeText runq =
		Block(SourceText("../recover.c"),
		      "static bool runq_intr(struct nvkm_runq *runq, struct nvkm_runl *runl)");
	EXPECT_TRUE(
		runq.before("tgm_acquire_intr(device, runq->id)",
			    "nvkm_wr32(device, PBDMA_INTR_0(runq->id), TGM_PBDMA_INTR_ACQUIRE)"));
	EXPECT_TRUE(runq.before("TGM_PBDMA_INTR_ACQUIRE)", "ctxsw.runq_nvkm->intr(runq, runl)"));
	const CodeText intr =
		Block(gpu(), "void tgm_acquire_intr(struct nvkm_device *device, int runq)");
	EXPECT_TRUE(intr.before("word & ~TGM_ACQUIRE_TIMEOUT_EN", "nvxnu_work_schedule(acq.work)"));
	EXPECT_TRUE(
		intr.has("nvkm_mask(device, PBDMA_INTR_EN_0(runq), TGM_PBDMA_INTR_ACQUIRE, 0)"));
	EXPECT_FALSE(intr.has("mutex_lock"));
	EXPECT_TRUE(Block(SourceText("../recover.c"), "void tgm_recover_suspend(void)")
			    .has("tgm_acquire_suspend();"));
}

// Every channel's RAMFC carries the timeout before its first submission (the subchannel binds), and a channel whose
// timeout an interrupt turned off gets it back only while it is idle and off the PBDMA, as a level change's timeslice.
TEST(GM20BAcquireSource, EveryChannelIsArmedBeforeItsFirstKickAndReArmedWhenIdle)
{
	const CodeText create = Block(gpu(), "static int chan_new(");
	EXPECT_TRUE(create.before("chan->chid = args.chan.chid;",
				  "ret = tgm_chan_ramfc_open(chan, critical);"));
	EXPECT_TRUE(
		create.before("ret = tgm_chan_ramfc_open(chan, critical);", "PUSH_KICK(push);"));
	EXPECT_TRUE(Block(gpu(), "int tgm_chan_ramfc_open(struct tgm_chan *chan, int critical)")
			    .has("tgm_sched_ramfc(tgm_gpu.device, chan->chid, ramfc_due_write, "
				 "&due, 1)"));
	EXPECT_TRUE(Block(gpu(), "static void ramfc_due_write(void *arg)")
			    .has("tgm_acquire_arm(due->chid);"));
	EXPECT_TRUE(Block(gpu(), "void tgm_chan_ramfc_due(struct tgm_chan *chan)")
			    .has("tgm_sched_ramfc(tgm_gpu.device, chan->chid, ramfc_due_write, "
				 "&due, 0)"));
	EXPECT_EQ(gpuCode().count("tgm_chan_ramfc_due(chan);"), 2u);
}

// A late wait and an acquire timeout blame by what the candidates' RAMFC says they wait on, and a check that finds a
// channel parked past its timeout with no interrupt says so.
TEST(GM20BAcquireSource, TheHangRuleReadsWhatEachCandidateWaitsOn)
{
	const CodeText recover = Block(gpu(), "enum tgm_hang tgm_hang_recover(struct tgm_chan "
					      "*late, const char *who, u32 seqno, u64 waited_ns)");
	EXPECT_TRUE(recover.has("candidate_wait(&waits, chan, &source)"));
	EXPECT_TRUE(recover.has("kind = candidate_wait(&waits, chans[i], &source);"));
	EXPECT_TRUE(
		recover.has("c[i].victim = kind == TGM_WAITS_VICTIM || kind == TGM_WAITS_UNREAD;"));
	EXPECT_TRUE(recover.has("tgm_acquire_unfired(chans[i], now);"));
}

// Nouveau puts the SW engine on the runlist with id 15 and no status register: PFIFO_ENG_STATUS(15) (0x0026b8)
// PRI-times out, and the GPU's bus error panics. Every per-engine status read is behind a context or a GR check.
TEST(GM20BSchedSource, EngineStatusReadsSkipTheSoftwareEngine)
{
	for (const char *file : {"../sched.c", "../recover.c", "../recover_engine.c"}) {
		const auto reads = Occurrences(SourceText(file), "PFIFO_ENG_STATUS(engn->id)");
		EXPECT_FALSE(reads.empty()) << file;
		for (const auto &read : reads) {
			// nvkm's engine functions with a context decode, which the SW engine's (gf100_engn_sw) lacks
			if (read.head.has(
				    "static int engn_cxid(struct nvkm_engn *engn, bool *cgid)"))
				continue;
			const CodeText guard = read.before.afterLast("nvkm_runl_foreach_engn");
			EXPECT_TRUE(guard.has("engn->func->cxid") || guard.has("NVKM_ENGINE_GR"))
				<< file << ": " << guard;
		}
	}
}

// With interrupts off XNU formats an os_log line only through the kernel printf into the message buffer, which has no
// privacy annotations: "%{public}s" prints "{public}s" and drops nvkm's report (the GR and MMU fault lines).
TEST(GM20BShimSource, InterruptPassReportsUsePlainFormats)
{
	const CodeText report = Block(SourceText("../shim/xnu_glue.cpp"),
				      "static void nvxnu_report(const char *line)");
	EXPECT_TRUE(report.has("os_log_error(OS_LOG_DEFAULT, \"%s\", line)"));
	EXPECT_FALSE(report.has("%{"));
}

// A channel made in a slot continues its predecessor's fence sequence, so every cursor the interrupt pass compares with
// the completed seqno starts at that fence, and the idle clock's tgm_busy words for the slot start there too.
TEST(GM20BChannelSource, SeqnoCursorsStartAtTheSlotsFence)
{
	const CodeText create = Block(gpu(), "static int chan_new(");
	EXPECT_TRUE(create.has("chan->submitted = READ_ONCE(*fence_cpu);"));
	for (const char *cursor : {"stat_done", "woken_done", "progress_done", "seen_done"})
		EXPECT_TRUE(create.has(std::string("chan->") + cursor + " = chan->submitted;"))
			<< cursor;
	EXPECT_TRUE(Block(gpu(), "static int chan_new(")
			    .has("switch_gpu_busy_open(&tgm_busy, chan->chid, chan->submitted);"));
}

// nvkm_chan_error preempts from the interrupt pass with interrupts off under the channel's spinlock: the wrapped preempt
// decides by tgm_preempt_how and waits for a preempt in flight only in thread context.
TEST(GM20BSchedSource, ThePreemptWaitsOnlyWithInterruptsOn)
{
	const CodeText preempt = Block(SourceText("../sched.c"),
				       "static void sched_chan_preempt(struct nvkm_chan *chan)");
	EXPECT_TRUE(preempt.has("switch (tgm_preempt_how(irqs_disabled(), pending)) {"));
	EXPECT_TRUE(
		preempt.before("case TGM_PREEMPT_RECOVER: nvkm_runl_rc_cgrp(chan->cgrp); return;",
			       "case TGM_PREEMPT_AFTER_WAIT:"));
	EXPECT_TRUE(preempt.before("case TGM_PREEMPT_AFTER_WAIT:", "nvkm_wait_msec("));
	EXPECT_TRUE(preempt.before("nvkm_wait_msec(", "case TGM_PREEMPT_NOW:"));
	EXPECT_EQ(preempt.count("nvkm_wait_msec("), 1u);
}

// Linux's work items, as kits/SwitchLinux's calls (whose host test holds that a call never runs twice at once): nvkm's
// shim allocates them there, a flush runs one still queued, and irqs_disabled() answers.
TEST(GM20BShimSource, WorkItemsKeepLinuxSemantics)
{
	const CodeText nvxnu(SourceText("../shim/include/nvxnu.h"));
	EXPECT_TRUE(nvxnu.has("return switch_linux_call_alloc(fn, arg);"));
	EXPECT_TRUE(nvxnu.has("return switch_linux_irqs_disabled() ? 0 : 1;"));
	const CodeText os(SourceText("../shim/include/nvif/os.h"));
	EXPECT_TRUE(os.has("static inline bool flush_work(struct work_struct *w) { return "
			   "switch_linux_call_flush(w->thread_call, nvxnu_work_trampoline, w); }"));
	EXPECT_TRUE(os.has("#define irqs_disabled() (!nvxnu_irqs_enabled())"));
}

// Rail-down stops what reaches the GPU from callouts as system sleep does: the deadline's holds released while the GPU
// is up and its callouts stopped before the fences drain, back at the rail-up; a context-switch tick's work stopped once
// interrupts are off.
TEST(GM20BRailSource, RailDownStopsTheGpuCallouts)
{
	const CodeText down = Block(gpu(), "int tgm_rail_down(void)");
	EXPECT_TRUE(down.before("sleep_gate();", "tgm_deadline_suspend();"));
	EXPECT_TRUE(down.before("tgm_deadline_suspend();", "sleep_fences();"));
	EXPECT_TRUE(Block(gpu(), "void tgm_rail_up_locked(const void *site)")
			    .before("__atomic_store_n(&tgm_gpu.railed, 0, __ATOMIC_SEQ_CST);",
				    "tgm_deadline_resume();"));
	const CodeText power = Block(gpu(), "static void sleep_power_down(void)");
	EXPECT_TRUE(power.before("nvxnu_irq_suspend();", "tgm_recover_suspend();"));
	EXPECT_TRUE(power.before("tgm_recover_suspend();", "tegra_gm20b_rail_gate("));
}

// IOGPUDeviceSetDisplayParams is scalar method 0x21 in iOS 17's IOGPU.framework and IOConnectTrap2 index 8 in iOS 27's;
// XNU answers a trap index with no trap behind it kIOReturnSuccess, so the device connection serves index 8 and refuses
// every other by name instead of letting it pass unseen.
TEST(GM20BDeviceSource, DisplayParamsArriveByMethodAndByTrap)
{
	const std::string client = SourceText("../TegraGM20BDeviceClient.cpp");
	EXPECT_TRUE(Block(client, "IOExternalTrap "
				  "*TegraGM20BDeviceUserClient::getTargetAndTrapForIndex(")
			    .has("*targetP = this; if (index == kSetDisplayParamsTrap) return "
				 "&displayParams; switch_refusal_k("));
	EXPECT_TRUE(CodeText(client).has("constexpr UInt32 kSetDisplayParamsTrap = 8;"));
	EXPECT_TRUE(Block(client, "IOReturn TegraGM20BDeviceUserClient::setDisplayParams(")
			    .has("storeDeadline(reinterpret_cast<uint64_t>(nextVblank), "
				 "reinterpret_cast<uint64_t>(interval));"));
	EXPECT_TRUE(
		CodeText(client).has("case kSetDisplayParams: if (arguments->scalarInputCount != "
				     "2) return kIOReturnBadArgument; storeDeadline("));
}

// A hang's line reads the hung submission's pushes through the user client that claimed the channel's space, and only
// that: the kernel's copiers (the tiling move, the copy engine's plane copies) claim theirs with their own state, which
// is no AGXDeviceUserClient. A live channel's client is not freed under it: hang recovery describes channels on the
// live list under tgm_live_lock, and the client's free() takes every channel off it before it frees its lock.
TEST(GM20BHangSource, OnlyAConnectionsPushesAreRead)
{
	EXPECT_TRUE(Block(gpu(), "void tgm_chan_describe(")
			    .has("CHAN_PUSH_BYTES, chan->vm->connection, note, sizeof(note));"));
	EXPECT_TRUE(Block(client(), "bool AGXDeviceUserClient::setUp(")
			    .has("tgm_vm_claim_connection(vm_, this);"));
	EXPECT_FALSE(clientCode().has("tgm_vm_claim("));
	// The kernel copy queues' spaces are claimed by their producers' state, never as a connection's.
	EXPECT_TRUE(Block(SourceText("../SwitchGpuKqueue.h"), "bool openSpace(")
			    .has("tgm_vm_claim(vm, owner);"));
	const std::string relayout = SourceText("../TegraGM20BRelayout.cpp");
	EXPECT_TRUE(CodeText(relayout).has("r.space.openSpace(kShape, &gRelayout)"));
	EXPECT_FALSE(CodeText(relayout).has("tgm_vm_claim_connection("));
	const std::string copy = SourceText("../TegraGM20BCopy.cpp");
	EXPECT_TRUE(Block(copy, "bool openSpace()").has("queue.space.openSpace(kShape, &queue)"));
	EXPECT_FALSE(CodeText(copy).has("tgm_vm_claim_connection("));
	EXPECT_TRUE(Block(client(), "void AGXDeviceUserClient::free()")
			    .before("drainResources();", "IOLockFree(lock_);"));
	EXPECT_TRUE(Block(client(), "void AGXDeviceUserClient::drainResources()")
			    .before("for (auto &chan : chans_) tgm_chan_del(&chan);",
				    "thread_call_cancel_wait(retireCall_);"));
	EXPECT_TRUE(Block(gpu(), "void tgm_chan_del(struct tgm_chan **pchan)")
			    .before("mutex_lock(&tgm_live_lock); list_del(&chan->live);",
				    "chan_free(chan);"));
}

// The tiling move and the copy engine's plane copies follow the surface's running writers through tgm_relayout_waits:
// acquires in their own push (TegraGM20BRelayout.h tgm_relayout_follow), a refusal for one they cannot acquire on, never a
// CPU wait. The kernel's own client channels' fences sit in the fence view's last slot, which every space maps and the
// connections' allocator never hands out, so a copy can acquire on the copy engine's writes.
TEST(GM20BRelayoutSource, KernelCopiesOrderOnTheGpu)
{
	const std::string relayout = SourceText("../TegraGM20BRelayout.cpp");
	const CodeText waits = Block(relayout, "extern \"C\" uint32_t tgm_relayout_waits(");
	EXPECT_TRUE(waits.has("nwaits = tgm_relayout_follow(self, running, fenceVa, n, waits, "
			      "nwaits, &refused);"));
	EXPECT_TRUE(
		waits.has("\"gpu-order\", \"writer-unordered\", \"a kernel copy cannot acquire"));
	EXPECT_TRUE(waits.has("return TGM_RELAYOUT_WAIT_FAILED;"));
	EXPECT_FALSE(waits.has("tgm_writer_wait("));
	const CodeText move = Block(relayout, "int move(");
	EXPECT_TRUE(
		move.has("tgm_relayout_waits(r.space.vm, tgm_chan_uid(chan), writers, nwriters, "
			 "waits, 0)"));
	// its wait is late on the writer an acquire sits on, so recovery finds that writer, not the copy behind it
	EXPECT_TRUE(move.has("acquired[i] = {waits[i].uid, waits[i].seqno};"));
	EXPECT_TRUE(move.has("tgm_copy_wait(tgm_chan_uid(chan), seqno, acquired, nwaits);"));
	EXPECT_FALSE(move.has("tgm_writer_wait("));
	const CodeText late = Block(gpu(), "static struct tgm_chan *late_writer(");
	EXPECT_TRUE(late.has("(s32)(tgm_chan_completed(chan) - acquired[i].seqno) < 0"));
	EXPECT_TRUE(late.has("*late_seqno = seqno; return live_by_uid(uid);"));
	EXPECT_TRUE(
		Block(gpu(), "static void writer_wait(")
			.has("chan = late_writer(uid, seqno, acquired, nacquired, &late); if "
			     "(chan) verdict = tgm_hang_recover(chan, \"IOSurface lock\", late, "));
	EXPECT_TRUE(CodeText(relayout).has(
		"{\"relayout\", TGM_FENCE_KERNEL_RELAYOUT, kSlots, kPushVa, 0, 1}"));
	EXPECT_TRUE(Block(SourceText("../SwitchGpuKqueue.h"), "bool openSpace(")
			    .has("tgm_fence_kernel(shape.fenceSlot, &fenceCpu, &fencePage, "
				 "&fenceOffset)"));
	const CodeText copy(SourceText("../TegraGM20BCopy.cpp"));
	EXPECT_TRUE(copy.has("switch_gpu_backing_dependencies(&transaction, queue.dependencies,"));
	EXPECT_TRUE(copy.has("queue.events.captureDependencies(queue.dependencies, count)"));
	EXPECT_TRUE(copy.has("switch_gpu_chan_exec_visibility("));
	EXPECT_FALSE(copy.has("tgm_writer_wait("));
	EXPECT_TRUE(copy.has("{\"ce-copy\", TGM_FENCE_KERNEL_COPY, Jobs, PushVa, 1, 0}"));
	const CodeText &fences = gpuCode();
	EXPECT_TRUE(fences.has("#define TGM_FENCE_PAGES (TGM_FENCE_USER_PAGES + 1)"));
	EXPECT_TRUE(Block(gpu(), "int tgm_fence_page_get(volatile uint32_t **va, uint64_t *pa)")
			    .has("i = (tgm_fence_pages.next + n) % TGM_FENCE_USER_PAGES;"));
	EXPECT_TRUE(Block(gpu(), "int tgm_fence_kernel(")
			    .has("TGM_FENCE_USER_PAGES * TGM_FENCE_SLOT + which * sizeof(struct "
				 "TegraGM20BAbiFence);"));
	EXPECT_TRUE(Block(gpu(), "int tgm_writer_fence_va(").has("off < TGM_FENCE_BYTES"));
}

TEST(GM20BSurfaceIdentitySource, ImportedSurfaceIdentityOutlivesLegacyCoherency)
{
	const CodeText imported = Block(client(), "IOReturn AGXDeviceUserClient::boImport(");
	EXPECT_TRUE(
		imported.before("bo->surface = switch_gpu_surface_hold(", "tgm_coherency_add("));
	const CodeText released = Block(client(), "void AGXDeviceUserClient::releaseBo(Bo *bo)");
	EXPECT_TRUE(
		released.before("tgm_coherency_put(bo->coh);", "OSSafeReleaseNULL(bo->surface);"));
}

// A held command buffer's writer promise (TegraGM20BMethodWriterPromise) is the connection's: method 34 reaches it under
// the connection lock with the submit gate open, it names writes of the connection's IOSurface imports only, and each
// release from the CPU reaches a parked GPU reader through the write-combined fence page.
TEST(GM20BJobSource, AWriterPromiseIsTheConnectionsAndEndsWithIt)
{
	const CodeText run = Block(client(), "IOReturn AGXDeviceUserClient::run(uint32_t method,");
	EXPECT_TRUE(run.has(
		"case TegraGM20BMethodWriterPromise: wantIn = sizeof(TegraGM20BAbiWriterPromise); "
		"wantOut = sizeof(TegraGM20BAbiWriterPromiseReply);"));
	EXPECT_TRUE(run.has("submitGate_.closed() ? kIOReturnNotOpen : writerPromise("));
	const std::string promises = SourceText("../TegraGM20BUserClientPromise.cpp");
	const CodeText promise =
		Block(promises, "IOReturn AGXDeviceUserClient::writerPromise(const "
				"TegraGM20BAbiWriterPromise *in,");
	EXPECT_TRUE(promise.has("if (!bo || !bo->backing || !bo->surface || uses[i].access != "
				"TegraGM20BSurfaceWrite"));
	EXPECT_FALSE(promise.has("switch_gpu_promise_release("));
	const CodeText release =
		Block(promises,
		      "void AGXDeviceUserClient::releasePromise(uint32_t promise, bool abandoned)");
	EXPECT_TRUE(release.before(
		"tgm_promise_abandon(offset, value);",
		"switch_gpu_promise_release(&promises_[promise], promiseWord(promise))"));
	EXPECT_TRUE(release.before(
		"switch_gpu_promise_release(&promises_[promise], promiseWord(promise))",
		"__asm__ volatile(\"dsb sy\" ::: \"memory\");"));
	EXPECT_TRUE(Block(client(), "void AGXDeviceUserClient::drainResources()")
			    .before("releasePromises(true);", "tgm_chan_del(&chan);"));
}

// A direct present whose producer still runs at the offer has no job (SwitchGpuPresentWatch.h): the producer's own
// interrupt pass makes it due with plain loads, the pass's end delivers it with interrupts on, and a kill, a hidden
// producer and the deadline go to the work item. The arm comes before the offer's own look.
TEST(GM20BPresentSource, AWatchedPresentIsCompletedByItsProducersInterruptPass)
{
	const CodeText fenced = Block(gpu(), "static inline void present_watch_fenced(");
	EXPECT_TRUE(fenced.has("switch_gpu_present_watch_fenced_any(&tgm_present.watch, chan->id, "
			       "tgm_chan_completed(chan), &matched)"));
	EXPECT_FALSE(fenced.has("mutex_lock"));
	EXPECT_FALSE(fenced.has("finish_visible"));
	const CodeText killed =
		Block(gpu(), "int tgm_chan_killed(struct nvif_event *event, void *repv, u32 repc)");
	EXPECT_TRUE(killed.before(
		"__atomic_store_n(chan->fence_cpu, chan->submitted, __ATOMIC_RELEASE);",
		"present_watch_fenced(chan, 1);"));
	const CodeText pass =
		Block(SourceText("../shim/xnu_glue.cpp"),
		      "void gpuInterrupt(OSObject *, IOInterruptEventSource *source, int)");
	EXPECT_TRUE(pass.before("nvxnu_poll_irqs();", "tgm_present_pass();"));
	EXPECT_TRUE(pass.before("tgm_present_pass();", "wakeAfterPass();"));
	const CodeText deliver = Block(gpu(), "void tgm_present_pass(void)");
	EXPECT_TRUE(deliver.has("switch_gpu_present_watch_take_visible("));
	// the pass's own thread: nothing may wait for a GPU interrupt
	EXPECT_TRUE(deliver.has("switch_gpu_present_tokens_finish_ready("));
	EXPECT_FALSE(deliver.has("finish_visible"));
	EXPECT_FALSE(deliver.has("_wait("));
	EXPECT_FALSE(Block(SourceText("../SwitchGpuPresentTokens.h"),
			   "switch_gpu_present_tokens_finish_ready(")
			     .has("token_wait"));
	EXPECT_TRUE(deliver.has("nvxnu_work_schedule(tgm_present.watch_work);"));
	EXPECT_FALSE(deliver.has("mutex_lock"));
	const CodeText arm = Block(
		gpu(), "int tgm_present_watch_dependencies(void (*done)(void *, int), void *arg,");
	EXPECT_TRUE(arm.before("switch_gpu_present_watch_arm_many(",
			       "mutex_unlock(&tgm_present.lock);"));
	EXPECT_TRUE(arm.before("gate_leave();",
			       "switch_gpu_present_watch_check(&tgm_present.watch, watch_gen, "
			       "watch_tokens, watch_count, present_physical)"));
	const char *const ask = "tgm_chan_need_tokens(watch_tokens, watch_count);";
	EXPECT_TRUE(arm.before("switch_gpu_present_watch_arm_many(", ask));
	EXPECT_TRUE(arm.before("gate_leave();", ask));
	EXPECT_TRUE(arm.before("tgm_present_promise_watch(&kept);",
			       "switch_gpu_present_watch_promise("));
	// the job that keeps a watched promise takes its place, retained before the watch is handed over
	const CodeText kept = Block(SourceText("../gpu_promise.c"),
				    "void tgm_present_promise_kept(uint32_t offset, uint32_t "
				    "value, const struct switch_gpu_backing_token *producer)");
	EXPECT_TRUE(kept.before("!switch_gpu_backing_token_retain(producer)",
				"switch_gpu_present_watch_kept("));
	EXPECT_TRUE(kept.before("switch_gpu_present_watch_rearm(&tgm_present.watch);",
				"tgm_chan_need_tokens(producer, 1);"));
	EXPECT_TRUE(kept.before("tgm_chan_need_tokens(producer, 1);",
				"switch_gpu_backing_token_physical_status(producer, 0)"));
	EXPECT_TRUE(arm.before("tgm_chan_need_tokens(",
			       "switch_gpu_present_watch_check(&tgm_present.watch, watch_gen, "
			       "watch_tokens, watch_count, present_physical)"));
	EXPECT_FALSE(arm.has("tgm_chan_need_uid("));
	EXPECT_TRUE(
		arm.has("nvxnu_work_schedule_after(tgm_present.deadline_work, TGM_PRESENT_NS);"));
	const CodeText outstanding = Block(gpu(), "int tgm_present_outstanding(const void *arg)");
	EXPECT_TRUE(outstanding.has("switch_gpu_present_watch_armed(&tgm_present.watch)"));
	// the deadline never delivers a watch: it follows the work an asked interrupt is behind, or panics for a lost one
	const CodeText deadline = Block(gpu(), "void tgm_present_deadline(void *arg)");
	EXPECT_FALSE(deadline.has("present_watch_work("));
	EXPECT_TRUE(
		deadline.before("switch_gpu_backing_token_physical_status(&watch_tokens[i], 0);",
				"mutex_lock(&tgm_live_lock);"));
	EXPECT_TRUE(deadline.before("if (watch_late(&late_token, watched, age, &waited, &seqno)) {",
				    "tgm_hang_recover("));
	const CodeText late = Block(gpu(), "static int watch_late(");
	EXPECT_TRUE(late.before("switch_gpu_quiet_interrupt(", "nvxnu_panic("));
	EXPECT_TRUE(late.has("if (now - tgm_present.late < TGM_PRESENT_WAKE_GRACE_NS) {"));
	EXPECT_FALSE(late.has("watch_claim"));
	EXPECT_FALSE(late.has("watch_work"));
	EXPECT_TRUE(
		Block(gpu(), "int tgm_present_watch_dependencies(").has("tgm_present.late = 0;"));
	const CodeText del = Block(gpu(), "void tgm_chan_del(struct tgm_chan **pchan)");
	EXPECT_TRUE(del.before("chan_abandon(chan);", "present_watch_fenced(chan, 1);"));
	EXPECT_TRUE(del.before("present_watch_fenced(chan, 1);", "list_del(&chan->live);"));
	EXPECT_TRUE(deadline.has(
		"if (tgm_present.pending && (s32)(tgm_chan_completed(tgm_present.chan) - "
		"tgm_present.pending) >= 0)"));
	const CodeText offer = Block(client(), "extern \"C\" int tegra_gm20b_scanout_copy(");
	EXPECT_TRUE(offer.has("ret = watch && !nreads ? presentWatched() : 1;"));
	EXPECT_TRUE(offer.has("ret = watch ? presentWatched() : 1;"));
}

// A context-switch timeout on the present's channel resets the copy engine and copies the frame again on the GPU: the cut
// present's completion waits for the redo, so the display never gets a partial-frame status and never copies on the CPU.
// Scrub jobs a reset cut go back dirty.
TEST(GM20BPresentSource, ACopyEngineResetRedoesThePresentOnTheGpu)
{
	const CodeText fenced = Block(gpu(), "static inline void present_fenced(");
	EXPECT_TRUE(fenced.before(
		"redo = pending; if (__atomic_compare_exchange_n(&tgm_present.redo, &redo, 0,",
		"tgm_present.fenced_due = 1;"));
	EXPECT_TRUE(
		fenced.before("__atomic_load_n(&tgm_present.redoing, __ATOMIC_ACQUIRE)) return;",
			      "redo = pending; if ("));
	EXPECT_TRUE(fenced.before("nvxnu_work_schedule(tgm_present.redo_work); return; }",
				  "tgm_present.fenced_due = 1;"));
	const CodeText redo = Block(gpu(), "void tgm_present_redo_work(");
	EXPECT_TRUE(redo.before("tgm_present_push_copy(push);", "chan_fence_volatile(chan);"));
	EXPECT_TRUE(Block(gpu(), "static void gate_open(").has("tgm_present_redo_kick();"));
	EXPECT_FALSE(redo.has("done(arg"));
	EXPECT_FALSE(redo.has("tgm_present.done"));
	EXPECT_FALSE(redo.has("SEMAPHORED"));
	// a redo with no room in the ring goes to the hang rule and is tried again, never a panic
	EXPECT_TRUE(redo.before(
		"tgm_chan_stalled(chan, \"present redo: push buffer still full after 1 s\"",
		"nvxnu_work_schedule(tgm_present.redo_work);"));
	EXPECT_FALSE(redo.has("nvxnu_panic("));
	const CodeText mark = Block(gpu(), "int tgm_present_redo(");
	EXPECT_TRUE(mark.before("__atomic_store_n(&tgm_present.redo, pending, __ATOMIC_SEQ_CST);",
				"if (__atomic_load_n(&tgm_present.pending, __ATOMIC_SEQ_CST) == "
				"pending) return 1;"));
	EXPECT_TRUE(Block(gpu(), "static inline void scrub_fenced(")
			    .has("nvxnu_pool_free_dirty(tgm_scrub.job[slot].pa, "
				 "tgm_scrub.job[slot].bytes);"));
	const CodeText decide = Block(gpu(), "enum tgm_ctxsw tgm_ctxsw_decide(");
	// a stuck kernel channel never panics here: tgm_kernel_stall_locked decides, and marks the redo before the reset
	EXPECT_TRUE(decide.has(
		"if (chan->kernel && (verdict == TGM_CTXSW_REDO || verdict == TGM_CTXSW_FATAL)) "
		"verdict = tgm_kernel_stall_locked(chan, engine_name, verdict == TGM_CTXSW_FATAL, "
		"0, 1, &killed);"));
	EXPECT_FALSE(decide.has("nvxnu_panic("));
	const CodeText stall = Block(gpu(), "enum tgm_ctxsw tgm_kernel_stall_locked(");
	EXPECT_TRUE(stall.before("if (verdict == TGM_CTXSW_FATAL) {", "*killed = kill_clients("));
	EXPECT_TRUE(stall.before("*killed = kill_clients(", "tgm_ctxsw_redo_mark(chan);"));
	EXPECT_TRUE(stall.has("if (!fatal_ok) return verdict;"));
	// an idle channel the engine cannot switch off has no drain to wait for: a kernel one is reset, a client's killed,
	// at the first tick
	EXPECT_TRUE(decide.has(
		"chan_stuck_for(chan, now, tgm_stuck_window(gr)) || chan_unowned(chan)"));
	// progress after the reset: the redone work landing, or with nothing to redo any job completing
	EXPECT_TRUE(Block(gpu(), "void tgm_ctxsw_redo_mark(")
			    .has("chan->ctxsw_reset_after = marked ? chan->submitted : "
				 "tgm_chan_completed(chan);"));
}

// gpu.recover.kernel_second_timeout's hook takes each cut of the present as a tick takes a stuck kernel channel, the second
// before the first's redo landed, and gets the hardware-failure verdict back instead of its panic
TEST(GM20BPresentSource, TheCopyResetHookCutsTwiceThroughTheKernelStallRuleAndNeverPanics)
{
	const std::string hook = SourceText("../gpu_copy_reset.c");
	EXPECT_TRUE(Block(hook, "static void copy_reset_mark(")
			    .has("again == AGAIN_REDO, 0, 0, &killed);"));
	EXPECT_FALSE(CodeText(hook).has("nvxnu_panic("));
	const CodeText redo = Block(gpu(), "void tgm_present_redo_work(");
	EXPECT_TRUE(redo.before("#if TGM_STATS again = (u32)tgm_copy_reset_again_take(); #endif",
				"chan_push_wait("));
	EXPECT_TRUE(redo.before("if (again) { PUSH_MTHD(push, NV906F, NON_STALL_INTERRUPT, 0);",
				"chan_fence_volatile(chan);"));
}

// A context load an engine is stuck in again after a reset under it: a client's is that client's alone and kills it, a
// kernel channel's goes to the kernel stall rule, whose only panic is the engine that did not come back
TEST(GM20BRecoverSource, ASecondStuckLoadKillsTheClientOrGoesToTheKernelStallRule)
{
	const CodeText load = Block(gpu(), "static enum tgm_ctxsw load_verdict(");
	EXPECT_TRUE(load.before("if (chan->kernel) return tgm_kernel_stall_locked(chan, engine, "
				"verdict == TGM_CTXSW_FATAL, 1, 1, &killed);",
				"tgm_chan_isolate(chan, report, nvxnu_nanotime());"));
	EXPECT_TRUE(load.before("tgm_chan_isolate(chan, report, nvxnu_nanotime());",
				"return TGM_CTXSW_KILL;"));
	EXPECT_FALSE(load.has("nvxnu_panic("));
	const CodeText stall = Block(gpu(), "enum tgm_ctxsw tgm_kernel_stall_locked(");
	EXPECT_EQ(stall.count("nvxnu_panic("), 1u);
	EXPECT_TRUE(
		stall.has("hardware failure: %s did not come back after a reset with no client "));
	EXPECT_TRUE(stall.has("chan->ctxsw_clean_ns = n ? 0 : now;"));
	EXPECT_TRUE(Block(gpu(), "static u32 kill_clients(")
			    .has("tgm_chan_isolate(clients[i], report, now); "
				 "tgm_recover_fault(tgm_gpu.device, (int)clients[i]->chid);"));
}

TEST(GM20BPresentSource, TypedProducerValidationAndReleaseNeverRunUnderTheIrqEventLock)
{
	const CodeText irq = Block(gpu(), "static inline void present_fenced(");
	EXPECT_TRUE(irq.has("tgm_present.fenced_due = 1;"));
	EXPECT_FALSE(irq.has("switch_gpu_present_tokens_finish"));
	EXPECT_TRUE(Block(gpu(), "void tgm_present_pass(void)")
			    .has("nvxnu_work_schedule(tgm_present.completion_work);"));
	const CodeText complete = Block(gpu(), "static void present_typed_complete(");
	EXPECT_TRUE(complete.before("mutex_unlock(&tgm_present.lock);",
				    "switch_gpu_present_tokens_finish_visible("));
	EXPECT_TRUE(complete.before("switch_gpu_present_tokens_finish_visible(",
				    "&tgm_present.typed_pending, 0"));
	const CodeText copy = Block(gpu(), "static int present_copy(");
	EXPECT_TRUE(copy.before(
		"tgm_backing_dependencies(chan, tgm_present.tokens.tokens, dependency_count)",
		"if (dst_va) tgm_present_push_copy(push);"));
	EXPECT_TRUE(copy.before("&tgm_present.typed_pending, 1", "switch_gpu_chan_fence_sealed("));
	EXPECT_TRUE(copy.has("dependency_count * 5"));
	EXPECT_TRUE(copy.has("push->cur = push_start"));
}

TEST(GM20BPresentSource, ExactFrameSkipsLiveChannelEnumerationEvenWithoutGpuTokens)
{
	const CodeText copy = Block(gpu(), "static int present_copy(");
	EXPECT_FALSE(copy.has("exact"));
	EXPECT_FALSE(copy.has("list_for_each_entry"));
	const CodeText exact = Block(gpu(), "int tgm_present_copy_dependencies(");
	EXPECT_TRUE(exact.has("dependencies, dependency_count);"));
	const CodeText scanout = Block(client(), "extern \"C\" int tegra_gm20b_scanout_copy(");
	EXPECT_TRUE(scanout.has("gPresentSnapshot.add(sourceBacking)"));
	EXPECT_TRUE(scanout.has("gPresentSnapshot.tokens, gPresentSnapshot.count"));
}

TEST(GM20BVmSource, MappingSerializesPerAddressSpaceWithSuspendAdmission)
{
	for (const char *signature :
	     {"int tgm_vm_map_raw(", "int tgm_vm_unmap_pages(", "int switch_gpu_vm_map_volatile(",
	      "void switch_gpu_vm_unmap(", "int tgm_vm_kmap_raw("}) {
		const CodeText body = Block(gpu(), signature);
		EXPECT_TRUE(body.before("gate_enter();", "mutex_lock(&vm->map_lock);"));
		EXPECT_TRUE(body.before("mutex_unlock(&vm->map_lock);", "gate_leave();"));
		EXPECT_FALSE(body.has("gpu_lock();"));
		EXPECT_FALSE(body.has("mem_prepare_big_views("));
	}
	const CodeText memory = Block(gpu(), "static int mem_new(");
	EXPECT_TRUE(memory.before("gpu_lock();", "mem_prepare_big_views(mem);"));
	EXPECT_TRUE(memory.before("mem_prepare_big_views(mem);", "mutex_unlock(&tgm_gpu.lock);"));
	EXPECT_TRUE(Block(gpu(), "void tgm_mem_del(").has("gpu_lock();"));
	EXPECT_TRUE(Block(gpu(), "void tgm_vm_del(").has("gpu_lock();"));
}

// A command buffer's batches are one SubmitJob (TegraGM20BPushFence): its uses are owned against the seqno of the
// job's own fence, after every internal one (switch_gpu_job_fences_test runs the emission order).
TEST(GM20BJobFencesSource, OneJobOwnsItsUsesUntilItsLastFence)
{
	const CodeText job = Block(client(), "IOReturn AGXDeviceUserClient::runJobUses(");
	EXPECT_TRUE(job.has("constexpr uint32_t kPushFlags = TegraGM20BPushNoPrefetch | "
			    "TegraGM20BPushIncomplete | TegraGM20BPushFence;"));
	EXPECT_TRUE(
		job.has("if (switch_gpu_job_fences(pushes, count, &fences)) return "
			"switch_gpu_refuse(why, SwitchGpuRefusal::Fences, kIOReturnBadArgument,"));
	EXPECT_TRUE(job.has("switch_gpu_chan_job_token(chan, fences, &completion);"));
	EXPECT_FALSE(job.has("switch_gpu_chan_next_token("));
	// resolved once: a written import is marked from what the uses resolved, not looked up again
	EXPECT_TRUE(job.has("job.written[writtenCount++] = bo->coh;"));
	EXPECT_TRUE(job.has("tgm_coherency_wrote(job.written[i], tgm_chan_uid(chan), seqno);"));
	EXPECT_FALSE(job.has("findAddress("));
	const CodeText emit = Block(gpu(), "int chan_exec_body(");
	const CodeText room = Block(gpu(), "static void chan_submission_room(");
	EXPECT_TRUE(
		room.has("*entries = count + 6 + stamps + job_fences + CHAN_WAKE_ENTRIES + 1;"));
	EXPECT_TRUE(
		room.has("SWITCH_GPU_FENCE_POINT_DWORDS * job_fences + CHAN_WAKE_DWORDS + 16 +"));
	EXPECT_TRUE(
		emit.has("last = switch_gpu_job_emit(pushes, count, chan->submitted, &emitter);"));
	EXPECT_TRUE(Block(client(), "uint32_t switch_gpu_info_flags(void)")
			    .has("TegraGM20BInfoPushFences;"));
}

// A use's handle is found by the index (switch_gpu_bo_index_test), never by walking the connection's BOs; every BO
// that joins bos_ joins the index, and every one that leaves it leaves the index.
TEST(GM20BBoIndexSource, EveryBoListChangeKeepsTheIndex)
{
	const CodeText find =
		Block(client(), "AGXDeviceUserClient::Bo *AGXDeviceUserClient::findBo(");
	EXPECT_TRUE(find.has("return boIndex_.find(handle);"));
	const std::string sources = SourceText("../TegraGM20BUserClientBo.cpp") +
				    SourceText("../TegraGM20BUserClientImport.cpp") +
				    SourceText("../SwitchGpuNativeImport.h") +
				    SourceText("../SwitchGpuClientRetirement.h");
	EXPECT_EQ(CodeText(sources).count("bos_ = bo;"),
		  CodeText(sources).count("boIndex_.insert(bo->handle, bo);"));
	EXPECT_EQ(CodeText(sources).count("boIndex_.reserve()"), 2u)
		<< "publishBo (BoAlloc, BoImport) and nativeSurfaceView";
	EXPECT_TRUE(Block(SourceText("../SwitchGpuClientRetirement.h"),
			  "IOReturn AGXDeviceUserClient::freeBo(Bo **p)")
			    .before("*p = bo->next;", "boIndex_.erase(bo->handle);"));
}

TEST(GM20BVisibilitySource, InternalFenceKeepsExecutionOrderAndExternalFencePublishesVisibleFirst)
{
	const CodeText fence = Block(gpu(), "static u32 chan_fence_job(");
	EXPECT_TRUE(fence.has("if (external && (l2_on() || chan->kernel))"));
	EXPECT_TRUE(fence.before("NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY",
				 "if (external == 1 && chan->visible_cpu)"));
	EXPECT_TRUE(
		fence.before("lower_32_bits(chan->visible_gpu)", "lower_32_bits(chan->fence_gpu)"));
	EXPECT_TRUE(fence.has("RELEASE_WFI, EN"));
	const CodeText exec = Block(gpu(), "int chan_exec_body(");
	EXPECT_TRUE(exec.has("if ((l2_on() && host_written) || tlb)"));
	EXPECT_TRUE(exec.before("NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY",
				"NVB06F_MEM_OP_D_OPERATION_L2_SYSMEM_INVALIDATE"));
	EXPECT_TRUE(exec.has(
		"*seqno = chan_fence_job(chan, external, chan->submitted + 1, last, quiet, "
		"releases, release_count, owned, waits);"));
	// A batch's internal fence orders execution only: no L2 flush, no visible release, no interrupt. Its kick is the
	// GPFIFO entry that keeps it between the batches; the job's own kick writes GP_PUT.
	const CodeText internal = Block(gpu(), "static void chan_job_fence(");
	EXPECT_TRUE(internal.has("RELEASE_WFI, EN"));
	EXPECT_FALSE(internal.has("L2_FLUSH_DIRTY"));
	EXPECT_FALSE(internal.has("visible_gpu"));
	EXPECT_FALSE(internal.has("NON_STALL_INTERRUPT"));
	EXPECT_FALSE(internal.has("defer_kick"));
	EXPECT_TRUE(internal.before("RELEASE_SIZE, 4BYTE));", "PUSH_KICK(push);"));
	EXPECT_TRUE(exec.before("chan->chan.defer_kick = true;", "last = switch_gpu_job_emit("));
	EXPECT_TRUE(fence.before("chan->chan.defer_kick = true;", "PUSH_KICK(push);"));
	EXPECT_TRUE(fence.before("PUSH_KICK(push);", "chan_doorbell(chan);"));
	EXPECT_TRUE(fence.before(
		"job = switch_gpu_jobs_kick_waits(&chan->jobs, first, next, chan->submit_ns, "
		"!announce, waits);",
		"PUSH_KICK(push);"));
	for (const char *signature : {"int tgm_vm_unmap_pages(", "void switch_gpu_vm_unmap("}) {
		const CodeText unmap = Block(gpu(), signature);
		EXPECT_TRUE(
			unmap.before("nvkm_ltc_flush(tgm_gpu.device->ltc)", "nvif_vmm_raw_unmap("));
	}
}

TEST(GM20BVisibilitySource, ColdGlobalInvalidationPreservesInternalDirtyData)
{
	const CodeText body = Block(SourceText("../nouveau/nvkm/subdev/ltc/base.c"),
				    "nvkm_ltc_invalidate(struct nvkm_ltc *ltc)");
	EXPECT_TRUE(body.before("mutex_lock(&ltc->mutex)", "ltc->func->flush(ltc)"));
	EXPECT_TRUE(body.before("ltc->func->flush(ltc)", "ltc->func->invalidate(ltc)"));
	EXPECT_TRUE(body.before("ltc->func->invalidate(ltc)", "mutex_unlock(&ltc->mutex)"));
}

// nvkm_runl_chan_get_chid returns with spinlocks held and interrupts off; nvkm_kmap takes ltc->mutex.
TEST(GM20BChannelSource, RamfcIsMappedOnlyAfterTheChannelLookupUnlocks)
{
	const CodeText lookup = Block(gpu(), "static struct nvkm_gpuobj *chan_inst(");
	EXPECT_TRUE(lookup.before("nvkm_runl_chan_get_chid(", "nvkm_chan_put(&nchan, flags)"));
	for (const char *signature : {"void tgm_ramfc_write(", "static int ramfc_read_waits("}) {
		const CodeText body = Block(gpu(), signature);
		EXPECT_TRUE(body.before("chan_inst(chid)", "nvkm_kmap("));
		EXPECT_TRUE(body.before("nvkm_kmap(", "nvkm_done("));
	}
	EXPECT_TRUE(Block(gpu(), "static void chan_timeslice(").has("tgm_ramfc_write("));
}

TEST(GM20BChannelSource, CopyChannelCreatesOnlyRequestedObjectsAndKeepsVerifiedRunlist)
{
	const CodeText create = Block(gpu(), "static int chan_new(");
	EXPECT_TRUE(create.has("NV_DEVICE_HOST_RUNLIST_ENGINES_GR"));
	EXPECT_TRUE(create.has("args.chan.runq = 0"));
	EXPECT_TRUE(create.has("i = switch_gpu_channel_first_object(copy_only); i < 5"));
	EXPECT_TRUE(create.has("switch_gpu_channel_bind_words(bind_words, classes, copy_only)"));
	EXPECT_TRUE(create.has("chan_push_wait(chan, bind_count)"));
	EXPECT_TRUE(create.has("PUSH_DATA(push, bind_words[i])"));
	const CodeText objects = create.between("i = switch_gpu_channel_first_object(copy_only)",
						"host->version = 0");
	EXPECT_TRUE(CodeText(objects).before("if (ret)", "goto fail"));
	EXPECT_TRUE(gpuCode().has("tgm_chan_new_copy(tgm_present.vm"));
	EXPECT_TRUE(gpuCode().has("tgm_chan_new_copy(tgm_scrub.vm"));
	EXPECT_TRUE(
		CodeText(SourceText("../SwitchGpuKqueue.h")).has("tgm_chan_new_copy(vm, owner"));
	for (const char *producer : {"../TegraGM20BCopy.cpp", "../TegraGM20BRelayout.cpp",
				     "../SwitchGpuCachedZero.cpp", "../SwitchGpuBridge.cpp"}) {
		EXPECT_TRUE(CodeText(SourceText(producer)).has("space.openChannel(kShape"));
		EXPECT_FALSE(CodeText(SourceText(producer)).has("tgm_chan_new_copy("));
	}
}

// Every channel's acquire on another channel's fence reads the address space's view of the fence pool: a space without
// it would refuse every cross-channel dependency for its life, so it is not made.
TEST(GM20BVmSource, AnAddressSpaceWithoutItsFenceViewIsNotMade)
{
	const CodeText make = Block(gpu(), "int tgm_vm_new(struct tgm_vm **pvm)");
	EXPECT_TRUE(make.before("ret = tgm_fences_map(vm);", "tgm_vm_del(&vm);"));
	EXPECT_FALSE(make.has("vm->fences_va = 0;"));
}

// A client unmaps a pool BO after its BoFree: the pages wait for that unmap, which releases them, and are not a leak.
TEST(GM20BPoolSource, FreedPoolPagesGoWithTheirLastMapping)
{
	EXPECT_FALSE(Block(client(), "void AGXDeviceUserClient::releaseBo(Bo *bo)")
			     .has("still mapped by its"));
	for (const char *signature : {"IOReturn AGXDeviceUserClient::unmapToken(",
				      "void AGXDeviceUserClient::releaseMappings()"})
		EXPECT_TRUE(Block(client(), signature).has("scheduleOrphanRelease();"));
}

// Pool allocations take zeroed pages only (nvxnu_pool_alloc_ready, chunks): the refusal rules count those.
TEST(GM20BPoolSource, PoolRefusalsCountOnlyZeroedFreePages)
{
	EXPECT_TRUE(Block(gpu(), "uint64_t tgm_pool_avail(void)").has("nvxnu_pool_ready_pages()"));
}

// A layout decision may move the surface on the copy engine and wait for it: the connection's lock and mutation gate
// are not held meanwhile, and what it uses is held for it.
TEST(GM20BLayoutSource, LayoutDecisionHoldsNeitherTheConnectionLockNorItsGate)
{
	const CodeText info = Block(client(), "IOReturn AGXDeviceUserClient::surfaceLayoutInfo(");
	EXPECT_TRUE(info.before("tgm_coh_hold_asker(coh);", "IOLockUnlock(lock_);"));
	EXPECT_TRUE(info.before("IOLockUnlock(lock_);", "tgm_coherency_layout(coh,"));
	EXPECT_TRUE(info.before("tgm_coh_put_asker(coh);", "IOLockLock(lock_);"));
	const CodeText run = Block(client(), "IOReturn AGXDeviceUserClient::run(");
	EXPECT_TRUE(run.has("const bool mutation = switch_gpu_method_closes_gate(method);"));
	const CodeText gated = Block(SourceText("../SwitchGpuGatedMethods.h"),
				     "static inline bool switch_gpu_method_closes_gate(");
	EXPECT_TRUE(gated.before("case TegraGM20BMethodSurfaceLayout:", "return false;"));
	EXPECT_TRUE(gated.before("case TegraGM20BMethodSurfaceLayoutInfo:", "return false;"));
}

// The asker's hold (gm20b_layout_ask_test) is the only reference the decision does not count as another user: it is
// taken once and put once, with no return between them.
TEST(GM20BLayoutSource, TheAskersHoldIsPutOnEveryPath)
{
	const CodeText info = Block(client(), "IOReturn AGXDeviceUserClient::surfaceLayoutInfo(");
	EXPECT_EQ(info.count("tgm_coh_hold_asker("), 1u);
	EXPECT_EQ(info.count("tgm_coh_put_asker("), 1u);
	EXPECT_FALSE(info.has("tgm_coh_hold("));
	EXPECT_FALSE(
		info.between("tgm_coh_hold_asker(coh);", "tgm_coh_put_asker(coh);").has("return"));
}

// nvgpu's gr_gm20b_init_fs_state writes ZROP's and CROP's settings whole with the L2 slice count (0x12006c,
// pri_ringmaster_enum_ltc), not the FBP count gk104's helper reads.
TEST(GM20BRopSource, ZropAndCropCountTheL2SlicesAsNvgpuDoes)
{
	const std::string gr = SourceText("../nouveau/nvkm/engine/gr/gm20b.c");
	const CodeText init = Block(gr, "gm20b_gr_init_rop_active_ltcs(struct gf100_gr *gr)");
	EXPECT_TRUE(init.has("nvkm_rd32(device, 0x12006c)"));
	EXPECT_TRUE(init.has("nvkm_wr32(device, 0x408850, ltc_count);"));
	EXPECT_TRUE(init.has("nvkm_wr32(device, 0x408958, ltc_count);"));
	EXPECT_TRUE(CodeText(gr).has(".init_rop_active_fbps = gm20b_gr_init_rop_active_ltcs,"));
}

TEST(GM20BParkedBoSource, LockOrderAndOffLockVmCalls)
{
	const CodeText run = Block(client(), "IOReturn AGXDeviceUserClient::run(");
	EXPECT_TRUE(run.before("return boPark(", "switch_gpu_method_closes_gate(method)"));
	EXPECT_TRUE(run.before("return boUnpark(", "switch_gpu_method_closes_gate(method)"));
	EXPECT_TRUE(run.before("return boFreeMethod(", "switch_gpu_method_closes_gate(method)"));
	EXPECT_TRUE(run.before("vmBindParkedRefusal(", "vmBind("));
	EXPECT_TRUE(run.has("cpuAcquireParkedRefusal("));

	const CodeText park = Block(client(), "IOReturn AGXDeviceUserClient::boPark(");
	EXPECT_TRUE(park.before("IOLockUnlock(lock_);", "switch_iokit_park_replace("));
	EXPECT_FALSE(park.between("IOLockLock(lock_);", "IOLockUnlock(lock_);")
			     .has("switch_iokit_park_replace("));

	const CodeText unpark = Block(client(), "IOReturn AGXDeviceUserClient::boUnpark(");
	EXPECT_TRUE(unpark.before("IOLockUnlock(lock_);", "switch_iokit_unpark_range("));
	EXPECT_FALSE(unpark.between("IOLockLock(lock_);", "IOLockUnlock(lock_);")
			     .has("switch_iokit_unpark_range("));
	EXPECT_TRUE(unpark.before("if (bo->parkState == SwitchGpuParkState::Reclaiming)",
				  "parkWaitStableLocked(in->handle)"));
	EXPECT_TRUE(unpark.before("parkWaitStableLocked(in->handle)",
				  "switch_gpu_park_begin_unpark(bo->parkState)"));
	const CodeText wait = unpark.between("parkWaitStableLocked(in->handle)",
					     "switch_gpu_park_begin_unpark(bo->parkState)");
	EXPECT_TRUE(wait.has("submitGate_.closed()"));
	EXPECT_TRUE(wait.has("findBo(in->handle)"));
	EXPECT_TRUE(wait.has("bo->freeing"));

	const CodeText freeMethod = Block(client(), "IOReturn AGXDeviceUserClient::boFreeMethod(");
	EXPECT_TRUE(freeMethod.before("IOLockUnlock(lock_);", "switch_iokit_drop_parked_range("));

	const CodeText work =
		Block(client(), "void spareWork(thread_call_param_t param, thread_call_param_t)");
	EXPECT_TRUE(work.before("reclaimParkedStep(", "IOLockLock(p->lock);"));

	const CodeText take = Block(client(), "Spare *spareTake(uint64_t bytes)");
	EXPECT_TRUE(take.before("IOLockUnlock(p->lock);", "parkedNotifyDemand("));
	EXPECT_TRUE(take.before("IOLockUnlock(p->lock);", "parkedHasAny()"));

	const CodeText stats = Block(client(), "extern \"C\" unsigned long tgm_owner_stats(");
	EXPECT_TRUE(stats.before("parkedStatsTake(", "IOLockLock(p->lock);"));
}

TEST(GM20BParkedBoSource, OverwriteMappingsAndXnuHelpersPreserveVmSafety)
{
	const CodeText map = Block(client(), "IOReturn AGXDeviceUserClient::mapToken(");
	EXPECT_TRUE(map.has("switch_iokit_map_fixed("));
	EXPECT_TRUE(map.has("switch_iokit_unmap_overwrite("));
	EXPECT_TRUE(map.has("mappingRefs"));
	EXPECT_TRUE(Block(client(), "IOReturn AGXDeviceUserClient::unmapToken(")
			    .has("switch_iokit_unmap_overwrite("));
	EXPECT_TRUE(Block(client(), "void AGXDeviceUserClient::releaseMappings()")
			    .has("switch_iokit_unmap_overwrite("));
	EXPECT_TRUE(Block(client(), "extern \"C\" uint32_t switch_gpu_info_flags(void)")
			    .has("TegraGM20BInfoBoPark"));
	const CodeText memType = Block(client(), "IOReturn AGXDeviceUserClient::memoryForType(");
	EXPECT_TRUE(memType.has("bo->parkState == SwitchGpuParkState::Live"));
	EXPECT_TRUE(memType.has("isBuffer ? kIOMapUnique : 0u"));

	const CodeText parkFn = Block(client(), "IOReturn AGXDeviceUserClient::boPark(");
	EXPECT_TRUE(parkFn.has("bo->mappingRefs != 0u"));
	EXPECT_TRUE(parkFn.has("backingParkable(bo->backing)"));
	EXPECT_FALSE(parkFn.has("switch_gpu_backing_member_freed("));
	EXPECT_FALSE(parkFn.has("switch_iokit_unpark_range("));

	const CodeText gpuBusy = Block(client(), "bool AGXDeviceUserClient::boGpuBusyLocked(");
	EXPECT_TRUE(gpuBusy.has("switch_gpu_mapping_pending("));
	EXPECT_TRUE(gpuBusy.has("liveBinds != bo->bindings"));

	const CodeText bindRefuse =
		Block(client(), "IOReturn AGXDeviceUserClient::vmBindParkedRefusal(");
	EXPECT_TRUE(bindRefuse.has("binds_[j].bo"));

	const CodeText reclaimFn = Block(client(), "bool AGXDeviceUserClient::reclaimParkedBo(");
	EXPECT_TRUE(reclaimFn.has("retirementStopped_"));
	EXPECT_TRUE(reclaimFn.has("switch_gpu_vm_unmap(vm_, &record->mapping)"));
	EXPECT_FALSE(reclaimFn.has("switch_gpu_mapping_mark_l2_clean("));
	EXPECT_TRUE(reclaimFn.before("passSubmitters(false)", "detachReclaimedLocked(still)"));

	const CodeText drain = Block(client(), "void AGXDeviceUserClient::drainParkedRanges()");
	EXPECT_FALSE(drain.has("IONew("));
	EXPECT_TRUE(drain.has("switch_iokit_drop_parked_range("));

	const std::string backingCpp = SourceText("../SwitchGpuBacking.cpp");
	const CodeText parkable =
		Block(backingCpp, "extern \"C\" int switch_gpu_backing_parkable(");
	EXPECT_TRUE(parkable.before("IOLockLock(lock);", "switch_gpu_backing_enter(backing)"));
	EXPECT_TRUE(parkable.has("switch_gpu_backing_access_status("));
	EXPECT_TRUE(parkable.has("backing->refs != 1u"));
	EXPECT_TRUE(parkable.has("backing->retiring"));
	EXPECT_TRUE(parkable.has("__atomic_load_n(&backing->mapped_engines, __ATOMIC_ACQUIRE)"));
	EXPECT_TRUE(parkable.has("__atomic_load_n(&backing->scanout_holds, __ATOMIC_ACQUIRE)"));

	const std::string parkCpp = SourceText("../../../kernel/switch/iokit/park_mapping.cpp");
	const CodeText replace = Block(parkCpp, "kern_return_t switch_iokit_park_replace(");
	EXPECT_TRUE(replace.before("switch_iokit_map_check_or_neuter(", "switch_vm_park_range("));
	EXPECT_TRUE(replace.before("buffer_mem->getRetainCount()", "switch_vm_park_range("));
	EXPECT_TRUE(replace.before("old_view->getRetainCount()", "switch_vm_park_range("));
	EXPECT_TRUE(replace.before("switch_vm_park_range(", "switch_vm_range_has_objects("));
	EXPECT_TRUE(
		replace.has("switch_iokit_map_neuter_after_park(old_map); old_map->release();"));

	const CodeText unparkHelper = Block(parkCpp, "IOReturn switch_iokit_unpark_range(");
	EXPECT_TRUE(unparkHelper.has("switch_vm_repark_matching("));
	EXPECT_TRUE(unparkHelper.has("desc_is_physical(desc)"));
	const CodeText fixedHelper = Block(parkCpp, "IOReturn switch_iokit_map_fixed(");
	EXPECT_TRUE(fixedHelper.has("switch_vm_unmap_matching("));
	EXPECT_TRUE(fixedHelper.has("desc_is_physical(desc)"));
	EXPECT_TRUE(fixedHelper.has("switch_iokit_map_is_unique(map)"));

	const std::string parkVm = SourceText("../../../kernel/switch/vm/park_vm.c");
	const CodeText vmCode(parkVm);
	EXPECT_FALSE(vmCode.has("entry_is_iokit_kernel"));
	EXPECT_FALSE(vmCode.has("vm_map_ilk_lock_shared"));
	EXPECT_TRUE(vmCode.has("VM_MAP_PAGE_MASK(map)"));
	EXPECT_TRUE(vmCode.has("vm_map_range_ex_lock("));
	EXPECT_TRUE(vmCode.has("vm_map_range_sh_lock("));
	EXPECT_TRUE(vmCode.has("VME_IS_SENTINEL("));
	EXPECT_TRUE(vmCode.has("vm_object_lock(obj)"));
	EXPECT_TRUE(vmCode.has("obj->shadowed"));
	EXPECT_TRUE(vmCode.has("obj->vo_copy == VM_OBJECT_NULL"));
	EXPECT_TRUE(vmCode.has("e->zero_wired_pages = FALSE;"));
	EXPECT_TRUE(vmCode.has("VME_OBJECT_SET(e, VM_OBJECT_NULL, false, 0)"));
	EXPECT_TRUE(vmCode.has("vm_map_iokit_unmapped_region("));
	EXPECT_TRUE(vmCode.has("VM_MAP_REMOVE_GAPS_FAIL"));
	EXPECT_TRUE(vmCode.has("vm_map_remove_and_unlock("));
	EXPECT_TRUE(vmCode.has("mach_memory_entry_from_port("));
}

TEST(GM20BParkedBoSource, ParkedDiagnosticsCompileOutOfRelease)
{
	for (const char *file : {"../TegraGM20BUserClientPark.cpp", "../SwitchGpuParkedPool.cpp"}) {
		const std::string src = SourceText(file);
		const CodeText release(Configured(src, "TGM_STATS", false));
		EXPECT_EQ(release.count("parkedStatsTake"), 0u);
		EXPECT_EQ(release.count("parkedStatUnpark"), 0u);
		EXPECT_EQ(release.count("unparkResident"), 0u);
		EXPECT_EQ(release.count("unparkReclaimed"), 0u);
		EXPECT_EQ(release.count("reclaimedCached"), 0u);
		EXPECT_EQ(release.count("reclaimedSystem"), 0u);
	}
}

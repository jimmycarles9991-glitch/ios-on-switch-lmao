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
		for (const char *unit : {"", "Pool", "Bo", "Channel", "Submit", "Dispatch",
					 "Present", "Job", "JobSubmit"})
			units += SourceText(std::string("../TegraGM20BUserClient") + unit + ".cpp");
		return units + SourceText("../TegraGM20BUserClientInternal.h") +
		       SourceText("../SwitchGpuClientRetirement.h");
	}();
	return source;
}

const std::string &gpu()
{
	static const std::string source = [] {
		std::string units;
		for (const char *unit :
		     {"bringup", "clock", "mem", "hang", "describe", "acquire", "stats",
		      "stats_tick", "chan", "sync", "exec", "present", "present_deadline",
		      "present_redo", "reload", "scrub", "power"})
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
	EXPECT_TRUE(clientCode().Has("#include \"TegraGM20BPoolPolicy.h\""));
	const CodeText refusal =
		Block(client(),
		      "const char *poolRefusal(uint64_t bytes, uint64_t held, bool compositor)");
	EXPECT_TRUE(refusal.Has("tgm_pool::refusal("));
	EXPECT_TRUE(refusal.Has("compositor);"));  // the compositor keeps nvkm's floor
	const CodeText alloc = Block(client(), "IOReturn AGXDeviceUserClient::boAlloc(");
	EXPECT_TRUE(alloc.Has(
		"refused = control ? nullptr : poolRefusal(in->bytes, pool_bytes_, critical_);"));
	EXPECT_TRUE(alloc.Has("tgm_pool::coherentSource(refused, bo->pool_va != nullptr)"));
	// A refusal does not fail the BO: no NoMemory between the tier check and the pool allocation.
	EXPECT_FALSE(alloc.Between("if (!ret && (in->flags & TegraGM20BBoCoherent)) {",
				   "if (!ret && bo->pool_va)")
			     .Has("kIOReturnNoMemory"));
	EXPECT_FALSE(clientCode().Has("GPU pool BoAlloc of %llu KiB refused"));
	// A system BO: a write-combined buffer of its own, mapped as itself. A physical view over its managed pages would set
	// their cache attribute through a device pager whose private pages reset it to cacheable whenever they are freed.
	EXPECT_TRUE(alloc.Between("} else if (!ret && !compressible) {", "if (!ret && bo->bmd) {")
			    .Has("if (system) options |= kIOMapWriteCombineCache;"));
	EXPECT_FALSE(alloc.Has(
		"kIOMemoryTypePhysical64) | kPoolCacheMode, TASK_NULL); IODelete(ranges"));
	EXPECT_TRUE(alloc.Has("if (!ret && bo->bmd) { bo->view = bo->backing_pages == bo->npages"));
}

TEST(GM20BPoolPolicySource, SystemBosGoAsCachedBosDo)
{
	const CodeText release = Block(client(), "void AGXDeviceUserClient::releaseBo(Bo *bo)");
	EXPECT_TRUE(release.Has("if (bo->pool_va && bo->view) orphan = IONew(PoolOrphan, "
				"1); if (orphan) {"));
	EXPECT_TRUE(release.Has("!bo->system && !(bo->flags & TegraGM20BBoContiguous) &&"));
	EXPECT_TRUE(release.Before("spareKeep(", "bo->bmd->release();"));
	EXPECT_FALSE(Block(client(), "void releaseOrphans()").Has("spareKeep"));
	// The kernel's mapping of a system BO is write-combined as its task's is: pushView reads it as a cached BO's.
	EXPECT_TRUE(Block(client(), "const uint32_t *AGXDeviceUserClient::pushView(")
			    .Has("if (hit) { if (hit->bo->pool_va) {"));
	EXPECT_TRUE(Block(client(), "void AGXDeviceUserClient::scanPushes(")
			    .Has("pushView(va, bytes, hit)"));
	EXPECT_TRUE(Block(client(), "int AGXDeviceUserClient::hungInterlocks(")
			    .Has("pushView(va[i], static_cast<uint64_t>(dwords[i]) * 4, hit)"));
}

// A system BO neither takes a spare nor gives its pages to the spares, which are cacheable: it is made at its own size.
// A cached BO's pages are in one state from every source: zero in DRAM, no line in the cacheable kernel alias
// (cleanZeros publishes KMA_ZERO; recycled spares become ready only after CE completion).
TEST(GM20BPoolPolicySource, SystemBosNeverUseSpares)
{
	const CodeText alloc = Block(client(), "IOReturn AGXDeviceUserClient::boAlloc(");
	EXPECT_TRUE(
		alloc.Has("if (!ret && !bo->pool_va && !compressible && !system && "
			  "!(in->flags & TegraGM20BBoContiguous)) spare = spareTake(in->bytes);"));
	EXPECT_TRUE(alloc.Has("(in->flags & TegraGM20BBoContiguous) || system ? kClasses : "
			      "classUp(in->bytes);"));
	EXPECT_TRUE(alloc.Has("cleanZeros(va, backing);"));
	EXPECT_TRUE(Block(client(), "Spare *spareMake(uint64_t bytes)")
			    .Has("cleanZeros(bmd->getBytesNoCopy(), bytes);"));
	const CodeText work =
		Block(client(), "void spareWork(thread_call_param_t param, thread_call_param_t)");
	EXPECT_TRUE(work.Has("switch_gpu_cached_zero(s->pas"));
	EXPECT_FALSE(work.Has("cleanZeros(s->bmd"));
	const CodeText completed = Block(client(), "void spareZeroed(void *context, int result)");
	EXPECT_TRUE(completed.Before("const bool keep = !result && !memoryPressure();",
				     "p->ready[c] = s;"));
	const CodeText zeros = Block(client(), "void cleanZeros(void *va, uint64_t bytes)");
	EXPECT_TRUE(zeros.Has("switch_dcache_clean_invalidate(va, bytes);"));
	EXPECT_FALSE(zeros.Has("bzero"));
	// A pool view's pages go back only once no task maps the view; a cached BO's buffer is a spare only once nothing
	// else holds it.
	const CodeText orphans = Block(client(), "void releaseOrphans()");
	EXPECT_TRUE(orphans.Before("if (orphan->view->getRetainCount() > 1) {",
				   "orphan->view->release();"));
	EXPECT_TRUE(orphans.Has("orphan->zero &&"));
	EXPECT_TRUE(Block(client(), "bool spareKeep(IOBufferMemoryDescriptor *bmd, "
				    "uint64_t *pas, uint64_t bytes,")
			    .Has("bmd->getRetainCount() == 1"));
	const CodeText free = Block(client(), "IOReturn AGXDeviceUserClient::boFree(");
	EXPECT_TRUE(free.Before("unbindOverlaps(binds_[i].va, binds_[i].range, bo);",
				"queueZombie(bo);"));
}

// The import costs (the "imports" and "slow import" lines, the refused import's region) are the debug kernel's alone:
// a release kernel neither reads the thread's CPU time nor walks the task's map.
TEST(GM20BImportCostSource, ImportCostsAreDebugOnly)
{
	EXPECT_TRUE(
		CodeText(Configured(gpu(), "TGM_STATS", true)).Has("thread_get_runtime_self()"));
	EXPECT_EQ(CodeText(Configured(gpu(), "TGM_STATS", false)).Count("thread_get_runtime_self"),
		  0u);
	EXPECT_TRUE(Block(Configured(SourceText("../TegraGM20BGpu.h"), "TGM_STATS", false),
			  "static inline uint64_t tgm_stats_cpu_ns(void)")
			    .Has("return 0;"));
	const CodeText import = Block(client(), "IOReturn AGXDeviceUserClient::boImport(");
	EXPECT_TRUE(import.Has("#if TGM_STATS describeRegion(task_, in->address, region, "
			       "sizeof(region)); #endif"));
	EXPECT_TRUE(import.Has("tgm_mem_new_waited(vm_, bo->pas, bo->npages, &bo->mem, "
			       "&cost.lock_ns)"));
	EXPECT_TRUE(Block(gpu(), "static int mem_new(")
			    .Has("asked = stats_on() && lock_wait_ns ? nvxnu_nanotime() : 0;"));
}

TEST(GM20BPoolPolicySource, StatsNameThePoolHolders)
{
	const CodeText &stats = gpuCode();
	EXPECT_TRUE(
		stats.Has("tgm_pool_breakdown(&pool_clients, &pool_orphan_bytes, &pool_orphans, "
			  "&pool_comp_bytes, &pool_comps, &pool_system_bytes, &pool_systems);"));
	EXPECT_TRUE(stats.Has(
		"pool %lu/%llu MiB (compositor %llu, clients \" \"%llu, orphans %llu in"));
	EXPECT_TRUE(stats.Has("compressible %llu MiB in %llu;"));
}

// A Compressible BO is pool chunks under the pool's tiers, never a view a task could map, and its chunks go back only once
// its GPU memory object (and with it its comptags) is gone.
TEST(GM20BPoolPolicySource, CompressibleBosArePoolChunksNoTaskMaps)
{
	const CodeText alloc = Block(client(), "IOReturn AGXDeviceUserClient::boAlloc(");
	const CodeText chunks = alloc.Between("if (!ret && compressible) {",
					      "if (!ret && (in->flags & TegraGM20BBoCoherent)) {");
	EXPECT_TRUE(
		chunks.Has("refused = compressibleRefusal(in->bytes, pool_bytes_, critical_);"));
	EXPECT_TRUE(Block(client(), "const char *compressibleRefusal(uint64_t bytes, "
				    "uint64_t held, bool compositor)")
			    .Has("tgm_pool::compressibleRefusal(bytes, held, avail, limits, "
				 "compositor)"));
	EXPECT_TRUE(chunks.Has("takeChunks(in->bytes, bo->pas)"));
	EXPECT_TRUE(alloc.Has("} else if (!ret && !compressible) {"));
	EXPECT_FALSE(Block(client(), "IOReturn takeChunks(uint64_t bytes, uint64_t *pas)")
			     .Has("poolZeroNow()"));
	const CodeText release = Block(client(), "void AGXDeviceUserClient::releaseBo(Bo *bo)");
	EXPECT_TRUE(release.Before("tgm_mem_del(&bo->mem);",
				   "tgm_pool_free_chunks(bo->pas, bo->npages);"));
	EXPECT_TRUE(Block(client(), "IOReturn AGXDeviceUserClient::clientMemoryForType(")
			    .Has("bo && bo->view"));
	EXPECT_TRUE(Block(SourceText("../shim/linux.c"),
			  "int nvxnu_pool_alloc_chunks(unsigned long count, unsigned "
			  "long long *pas)")
			    .Has("tgm_comp_pick_chunks(pool_map, pool_dirty, pool_npages,"));
}

TEST(GM20BPoolScrubSource, NoCpuClearOfTheReservedPool)
{
	const CodeText init = Block(SourceText("../shim/xnu_glue.cpp"),
				    "extern \"C\" int nvxnu_pool_init(int *zeroed)");
	EXPECT_FALSE(
		init.Between("switch_ram_gpu_pool(&start, &end)", "IOBufferMemoryDescriptor *md")
			.Has("memset"));
	EXPECT_TRUE(init.Has("*zeroed = 0;"));
	EXPECT_TRUE(init.Has("*zeroed = 1;"));  // the fallback still zeroes its own pages
	EXPECT_TRUE(Block(SourceText("../shim/linux.c"), "int nvxnu_pool_setup(void)")
			    .Has("bitmap_set(pool_dirty, 0, pool_npages)"));
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
		EXPECT_TRUE(Block(shim, signature).Has("pool_zero_dirty(")) << signature;
	const CodeText chunks = Block(
		shim, "int nvxnu_pool_alloc_chunks(unsigned long count, unsigned long long *pas)");
	EXPECT_TRUE(chunks.Has("dirty > 0"));
	EXPECT_FALSE(chunks.Has("pool_zero_dirty("));
	EXPECT_FALSE(Block(shim, "void *nvxnu_pool_alloc_ready(").Has("pool_zero_dirty("));
	const CodeText zero = Block(
		shim, "static void pool_zero_dirty(unsigned long first, unsigned long n, int all)");
	EXPECT_TRUE(zero.Has("test_bit(first + k, pool_dirty)"));
	EXPECT_TRUE(zero.Has("bitmap_clear(pool_dirty, first, n)"));
	// A plain free marks pages dirty; only a zeroed free (CPU or scrubber) marks them clean.
	EXPECT_TRUE(Block(shim, "static void pool_free(unsigned long first, unsigned long n)")
			    .Has("bitmap_set(pool_dirty, first, n)"));
	const CodeText claim = Block(shim, "unsigned long nvxnu_pool_claim_dirty(unsigned "
					   "long max_bytes, unsigned long long *pa)");
	EXPECT_TRUE(claim.Has("pool_dirty[w] & ~pool_map[w]"));
	EXPECT_TRUE(claim.Has(
		"bitmap_set(pool_map, first, n)"));  // claimed ranges are allocated: nobody takes them
}

// A process maps whole VM pages (16 KiB, four pool pages): every pool BO and fence slot it can map is whole VM pages,
// so its mapping never reaches another BO's, another connection's fence or nvkm's own memory.
TEST(GM20BPoolIsolationSource, CpuMappedPoolMemoryIsWholeVmPages)
{
	const CodeText alloc =
		Block(gpu(), "void *tgm_pool_alloc(uint64_t bytes, uint64_t align, uint64_t *pa)");
	EXPECT_TRUE(alloc.Has("nvxnu_pool_alloc_clean(pool_bo_bytes(bytes), "
			      "max_t(u64, align, nvxnu_vm_page_size()), &h)"));
	EXPECT_TRUE(Block(gpu(), "void tgm_pool_free(void *va, uint64_t bytes, uint64_t pa)")
			    .Has("pool_bo_bytes(bytes)"));
	EXPECT_TRUE(Block(gpu(), "void tgm_pool_free_zeroed(void *va, uint64_t bytes, uint64_t pa)")
			    .Has("bytes = pool_bo_bytes(bytes)"));
	EXPECT_TRUE(Block(SourceText("../shim/linux.c"),
			  "void *nvxnu_pool_alloc_clean(unsigned long size, unsigned long "
			  "align, unsigned long long *pa)")
			    .Has("align >> PAGE_SHIFT"));
	EXPECT_TRUE(Block(gpu(), "int tgm_fence_page_get(volatile uint32_t **va, uint64_t *pa)")
			    .Has("i * TGM_FENCE_SLOT"));
	EXPECT_TRUE(clientCode().Has("IOAddressRange range = {fence_pa_, "
				     "static_cast<IOByteCount>(PAGE_SIZE)};"));
}

TEST(GM20BPoolScrubSource, ScrubberFreesOnlyFromItsFence)
{
	const CodeText fenced = Block(gpu(), "void scrub_fenced(struct tgm_chan *chan)");
	EXPECT_TRUE(fenced.Has("nvxnu_pool_free_zeroed("));
	EXPECT_TRUE(fenced.Has("tgm_chan_completed(chan)"));
	EXPECT_EQ(gpuCode().Count("nvxnu_pool_free_zeroed("), 2u)
		<< "tgm_pool_free_zeroed's and the scrubber's";
	const CodeText submit = Block(gpu(), "static int scrub_submit(u64 pa, u64 bytes)");
	EXPECT_TRUE(submit.Has("tgm_fill_setup(push->cur, 0)"));
	EXPECT_TRUE(submit.Has(
		"chan_fence(chan)"));  // WFI, FB flush, L2 flush, release, nonstall interrupt
	EXPECT_TRUE(submit.Has("nvxnu_work_schedule_after(tgm_scrub.deadline, SCRUB_NS)"));
	EXPECT_FALSE(submit.Has("nvxnu_idle"));  // no CPU wait for the copy engine
	EXPECT_TRUE(Block(gpu(), "static void scrub_deadline(void *arg)").Has("nvxnu_panic("));
	EXPECT_TRUE(
		Block(gpu(),
		      "static int chan_nonstall(struct nvif_event *event, void *repv, u32 repc)")
			.Has("chan->kernel == CHAN_SCRUB"));
}

// T-v5a: the scrubber stuck on PBDMA0 with every engine idle went to a last resort that reset only busy or switching
// engines, and the next check panicked. A hang check that finds a kernel channel stuck marks its cut work to be redone
// (the scrubber's ranges go back dirty) before any reset, resets its own engines, then every engine with the client
// channels on them killed; only a reset of every engine with no client context resident that did not free it panics.
TEST(GM20BPoolScrubSource, AStuckKernelChannelHasItsEnginesResetAndItsWorkRedone)
{
	EXPECT_TRUE(Block(gpu(), "enum tgm_hang tgm_hang_recover(")
			    .Has("if (verdict == TGM_HANG_KERNEL) return "
				 "tgm_kernel_hang_locked(chans[b], late, who, seqno, waited_ns, "
				 "moved_ns);"));
	EXPECT_FALSE(gpuCode().Has("tgm_hang_last_resort"));
	const CodeText hang = Block(gpu(), "enum tgm_hang tgm_kernel_hang_locked(");
	EXPECT_EQ(hang.Count("nvxnu_panic("), 1u);
	EXPECT_TRUE(hang.Has("hardware failure: the engines of %s's channel %u "));
	EXPECT_TRUE(hang.Has("did not come back after a reset with no client context resident: "));
	EXPECT_TRUE(
		hang.Before("if (step == TGM_KERNEL_HANG_FATAL) {", "tgm_ctxsw_redo_mark(stuck);"));
	EXPECT_TRUE(hang.Before("if (step == TGM_KERNEL_HANG_WAIT) return TGM_HANG_RECOVERING;",
				"tgm_recover_kernel(tgm_gpu.device,"))
		<< "a waiter within a step's gap takes no step";
	EXPECT_TRUE(hang.Before("if (step == TGM_KERNEL_HANG_ALL) {", "kill_clients(stuck,"));
	EXPECT_TRUE(
		hang.Before("tgm_ctxsw_redo_mark(stuck);", "tgm_recover_kernel(tgm_gpu.device,"));
	EXPECT_TRUE(
		Block(gpu(), "void tgm_ctxsw_redo_mark(").Has("tgm_scrub_redo(chan->submitted)"));
	const CodeText resets = Block(SourceText("../recover_held.c"),
				      "static int reset_for(struct nvkm_device *device,");
	EXPECT_TRUE(resets.Before("tgm_chid_cut(held[i]);",
				  "tgm_engine_reset_locked(runl, engn, chid, why);"))
		<< "another kernel channel's context on a reset engine is redone too";
	EXPECT_TRUE(resets.Has("if (!all && !tgm_kernel_hang_resets("));
	EXPECT_FALSE(resets.Has("PFIFO_ENG_BUSY")) << "an idle engine is reset as a busy one is";
}

TEST(GM20BPoolScrubSource, FreedPoolBosGoToTheGpuFirst)
{
	EXPECT_TRUE(Block(client(), "bool poolZeroLater(void *va, uint64_t pa, uint64_t bytes)")
			    .Before("tgm_pool_scrub(pa, bytes)", "spares()"));
}

// The user client reads pushes only through the header gm20b_push_scan_test tests, and profiles only what it read.
TEST(GM20BPushScanSource, KernelUsesTheHeader)
{
	EXPECT_TRUE(clientCode().Has("#include \"TegraGM20BPushScan.h\""));
	EXPECT_FALSE(clientCode().Has("void pushMethod("));
	EXPECT_FALSE(clientCode().Has("void pushParse("));
	const CodeText scan = Block(client(), "void AGXDeviceUserClient::scanPushes(");
	EXPECT_TRUE(scan.Has("scanPush(c, p, whole, nullptr, 0)"));
	EXPECT_TRUE(scan.Has("prof->cls[i] = cls;"));
	// A stamp only after a push the profile's rule left a class (switch_gpu_prof_stamped), only on a profiled submission.
	EXPECT_TRUE(Block(gpu(), "void chan_job_entry(")
			    .Has("if (prof && chan->prof_cls[i] != TGM_PC_SKIP) {"));
	// A job with internal fences is profiled whole: read at its last seqno, against its first's start stamp.
	EXPECT_TRUE(Block(gpu(), "static void chan_submission_room(")
			    .Has("|| count > TGM_PROF_MAX || profiled))"));
	EXPECT_TRUE(Block(gpu(), "int chan_exec_body(").Has("chan->submitted + 1 + fences); /*"));
	EXPECT_TRUE(Block(gpu(), "static inline void stats_fences(")
			    .Has("prof_collect(chan, first, fs->start.ns, st->end.ns)"));
}

// Direct scanout's fence-only present (NativeMobileDisplay, TEGRA_SCANOUT_COPY_FENCE_ONLY) launches no copy.
TEST(GM20BPresentSource, FenceOnlyPresentHasNoCopy)
{
	EXPECT_TRUE(Block(gpu(), "static int present_copy(")
			    .Has("if (dst_va) tgm_present_push_copy(push);"));
}

// gpu.poster_targets reads each process's largest colour target, which the stats' push scan records (debug kernels).
// Every submission path takes the scan before its kick and hands its profile to the kick, so raw's SubmitJob jobs
// (57b71739d) are read as its Exec jobs were; the scan's counts are atomic, as SubmitJob runs without the connection lock.
TEST(GM20BPushScanSource, EverySubmissionPathScansItsPushes)
{
	const CodeText job =
		Block(client(),
		      "IOReturn AGXDeviceUserClient::runJobUses(const SwitchGpuJobRecord &record,");
	EXPECT_TRUE(
		job.Before("const tgm_prof *profile = scanSubmission(pushes, count, prof, true);",
			   "switch_gpu_chan_exec_events("));
	EXPECT_TRUE(job.Has("&seqno, profile, jobVisibility, releases, keptCount);"));
	const CodeText exec = Block(
		client(),
		"IOReturn AGXDeviceUserClient::exec(uint32_t channel, TegraGM20BAbiPush *input,");
	EXPECT_TRUE(
		exec.Before("const tgm_prof *profile = scanSubmission(pushes, count, prof, false);",
			    "switch_gpu_chan_exec_events("));
	EXPECT_TRUE(exec.Has("&seqno, profile, jobVisibility, nullptr, 0);"));
	EXPECT_EQ(clientCode().Count("scanPushes(pushes, count,"), 1u);
	EXPECT_TRUE(Block(client(), "const tgm_prof *AGXDeviceUserClient::scanSubmission(")
			    .Has("__atomic_fetch_add(&pushSubmits_, 1u, __ATOMIC_RELAXED) % "
				 "kScanEvery"));
}

// A copying present flushes the L2 once, before the copy reads the frame; its copy writes around the L2, so its fence
// does not flush again. A fence-only present (direct scanout of a frame the GPU drew through the L2) keeps the fence's.
TEST(GM20BPresentSource, OneL2FlushPerPresent)
{
	const CodeText copy = Block(gpu(), "static int present_copy(");
	EXPECT_TRUE(
		Block(gpu(), "void tgm_present_push_copy(")
			.Before("NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY", "tgm_copy_texels("));
	EXPECT_TRUE(copy.Has("if (dst_va) chan_fence_volatile(chan); else chan_fence(chan);"));
	const CodeText fence = Block(gpu(), "static u32 chan_fence_job(");
	EXPECT_TRUE(
		fence.Before("NVB06F_FB_FLUSH, 0); if (external != TGM_FENCE_VOLATILE_WRITES) {",
			     "NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY"));
}

// A present waits for exactly the producers its surfaces' backings name (TegraScanoutCopy alsoWritten: window B's too),
// never for every channel whose address space maps them; window B's own fence maps nothing and copies nothing.
TEST(GM20BPresentSource, PresentWaitsForTheSurfacesProducersOnly)
{
	const CodeText copy = Block(gpu(), "static int present_copy(");
	EXPECT_FALSE(copy.Has("tgm_live_chans"));
	EXPECT_FALSE(copy.Has("vm_imports"));
	EXPECT_FALSE(gpuCode().Has("int tgm_present_copy("));
	EXPECT_FALSE(gpuCode().Has("int tgm_present_copy_read("));
	EXPECT_TRUE(Block(client(), "extern \"C\" int tegra_gm20b_scanout_copy(")
			    .Has("switch_gpu_backing_find_pa(copy->alsoWritten & ~0xfffULL)"));
	const CodeText fence = Block(client(), "extern \"C\" int tegra_gm20b_scanout_fence(");
	EXPECT_TRUE(fence.Before("switch_gpu_backing_find_pa(pa & ~0xfffULL)",
				 "gPresentSnapshot.add(backing)"));
	EXPECT_TRUE(fence.Before("gPresentSnapshot.add(backing)",
				 "tgm_present_copy_dependencies(0, 0, -1, 1, 1, 0, 0, 0, "
				 "SwitchGpuScanoutSnapshot::complete, &gPresentSnapshot, "
				 "gPresentSnapshot.tokens, gPresentSnapshot.count)"));
	EXPECT_TRUE(fence.Has("if (ret && !presentBusy) gPresentSnapshot.cancel();"));
	EXPECT_TRUE(fence.Has("switch_gpu_backing_release(backing);"));
	EXPECT_FALSE(fence.Has("presentSurface("));
}

// The scanout read-back (TegraGM20BReadbackABI.h) rides the frame's own present: a submission of its own would hold the
// present channel, which then refuses the display's next offer, and the display copies that frame on the CPU. Its copies
// run after the frame's copy, each after the copies before it, into pitch-linear buffers, and before the fence; a refused
// present gives the read-back back; its completion, in the GPU interrupt pass, takes no lock and touches no cache; and a
// release kernel has none of it.
TEST(GM20BPresentSource, ReadbackRidesTheFramesPresent)
{
	const CodeText copy = Block(gpu(), "static int present_copy(");
	EXPECT_TRUE(copy.Before("if (dst_va) tgm_present_push_copy(push);",
				"present_push_reads(push, reads, nreads);"));
	EXPECT_TRUE(copy.Before("present_push_reads(push, reads, nreads);", "chan_fence(chan);"));
	EXPECT_TRUE(copy.Has("nreads * TGM_COPY_RECT_DWORDS"));
	const CodeText reads = Block(gpu(), "static void present_push_reads(");
	EXPECT_TRUE(reads.Has("dst = {r->dst_va, r->dst_pitch, -1};"));  // pitch-linear
	EXPECT_TRUE(reads.Has("tgm_copy_texels(push->cur, &src, &dst, r->width, r->height)"));
	EXPECT_TRUE(CodeText(Configured(gpu(), "TGM_STATS", true))
			    .Has("static void present_push_reads("));
	EXPECT_FALSE(CodeText(Configured(gpu(), "TGM_STATS", false)).Has("present_push_reads("));

	const CodeText scanout = Block(client(), "extern \"C\" int tegra_gm20b_scanout_copy(");
	EXPECT_TRUE(scanout.Before("#if TGM_STATS tgm_present_read reads",
				   "#else gPresentSnapshot.done = presentDone; ret = watch ? "
				   "presentWatched() : 1; if (ret "
				   "== 1) ret = tgm_present_copy_dependencies("));
	EXPECT_TRUE(scanout.Has("nreads ? tgm_readback_done : presentDone"));
	EXPECT_TRUE(scanout.Has("if (ret && nreads) tgm_readback_refused();"));

	const std::string readback = SourceText("../TegraGM20BReadback.cpp");
	const CodeText done = Block(readback, "void tgm_readback_done(void *display, int status)");
	EXPECT_TRUE(done.Has("native_legacy_gpu_complete(display, status);"));
	EXPECT_FALSE(done.Has("IOLock"));
	EXPECT_FALSE(done.Has("tgm_cpu_clean_pages"));
	EXPECT_TRUE(CodeText(Configured(readback, "TGM_STATS", false))
			    .EndsWith("#include \"TegraGM20BReadback.h\""));
}

// Live (ui.glass_live): every present, armed or not, records what window A will scan for it, so the next armed present
// can read the frame shown before it again, as it is at the end of its time on screen; only an Arm takes the live flag.
TEST(GM20BPresentSource, LiveReadbackRereadsTheFrameShownBefore)
{
	const std::string readback = SourceText("../TegraGM20BReadback.cpp");
	const CodeText take = Block(readback, "uint32_t tgm_readback_take(");
	EXPECT_TRUE(take.Before("const Shown before = gShown;",
				"int armed = TegraGM20BReadbackArmed;"));
	EXPECT_TRUE(take.Before("gShown.sequence = copy->sequence;",
				"int armed = TegraGM20BReadbackArmed;"));
	EXPECT_TRUE(take.Has("gShown.read = direct ? tgm_present_read{source,"));
	EXPECT_TRUE(take.Has(": tgm_present_read{target,"));
	EXPECT_TRUE(take.Before("reads[1] = before.read;", "reads[1].dst_va = bufferVa(1);"));
	EXPECT_TRUE(take.Has("r.live && before.read.src_va && before.read.width == copy->width"));
	const CodeText call = Block(readback, "IOReturn tgm_readback_call(");
	EXPECT_TRUE(
		call.Has("in->reserved > 1 || (in->reserved && in->op != TegraGM20BReadbackArm)"));
	EXPECT_TRUE(call.Has("arm(owner, in->reserved == 1)"));
}

// A tiled (block-linear) source: the copy engine reads it block-linear with the tiled block height, never as pitch, through
// a mapping of the kind its texels were written under; a surface's layout comes from its decision (SurfaceLayout), never
// from how one client bound it.
TEST(GM20BPresentSource, TiledSourceIsCopiedBlockLinear)
{
	const CodeText copy = Block(gpu(), "void tgm_present_push_copy(");
	EXPECT_TRUE(copy.Has("const u32 src_pitch = tgm_present.copy.src_block_log2 >= 0 ? "
			     "tgm_present.copy.width * 4 : (u32)tgm_present.copy.stride;"));
	EXPECT_TRUE(copy.Has("{tgm_present.copy.src_va, src_pitch, "
			     "tgm_present.copy.src_block_log2}"));
	const CodeText scanout = Block(client(), "extern \"C\" int tegra_gm20b_scanout_copy(");
	EXPECT_TRUE(scanout.Has(
		"sourceBlock ? static_cast<int32_t>(copy->sourceBlockHeightLog2) : -1;"));
	EXPECT_TRUE(scanout.Has("tgm_readback_take(copy, source, blockLog2, target, reads)"));
	EXPECT_FALSE(Block(SourceText("../TegraGM20BReadback.cpp"), "uint32_t tgm_readback_take(")
			     .Has("TEGRA_SCANOUT_TILED"));
	const CodeText surface = Block(client(), "uint64_t presentSurface(");
	EXPECT_TRUE(surface.Has("const uint32_t kind = blockLinear ? TGM_LAYOUT_KIND : 0;"));
	EXPECT_TRUE(surface.Has("s.mem, bytes, static_cast<uint8_t>(kind))"));
	EXPECT_FALSE(Block(client(), "IOReturn AGXDeviceUserClient::vmBind(").Has("tiled"));
	EXPECT_TRUE(Block(client(), "IOReturn AGXDeviceUserClient::surfaceLayoutInfo(")
			    .Has("tgm_coherency_layout(coh,"));
	const CodeText info = Block(client(), "IOReturn AGXDeviceUserClient::surfaceLayoutInfo(");
	EXPECT_TRUE(info.Has("? tgm_coherency_layout_fitted(coh, width, height, "
			     "surface.bpp, pitch, writes)"));
	EXPECT_TRUE(info.Has("canonical || (same && tgm_layout_nil_height(&tiled))"));
}

// System sleep (gpu_power.c "system sleep"): every path that touches the GPU outside tgm_gpu.lock goes through the gate
// gm20b_sleep_test runs, and sleep and wake keep the orders the hardware needs.
TEST(GM20BSleepSource, GpuTouchingPathsGoThroughTheGate)
{
	for (const char *waits : {"int tgm_chan_exec(", "int tgm_chan_new("}) {
		const CodeText body = Block(gpu(), waits);
		EXPECT_TRUE(body.Before("gate_enter();", "gate_leave();"));
	}
	for (const char *refused :
	     {"int tgm_present_copy_dependencies(", "int tgm_present_copy_read_dependencies(",
	      "int tgm_pool_scrub(uint64_t pa, uint64_t bytes)",
	      "static void scrub_work(void *arg)"}) {
		const CodeText body = Block(gpu(), refused);
		EXPECT_TRUE(body.Before("if (!gate_try())", "gate_leave();"));
	}
	// the drain's and the wake's fence waits are woken by the scrubber's fences too
	EXPECT_TRUE(Block(gpu(), "static int chan_nonstall(struct nvif_event *event, "
				 "void *repv, u32 repc)")
			    .Has("if (tgm_gate_is_shut(&tgm_sc7_gate)) nvxnu_wake();"));
	// a pass for an interrupt taken before the GPU's power went reads nothing
	EXPECT_TRUE(Block(SourceText("../shim/xnu_glue.cpp"),
			  "void gpuInterrupt(OSObject *, IOInterruptEventSource *source, int)")
			    .Has("if (__atomic_load_n(&gGpuOff, __ATOMIC_ACQUIRE)) return;"));
}

// Fence waits sleep on their channel's word: a fence interrupt wakes only the waiters of a channel whose fence reached
// the lowest seqno one of them published, a kill or a teardown wakes that channel's, and the interrupt pass's own wake is
// left to the kernel's waits.
TEST(GM20BFenceWakeSource, WaitersWakeByChannel)
{
	const CodeText nonstall = Block(
		gpu(), "static int chan_nonstall(struct nvif_event *event, void *repv, u32 repc)");
	EXPECT_TRUE(
		nonstall.Has("} else { chan_wake_waiters(chan); present_watch_fenced(chan, 0); }"));
	EXPECT_EQ(nonstall.Count("nvxnu_wake();"),
		  1u);  // the scrubber's, while the sleep gate is shut
	const CodeText wake = Block(gpu(), "static void chan_wake_waiters(struct tgm_chan *chan)");
	EXPECT_TRUE(wake.Has("if (done == chan->woken_done) return;"));
	EXPECT_TRUE(wake.Has("if (switch_gpu_waiter_reached(&chan->wanted, done)) "
			     "chan_wake_all_waiters(chan);"));
	EXPECT_TRUE(Block(gpu(), "int tgm_chan_killed(struct nvif_event *event, void "
				 "*repv, u32 repc)")
			    .Has("chan_wake_all_waiters(chan);"));
	EXPECT_TRUE(
		Block(gpu(), "void tgm_chan_del(struct tgm_chan **pchan)")
			.Before("*pchan = NULL;", "nvxnu_word_wake(&tgm_waiter_words[waiter]);"));
	EXPECT_TRUE(Block(SourceText("../shim/xnu_glue.cpp"),
			  "void gpuInterrupt(OSObject *, IOInterruptEventSource *source, int)")
			    .Has("nvxnu_wake();"));
	const CodeText wait = Block(client(), "IOReturn AGXDeviceUserClient::waitFence(");
	EXPECT_TRUE(
		wait.Before("seq = tgm_waiter_seq(waiter);", "tgm_chan_want(chan, in->seqno);"));
	EXPECT_TRUE(wait.Before("tgm_chan_want(chan, in->seqno);",
				"tgm_chan_completed(chan) - in->seqno"));
	EXPECT_TRUE(wait.Has("tgm_waiter_idle(waiter, seq, deadline)"));
	// a seqno passed silently (an internal fence) is announced only by its job's end
	EXPECT_TRUE(wait.Before("tgm_chan_announced(chan, in->seqno);",
				"tgm_waiter_unwoken(waiter, seq, in->seqno);"));
	EXPECT_FALSE(wait.Has("tgm_idle("));
}

// A killed channel is dead alone (client fault isolation): what names it refuses, the connection's memory, binds and other
// channels go on, a new channel can be made in it, and nothing another channel waits for stays on the dead one.
TEST(GM20BKillSource, AKilledChannelLeavesItsConnectionWorking)
{
	const CodeText run = Block(client(), "IOReturn AGXDeviceUserClient::run(uint32_t method,");
	EXPECT_FALSE(clientCode().Has("dead_"));
	EXPECT_FALSE(run.Has("tgm_chan_error("));
	EXPECT_TRUE(Block(gpu(), "int chan_exec_body(struct tgm_chan *chan,")
			    .Has("if (chan->error) return -EIO;"));
	const CodeText order = Block(gpu(), "int tgm_chan_order(struct tgm_chan *chan,");
	EXPECT_TRUE(order.Has("if (READ_ONCE(chan->error)) return -EIO;"));
	EXPECT_TRUE(order.Has("READ_ONCE(other->error)) continue;"));
	const CodeText killed =
		Block(gpu(), "int tgm_chan_killed(struct nvif_event *event, void *repv, u32 repc)");
	EXPECT_TRUE(killed.Has(
		"__atomic_store_n(chan->fence_cpu, chan->submitted, __ATOMIC_RELEASE);"));
	EXPECT_TRUE(killed.Has("chan->kernel || !tgm_all_clients()"));
	EXPECT_TRUE(
		killed.Before("switch_gpu_timeline_failed(", "__atomic_store_n(chan->fence_cpu"));
	const CodeText freed = Block(gpu(), "static void chan_free(struct tgm_chan *chan)");
	EXPECT_TRUE(freed.Before("nvif_event_dtor(&chan->kill)", "switch_gpu_timeline_close("));
	EXPECT_TRUE(freed.Before("nvif_event_dtor(&chan->nonstall)", "switch_gpu_timeline_close("));
	EXPECT_TRUE(Block(client(), "bool AGXDeviceUserClient::bindingIdle(")
			    .Has("!tgm_chan_error(chans_[slot])"));
}

// The raw path's reopen after its channel is killed: the shader heap's BO and VA outlive the channel, so every program
// placed in it (Device::place) keeps its slot and every state that baked its offset stays drawable; the new channel gets
// what lives in a channel's context again, the program region at the heap, the texture and sampler pools and the root
// constant buffer (emitContext, from the pack's relocations) and the 2D state, and the first command buffer's prologue
// invalidates the shader caches before any draw.
TEST(GM20BRawReopenSource, ReopenKeepsTheHeapAndReplaysTheContext)
{
	const std::string raw = SourceText("../../TegraGM20BMetal/raw/runtime/raw_gpu.cpp");
	// raw_channel_recovery.h makeChannel makes the channel, then runs newContext on it
	const CodeText reopen =
		Block(raw, "bool Device::reopen()") +
		Block(raw, "bool Device::newContext(uint32_t channel, uint32_t &seq)");
	EXPECT_FALSE(reopen.Has("placed_"));
	EXPECT_FALSE(reopen.Has("heapPlaced_"));
	EXPECT_FALSE(reopen.Has("pipelines_"));
	EXPECT_FALSE(reopen.Has("heap_"));
	EXPECT_TRUE(reopen.Before("makeChannel(k_, kEngines, channel_, fence_", "emitContext(p);"));
	EXPECT_TRUE(reopen.Before("emitContext(p);", "fx::emitTwodInit(p);"));
	// the interlock's ticket buffer zeroed by the copy engine in the new channel, after the context names it and before
	// the 3D engine runs again
	EXPECT_TRUE(
		reopen.Before("emitContext(p);",
			      "uint32_t(tickets_.va >> 32), uint32_t(tickets_.va)}, SUBC_COPY);"));
	EXPECT_TRUE(reopen.Before("uint32_t(tickets_.va)}, SUBC_COPY);",
				  "p.m(CE_LAUNCH_DMA, kLaunchPitch, SUBC_COPY);"));
	EXPECT_TRUE(reopen.Before("p.m(CE_LAUNCH_DMA, kLaunchPitch, SUBC_COPY);",
				  "p.m(WAIT_FOR_IDLE, 0);"));
	const CodeText context = Block(raw, "void Device::emitContext(Push &p)");
	// the init template relocated to this device's own addresses (raw_pack.cpp relocated, shared with the runtime's capture),
	// the interlock's ticket buffer among them
	EXPECT_TRUE(context.Has("const InitAddresses here{heap_.va, zero_.va, root_.va, "
				"tic_.va, tsc_.va, kTicCount - 1, kTscCount - 1, tickets_.va};"));
	EXPECT_TRUE(context.Has("relocated(im[i], here)"));
	const CodeText relocated =
		Block(SourceText("../../TegraGM20BMetal/raw/runtime/raw_pack.cpp"),
		      "uint32_t relocated(const PackInitMethod &m, const InitAddresses &a)");
	for (const char *reloc :
	     {"case RelocHeapHi:", "case RelocHeapLo:", "case RelocTicHi:", "case RelocTicLo:",
	      "case RelocTscHi:", "case RelocTscLo:", "case RelocRootHi:", "case RelocRootLo:",
	      "case RelocTicketsLo:", "case RelocTicketsHi:"})
		EXPECT_TRUE(relocated.Has(reloc));
	EXPECT_TRUE(context.Has("sampleWords(p, 1, root_.va);"));
	EXPECT_TRUE(Block(raw, "void sampleWords(Push &p, uint32_t samples, uint64_t root,")
			    .Has("sampleLayoutMethods(p, l, kRootBytes, root);"));
	EXPECT_TRUE(Block(SourceText("../../TegraGM20BMetal/raw/runtime/raw_multisample.h"),
			  "inline void sampleLayoutMethods(")
			    .Has("p.inc(NVB197_SET_CONSTANT_BUFFER_SELECTOR_A,"));
	const CodeText begin = Block(raw, "CommandBuffer *Device::begin()");
	EXPECT_TRUE(begin.Before("!reopen())", "p.m(INVALIDATE_SHADER_CACHES_NO_WFI,"));
}

// No spin for GPFIFO or push room: every wait for room sleeps until a fence's interrupt, and the fence reports GET to nvif
// before that interrupt, so the room the GPU freed is there when the waiter looks.
TEST(GM20BRoomSource, RoomIsWaitedForByInterrupt)
{
	EXPECT_EQ(gpuCode().Count("PUSH_WAIT("), 0u);
	EXPECT_EQ(gpuCode().Count("nvif_chan_gpfifo_wait("), 3u);
	const CodeText wait =
		Block(gpu(),
		      "int tgm_chan_wait_ready(struct tgm_chan *chan, u32 gpfifo_nr, u32 push_nr)");
	EXPECT_TRUE(wait.Has("if (!nvxnu_idle(seq, end)"));
	const CodeText reserve = Block(gpu(), "static inline int chan_wait_room(");
	EXPECT_TRUE(reserve.Before("chan_wait_ready(chan, gpfifo_nr, push_nr)",
				   "nvif_chan_gpfifo_wait(&chan->chan, gpfifo_nr, push_nr)"));
	EXPECT_TRUE(
		Block(gpu(), "static u32 chan_fence_job(struct tgm_chan *chan, int "
			     "external, u32 first, u32 next, int quiet,")
			.Before("nvif_chan_gpfifo_post(&chan->chan);", "NON_STALL_INTERRUPT, 0);"));
}

TEST(GM20BRoomSource, NativePublicationNeverSleepsWithBackingLocksHeld)
{
	const CodeText exec = Block(client(), "IOReturn AGXDeviceUserClient::exec(");
	EXPECT_TRUE(exec.Before("switch_gpu_chan_submit_enter(chan);",
				"switch_gpu_backing_begin_checked("));
	EXPECT_TRUE(exec.Before("switch_gpu_backing_abort(&transaction);",
				"switch_gpu_chan_wait_submission_room("));
	EXPECT_TRUE(exec.Before("switch_gpu_backing_commit(&transaction);",
				"switch_gpu_chan_submit_leave();"));
	const CodeText emit = Block(gpu(), "int chan_exec_body(");
	EXPECT_TRUE(emit.Has("if (!may_wait && !chan_room(chan, entries, words)) return -EAGAIN;"));
	EXPECT_TRUE(emit.Before("chan->chan.defer_kick = true;",
				"nvif_chan_gpfifo_wait(&chan->chan, entries, words)"));
	EXPECT_TRUE(emit.Has("chan->chan.gpfifo.cur = ring_cur;"));
}

// SubmitJob holds no connection lock and orders its shared uses' backing locks exactly as exec does.
TEST(GM20BRoomSource, SubmitJobNeverSleepsWithBackingLocksHeld)
{
	const CodeText job = Block(client(), "IOReturn AGXDeviceUserClient::runJobUses(");
	EXPECT_FALSE(job.Has("IOLockLock(lock_)"));
	EXPECT_TRUE(job.Before("switch_gpu_chan_submit_enter(chan);",
			       "switch_gpu_backing_begin_checked("));
	EXPECT_TRUE(job.Before("switch_gpu_backing_abort(&transaction);",
			       "switch_gpu_chan_wait_submission_room("));
	EXPECT_TRUE(job.Before("switch_gpu_backing_commit(&transaction);",
			       "switch_gpu_chan_submit_leave();"));
	EXPECT_TRUE(job.Before("switch_gpu_set_note_job(set, slot, &completion);",
			       "switch_gpu_chan_submit_leave();"));
	const CodeText submit = Block(client(), "IOReturn AGXDeviceUserClient::submitJob(");
	EXPECT_TRUE(submit.Before("submitGate_.enterFast();", "runJob(record, job, out)"));
	EXPECT_TRUE(submit.Before("runJob(record, job, out)", "submitGate_.leaveFast(lock_);"));
}

// The HW Test's gpu.path.submit_job reads these per process: a job SubmitJob ran, whether it named its set at the current
// generation, a refusal for an ended generation, and a job the older SubmitAddress ran.
TEST(GM20BRoomSource, SubmissionMethodsCountTheirPaths)
{
	const CodeText run = Block(client(), "IOReturn AGXDeviceUserClient::runJob(");
	EXPECT_TRUE(run.Before("switch_gpu_path_taken(SWITCH_GPU_PATH_STALE_SET, 1);",
			       "return kIOReturnNotReady;"));
	const CodeText job = Block(client(), "IOReturn AGXDeviceUserClient::runJobUses(");
	EXPECT_TRUE(job.Before("switch_gpu_path_taken(SWITCH_GPU_PATH_SUBMIT_JOB, 1);",
			       "return kIOReturnSuccess;"));
	EXPECT_TRUE(job.Before("switch_gpu_path_taken(SWITCH_GPU_PATH_SET_JOB, set ? 1 : 0);",
			       "return kIOReturnSuccess;"));
	EXPECT_TRUE(
		job.Before("switch_gpu_path_taken(SWITCH_GPU_PATH_SHARED_USE, head.shared_count);",
			   "return kIOReturnSuccess;"));
	const CodeText address = Block(client(), "IOReturn AGXDeviceUserClient::submitAddress(");
	EXPECT_TRUE(address.Before(
		"result = exec(submit->channel,",
		"switch_gpu_path_taken(SWITCH_GPU_PATH_ADDRESS_JOB, result ? 0 : 1);"));
	const std::string paths = SourceText("../SwitchGpuPathStats.cpp");
	for (const char *name : {"\"submit_jobs\"", "\"set_jobs\"", "\"stale_sets\"",
				 "\"address_jobs\"", "\"shared_uses\""})
		EXPECT_TRUE(CodeText(paths).Has(name));
}

// A SubmitJob command buffer of several jobs holds its shared uses' CPU-lock admission from Begin to End, as SubmitAddress
// did: a Continue job takes the admission's guards instead of refreshing its surfaces, and a refused job ends it.
TEST(GM20BRoomSource, SubmitJobHoldsACommandBuffersAdmission)
{
	const CodeText job = Block(client(), "IOReturn AGXDeviceUserClient::runJobUses(");
	EXPECT_TRUE(job.Has("switch_gpu_claim_flags(admissionBegin || admissionContinue)"));
	EXPECT_TRUE(job.Before("if (admissionContinue && !admission.covers(job.uses, useTotal))",
			       "refresh:"));
	EXPECT_TRUE(job.Before("error = admission.begin(job.uses, useTotal, job.guards, "
			       "guardCount, job.leases, leaseCount);",
			       "switch_gpu_chan_submit_enter(chan);"));
	EXPECT_TRUE(job.Before("switch_gpu_chan_submit_leave();", "admission.end();"));
	const CodeText submit = Block(client(), "IOReturn AGXDeviceUserClient::submitJob(");
	EXPECT_TRUE(submit.Before("(record.job->flags & TegraGM20BJobAdmissionCancel)",
				  "runJob(record, job, out)"));
}

// A fence wait woken at its seqno decides from the fence page alone; the connection lock is for publishing, errors and
// timeouts.
TEST(GM20BRoomSource, AWokenFenceWaitTakesNoConnectionLock)
{
	const CodeText wait = Block(client(), "IOReturn AGXDeviceUserClient::waitFence(");
	EXPECT_TRUE(wait.Before("switch_gpu_fence_passed(record, in->seqno, waitedOn)",
				"IOLockLock(lock_);"));
	EXPECT_TRUE(wait.Before("if (ret != kIOReturnSuccess) { IOLockLock(lock_);",
				"tgm_chan_want(chan, in->seqno);"));
}

// The waiter a submitter sleeps in after -EAGAIN waits for exactly the room chan_exec then checks, computed in one place:
// a waiter that returned with less left the retry loops (TegraGM20BCopy.cpp submit, the user client's exec) spinning.
TEST(GM20BRoomSource, WaiterAndSubmissionReserveOneRoom)
{
	EXPECT_EQ(gpuCode().Count("5 * (TGM_ORDER_MAX + dependency_count"), 1u);
	const CodeText room = Block(gpu(), "static void chan_submission_room(");
	EXPECT_TRUE(room.Has("5 * (TGM_ORDER_MAX + dependency_count + 1)"));
	// SubmitJob's sibling acquires take a dependency's room; its retry asks the waiter for both together.
	EXPECT_TRUE(Block(gpu(), "int chan_exec_body(")
			    .Before("chan_submission_room(chan, pushes, count, dependency_count + "
				    "sibling_count + word_wait_count, "
				    "fences, &prof, &entries, &words);",
				    "if (!may_wait && !chan_room(chan, entries, words)) return "
				    "-EAGAIN;"));
	EXPECT_TRUE(Block(gpu(), "int switch_gpu_chan_wait_submission_room(")
			    .Before("chan_submission_room(chan, pushes, count, dependency_count, "
				    "fences, &prof, &entries, &words);",
				    "ret = tgm_chan_wait_ready(chan, entries, words);"));
}

// The L2 invalidate that makes the CPU's writes visible is ordered on the GPU, in the submission's own stream, ahead of
// a first push that is not prefetched; the CPU does not wait for it. A channel behind its space's GPU-ordered maps
// invalidates its TLB after the L2 (the walk reads page tables through it) in the same entry.
TEST(GM20BL2Source, InvalidateOnTheGpu)
{
	EXPECT_EQ(gpuCode().Count("FLUSH_L2_SYSTEM_INVALIDATE"), 0u);
	const CodeText exec = Block(gpu(), "int chan_exec_body(");
	EXPECT_TRUE(exec.Before("tlb = tgm_tlb_take(&chan->tlb, &chan->vm->tlb) || bind;",
				"NVB06F_MEM_OP_D_OPERATION_L2_SYSMEM_INVALIDATE);"));
	// a pending bind's acquire comes before the TLB invalidate that makes its new PTEs seen
	EXPECT_TRUE(exec.Before(
		"bind = !ret && !chan->kernel && bind_pending(chan->vm, &bind_target, &bind_va);",
		"tlb = tgm_tlb_take(&chan->tlb, &chan->vm->tlb) || bind;"));
	EXPECT_TRUE(exec.Before(
		"NVB06F_MEM_OP_D_OPERATION_L2_SYSMEM_INVALIDATE);",
		"NVB06F_MEM_OP_D_OPERATION_MMU_TLB_INVALIDATE); } PUSH_KICK(&chan->chan.push);"));
	EXPECT_TRUE(exec.Before(
		"NVB06F_MEM_OP_D_OPERATION_MMU_TLB_INVALIDATE); } PUSH_KICK(&chan->chan.push);",
		"job.unprefetched = (l2_on() && host_written) || tlb;"));
	EXPECT_TRUE(Block(gpu(), "void chan_job_entry(")
			    .Has("pushes[i].no_prefetch || (job->unprefetched && !i));"));
}

// A CPU IOSurfaceLock waits for the submissions that may write the surface: Exec names each one once it has its seqno.
TEST(GM20BCoherencySource, ExecNamesTheWriterOnceTheSubmissionHasItsSeqno)
{
	const CodeText exec = Block(client(), "IOReturn AGXDeviceUserClient::exec(");
	EXPECT_TRUE(exec.Before(
		"error = switch_gpu_chan_exec_events(",
		"surfaces_.after(listed, importsWritten_, tgm_chan_uid(chan), seqno);"));
	EXPECT_TRUE(exec.Before("error = switch_gpu_chan_exec_events(",
				"switch_gpu_backing_commit(&transaction);"));
	const CodeText after =
		Block(SourceText("../TegraGM20BExecSurfaces.cpp"), "void ExecSurfaces::after(");
	EXPECT_TRUE(after.Has("tgm_coherency_wrote(refs_[i].coh, always, uid, seqno);"));
	// only a listed write is a writer another channel waits for on the GPU (gm20b_fences_test)
	EXPECT_TRUE(after.Has("tgm_coherency_wrote_listed(r->coh, uid, seqno);"));
}

// Method 17's acquires go into the submission's own stream: queued (order_add, under tgm_live_lock) before tgm_chan_exec makes
// it, every exit of Exec past the channel check having taken the list; the decision is TegraGM20BFences.h's, which
// gm20b_fences_test runs.
TEST(GM20BFencesSource, ExecQueuesItsWritersAcquiresBeforeItSubmits)
{
	const CodeText exec = Block(client(), "IOReturn AGXDeviceUserClient::exec(");
	EXPECT_TRUE(exec.Before("if (followed < 0) return ioreturn(followed);",
				"error = switch_gpu_chan_exec_events("));
	EXPECT_TRUE(exec.Before("switch_gpu_backing_dependencies(",
				"error = switch_gpu_chan_exec_events("));
	EXPECT_TRUE(exec.Has("ownershipBusy ? kIOReturnCannotLock : ioreturn(error)"));
	// Named uses carry no CPU exclusion unless an admission ordered them; a legacy job claims only listed imports.
	EXPECT_TRUE(exec.Has("const uint32_t claim = switch_gpu_claim_flags(admissionBegin "
			     "|| admissionContinue);"));
	EXPECT_TRUE(exec.Has("addUse(bo, uses[i].access, claim, offset, bytes)"));
	EXPECT_TRUE(exec.Has("addUse(bo, SWITCH_GPU_BACKING_READ, claim)"));
	EXPECT_TRUE(exec.Has("switch_gpu_legacy_uses("));
	for (const char *signature :
	     {"int tgm_chan_follow(struct tgm_chan *chan,", "int tgm_chan_follow_importers("}) {
		const CodeText follow = Block(gpu(), signature);
		EXPECT_TRUE(follow.Before("mutex_lock(&tgm_live_lock);", "tgm_fence_wanted("));
		EXPECT_TRUE(follow.Before("tgm_fence_wanted(", "ret = order_add(chan,"));
		EXPECT_TRUE(
			follow.Before("ret = order_add(chan,", "mutex_unlock(&tgm_live_lock);"));
	}
	const CodeText capabilities = Block(client(), "uint32_t switch_gpu_info_flags(void)");
	for (const char *capability : {"TegraGM20BInfoExecSurfaces", "TegraGM20BInfoSubmitAddress",
				       "TegraGM20BInfoCpuAccess"})
		EXPECT_TRUE(capabilities.Has(capability));
	EXPECT_TRUE(Block(client(), "IOReturn AGXDeviceUserClient::getInfo(")
			    .Has("out->flags = switch_gpu_info_flags();"));
	EXPECT_FALSE(clientCode().Has("kInfoAwaitingUserspace"));
}

// The flags exec hands chan_exec are SwitchGpuJobVisibility.h's (switch_gpu_job_visibility_test), with a write into an
// IOSurface over its creator's memory, which IOSurface's own field names at the import, as one that ends External.
TEST(GM20BVisibilitySource, AJobWritingCreatorMemoryIsExternal)
{
	const CodeText imported = Block(client(), "IOReturn AGXDeviceUserClient::boImport(");
	EXPECT_TRUE(imported.Before(
		"bo->surface = switch_gpu_surface_hold(",
		"bo->unlockedReads = switch_gpu_surface_client_memory(bo->surface) != 0;"));
	const CodeText exec = Block(client(), "IOReturn AGXDeviceUserClient::exec(");
	EXPECT_TRUE(exec.Before("if (access & SWITCH_GPU_BACKING_WRITE) {",
				"unlockedWrites |= bo->unlockedReads;"));
	EXPECT_TRUE(exec.Has(
		"switch_gpu_job_visibility(visibility, events && events->signals(), "
		"unlockedWrites || scanoutWrites, hostWritten || transaction.host_written)"));
	EXPECT_TRUE(Block(SourceText("../TegraGM20BSurfaceClient.cpp"),
			  "int switch_gpu_surface_client_memory(const OSObject *surface)")
			    .Has("at<uint64_t>(surface, OriginalIOSurface::clientMemory) != 0"));
}

// FRAMECPU C6.1: a job writing a backing the display scans in place (a direct scanout slot) ends External in both submit
// paths, so its completion means its writes are in DRAM and the display may flip with no present job.
TEST(GM20BVisibilitySource, AJobWritingAScannedBackingIsExternal)
{
	const CodeText exec = Block(client(), "IOReturn AGXDeviceUserClient::exec(");
	EXPECT_TRUE(exec.Before("if (access & SWITCH_GPU_BACKING_WRITE) {",
				"scanoutWrites |= switch_gpu_backing_scanned(bo->backing) != 0;"));
	const CodeText job(SourceText("../TegraGM20BUserClientJobSubmit.cpp"));
	EXPECT_TRUE(job.Before("if (access & TegraGM20BSurfaceWrite) {",
			       "scanoutWrites |= switch_gpu_backing_scanned(bo->backing) != 0;"));
	EXPECT_TRUE(
		job.Has("switch_gpu_job_visibility(submit, events->signals(), unlockedWrites || "
			"scanoutWrites, hostWritten || transaction.host_written)"));
	EXPECT_TRUE(
		Block(SourceText("../SwitchGpuBacking.cpp"),
		      "extern \"C\" int switch_gpu_backing_scanned(")
			.Has("__atomic_load_n(&backing->scanout_holds, __ATOMIC_ACQUIRE) != 0"));
}

// A SurfaceWrites a client could not deliver never leaves a GPU write unmarked: an Exec flagged
// TegraGM20BPushImportsWritten marks every writable import of its connection (NVK's flag is gm20b_nvk_source_test's;
// the raw path's imports, raw_kernel_memory_test's ASurfaceWritesTheKernelRefusedMarksEveryLaterImportWritten).
TEST(GM20BCoherencySource, AFailedSurfaceWritesFallsBackToMarkingEveryImport)
{
	const CodeText exec = Block(client(), "IOReturn AGXDeviceUserClient::exec(");
	EXPECT_TRUE(exec.Before(
		"importsWritten_ = true;",
		"surfaces_.after(listed, importsWritten_, tgm_chan_uid(chan), seqno);"));
}

// Only a connection's maps skip the CPU's L2 and TLB maintenance; every removal keeps it (TegraGM20BTlb.h).
TEST(GM20BTlbSource, MapsOrderedOnTheGpuRemovalsSynchronous)
{
	EXPECT_TRUE(clientCode().Has("switch_gpu_vm_map_volatile(vm_, op.va"));
	EXPECT_EQ(clientCode().Count("tgm_vm_map("), 0u);
	EXPECT_EQ(gpuCode().Count("nvxnu_gpu_ordered_begin();"), 3u);
	const CodeText map = Block(gpu(), "int tgm_vm_map_raw(");
	EXPECT_TRUE(map.Before("nvxnu_gpu_ordered_end(ordered_scope);", "nvif_vmm_raw_put("));
	const CodeText mixed = Block(gpu(), "int switch_gpu_vm_map_volatile(");
	EXPECT_TRUE(mixed.Before("nvxnu_gpu_ordered_end(ordered_scope);", "nvif_vmm_raw_unmap("));
	EXPECT_TRUE(mixed.Has("entry->va, entry->bytes, entry->shift"));
	const CodeText unmap = Block(gpu(), "void switch_gpu_vm_unmap(");
	EXPECT_FALSE(unmap.Has("gpu_ordered"));
	EXPECT_TRUE(unmap.Has("entry->va, entry->bytes, entry->shift"));
	EXPECT_TRUE(Block(gpu(), "void tgm_vm_del(").Has("mmu_invalidate_all();"));
	// a cut changes no PTE; what it leaves pending is installed GPU-ordered, its failures put outside that scope
	EXPECT_FALSE(Block(gpu(), "int switch_gpu_vm_split(").Has("nvif_vmm_raw"));
	const CodeText install = Block(gpu(), "int switch_gpu_vm_install(");
	EXPECT_TRUE(install.Before("nvxnu_gpu_ordered_end(ordered_scope);", "nvif_vmm_raw_put("));
}

// Rail-gating (TegraGM20BRail.h): every other tgm_gpu.lock taker goes through tgm_gpu_lock(), which wakes a rail-gated GPU in
// the caller under the lock it took; the rail-up opens the gate before it clears `railed`, so a closer that has seen the
// GPU up never has its close undone (gm20b_rail_test's model).
TEST(GM20BRailSource, GpuLockWakesARailGatedGpu)
{
	EXPECT_EQ(gpuCode().Count("mutex_lock(&tgm_gpu.lock);"), 6u)
		<< "gpu_lock, tgm_timestamp_ns, tgm_sleep, tgm_rail_down, tgm_rail_up, "
		   "tgm_rail_sc7_hold";
	EXPECT_TRUE(Block(gpu(), "noinline void tgm_gpu_lock(void)")
			    .Has("if (unlikely(tgm_gpu.railed)) "
				 "tgm_rail_up_locked(__builtin_return_address(0));"));
	EXPECT_TRUE(Block(gpu(), "void tgm_rail_up_locked(const void *site)")
			    .Before("gate_open();",
				    "__atomic_store_n(&tgm_gpu.railed, 0, __ATOMIC_SEQ_CST);"));
	// tgm_rail_up_locked takes these under the caller's tgm_gpu.lock: no tgm_gpu_lock() caller may hold one
	const auto calls = Occurrences(gpu(), "tgm_gpu_lock();");
	EXPECT_FALSE(calls.empty());
	for (const auto &call : calls)
		for (const char *lock : {"(&tgm_live_lock)", "(tgm_idle.clk)", "(&tgm_gate_lock)"})
			EXPECT_EQ(call.before.Count(std::string("mutex_lock") + lock),
				  call.before.Count(std::string("mutex_unlock") + lock))
				<< lock << " held at a tgm_gpu_lock() in " << call.head;
	// GPU time never backwards (gm20b_rail_test's model): the floor read before every power-down, PTIMER reseeded to it
	// after nvkm_timer_init, every time handed out clamped to it
	EXPECT_TRUE(Block(gpu(), "static void sleep_nvkm(void)")
			    .Before("tgm_stamp_last = tgm_stamp_monotonic(tgm_stamp_last, "
				    "nvkm_timer_read(device->timer));",
				    "nvkm_device_fini(device, NVKM_SUSPEND)"));
	EXPECT_TRUE(Block(gpu(), "static void wake_m1(void)")
			    .Before("ret = nvkm_subdev_init(&device->timer->subdev);",
				    "stamp_reseed(device);"));
	// a power-up's clock anchor (TegraGM20BClockABI.h) after its PTIMER is set, at boot and at every wake, before a channel
	// runs; a fence page handed out holds them all once zeroed
	EXPECT_TRUE(Block(gpu(), "static int m1(char *line, unsigned int len)")
			    .Before("ret = nvkm_subdev_init(&device->timer->subdev);",
				    "tgm_clock_anchor_publish(device);"));
	EXPECT_TRUE(Block(gpu(), "static void stamp_reseed(struct nvkm_device *device)")
			    .Before("nvkm_timer_alarm_trigger(device->timer);",
				    "tgm_clock_anchor_publish(device);"));
	EXPECT_TRUE(Block(gpu(), "static void wake_m1(void)")
			    .Before("stamp_reseed(device);",
				    "nvkm_subdev_init(&device->clk->subdev);"));
	EXPECT_TRUE(Block(gpu(), "int tgm_fence_page_get(volatile uint32_t **va, uint64_t *pa)")
			    .Before("memset((u8 *)tgm_fence_pages.va + i * TGM_FENCE_SLOT, 0, "
				    "PAGE_SIZE);",
				    "tgm_clock_anchors_fill(i);"));
	EXPECT_TRUE(Block(gpu(), "uint64_t tgm_timestamp_ns(void)")
			    .Has("tgm_stamp_last = t = tgm_stamp_monotonic(tgm_stamp_last, t);"));
	const CodeText down = Block(gpu(), "int tgm_rail_down(void)");
	EXPECT_TRUE(down.Before("sleep_gate();", "mutex_lock(&tgm_gpu.lock);"));
	EXPECT_TRUE(down.Before("__atomic_store_n(&tgm_gpu.railed, 1, __ATOMIC_SEQ_CST);",
				"nvxnu_wakeup(&tgm_sc7_gate.shut);"));
}

TEST(GM20BSleepSource, SleepAndWakeOrder)
{
	const CodeText sleep = Block(gpu(), "void tgm_sleep(void)").After("case TGM_SLEEP_FULL:");
	EXPECT_TRUE(sleep.Before("sleep_gate();", "sleep_fences();"));
	EXPECT_TRUE(sleep.Before("sleep_fences();", "mutex_lock(&tgm_gpu.lock);"));
	EXPECT_TRUE(sleep.Before("sleep_idle_off();", "sleep_nvkm();"));
	EXPECT_TRUE(sleep.Before("sleep_nvkm();", "sleep_power_down();"));
	const CodeText nvkm = Block(gpu(), "static void sleep_nvkm(void)");
	EXPECT_TRUE(nvkm.Before("nvif_client_suspend(&tgm_gpu.client, false)", "nvkm_ltc_flush("));
	// nvgpu_mm_suspend: the L2's comptag lines written back (FLUSH_L2_CLEAN_COMPTAGS) before the data flush
	EXPECT_TRUE(nvkm.Before("nvkm_wr32(device, 0x07000c, 0x00000001);", "nvkm_ltc_flush("));
	EXPECT_TRUE(nvkm.Before("nvkm_ltc_flush(", "nvkm_device_fini(device, NVKM_SUSPEND)"));
	EXPECT_TRUE(Block(gpu(), "static void sleep_power_down(void)")
			    .Before("nvxnu_irq_suspend();", "regulator_disable("));
	// the boot's order: the GPCPLL relock (tgm_clk_raise) before ACR, the PMU and GR (tgm_m2_device_init); interrupts on before
	// nvkm's first init that may wait for one
	const CodeText wake = Block(gpu(), "void tgm_wake(void)").After("case TGM_SLEEP_FULL:");
	EXPECT_TRUE(wake.Before("wake_power_up();", "wake_m1();"));
	EXPECT_TRUE(wake.Before("wake_m1();", "wake_clock();"));
	EXPECT_TRUE(wake.Before("wake_clock();", "wake_device();"));
	EXPECT_TRUE(wake.Before("wake_device();", "wake_nvkm();"));
	EXPECT_TRUE(wake.Before("wake_nvkm();", "wake_fence();"));
	EXPECT_TRUE(wake.Before("wake_fence();", "wake_open();"));
	EXPECT_TRUE(Block(gpu(), "static void wake_power_up(void)")
			    .Before("power_up(", "nvxnu_irq_resume();"));
	EXPECT_TRUE(
		Block(gpu(), "static void wake_clock(void)").Has("clk_raise(line, sizeof(line))"));
	EXPECT_TRUE(
		Block(gpu(), "static void wake_device(void)").Has("m2_device_init(device, &at)"));
}

// At M2 the FIFO has no user: nvkm makes its runqueues and runlists and registers its interrupt handler at the first
// channel (nvkm_fifo_oneinit), from its function table. What recover.c and sched.c take over goes into that table.
TEST(GM20BSchedSource, HooksGoWhereTheFifosOneinitReadsThem)
{
	const CodeText recover = Block(SourceText("../recover.c"),
				       "void tgm_recover_init(struct nvkm_device *device)");
	EXPECT_TRUE(recover.Has("func->intr = fifo_intr;"));
	EXPECT_TRUE(recover.Has("func->runq = &ctxsw.runq;"));
	EXPECT_TRUE(recover.Has("func->chan.func = &ctxsw.chan;"));
	EXPECT_TRUE(recover.Has("func->intr_ctxsw_timeout = ctxsw_tick;"));
	const std::string sched = SourceText("../sched.c");
	EXPECT_TRUE(Block(sched, "void tgm_sched_init(struct nvkm_device *device)")
			    .Has("tgm_fifo_func(fifo)->runl_ctor = sched_runl_ctor;"));
	EXPECT_TRUE(Block(sched, "static int sched_runl_ctor(struct nvkm_fifo *fifo)")
			    .Has("interleave(fifo->engine.subdev.device, runl);"));
}

// A submission that writes a block-linear IOSurface under a CPU write lock keeps the lock's rows before it runs, with the
// connection's lock dropped and before the active-submit gate's channel entry (the copy engine's wait sleeps).
// The push profile's stamps (debug kernels): one rule (switch_gpu_prof_stamped) decides the room a profiled job takes, the
// pushes that stamp and the classes read back, so the compositor stamps only where its job already waits for idle; and
// every window says per owner what the stamps cost.
TEST(GM20BPushScanSource, OneRuleDecidesAProfiledJobsStamps)
{
	EXPECT_TRUE(Block(gpu(), "static void chan_submission_room(")
			    .Has("stamps += switch_gpu_prof_stamped(chan->critical, &pushes[i], i, "
				 "count, (*prof)->cls[i]);"));
	EXPECT_TRUE(
		Block(gpu(), "static inline __attribute__((always_inline)) void chan_job_entry(")
			.Has("if (prof && chan->prof_cls[i] != TGM_PC_SKIP) {"));
	const CodeText &body = gpuCode();
	EXPECT_TRUE(body.Before(
		"const int stamped = switch_gpu_prof_stamped(chan->critical, &pushes[i], i, count, "
		"prof->cls[i]);",
		"last = switch_gpu_job_emit(pushes, count, chan->submitted, &emitter);"));
	EXPECT_TRUE(
		body.Has("__atomic_fetch_add(&chan->prof_span, end - start, __ATOMIC_RELAXED);"));
	EXPECT_TRUE(body.Has("nvxnu_stats(\"tegra-gm20b: gpuprof cost %s:%s\", o[i].name, line);"));
}

// gpu.path.resource_ready: a GPU write under a CPU write lock keeps the lock's rows for the merge only when its
// submission asks before its job is kicked (TegraGM20BViews.h tgm_views_job_writes). Both submission paths ask, once
// their uses are resolved (Exec outside the connection lock); nothing else calls the per-surface hook, so no path can
// skip the list, as SubmitJob did once raw moved onto it (57b71739d).
TEST(GM20BViewsSource, AGpuWriteUnderACpuWriteLockIsMergedNotHeldBack)
{
	const CodeText job =
		Block(client(),
		      "IOReturn AGXDeviceUserClient::runJobUses(const SwitchGpuJobRecord &record,");
	EXPECT_TRUE(job.Before("job.written[writtenCount++] = bo->coh;",
			       "tgm_views_job_writes(job.written, writtenCount);"));
	EXPECT_TRUE(job.Before("tgm_views_job_writes(job.written, writtenCount);",
			       "switch_gpu_chan_submit_enter(chan);"));
	EXPECT_TRUE(job.Before("tgm_views_job_writes(job.written, writtenCount);",
			       "switch_gpu_chan_exec_events("));
	const CodeText exec = Block(client(), "IOReturn AGXDeviceUserClient::exec(");
	EXPECT_TRUE(exec.Before("job.written[writtenCount++] = bo->coh;",
				"tgm_views_job_writes(job.written, writtenCount);"));
	EXPECT_TRUE(exec.Before("IOLockUnlock(lock_);",
				"tgm_views_job_writes(job.written, writtenCount);"));
	EXPECT_TRUE(exec.Before("tgm_views_job_writes(job.written, writtenCount);",
				"switch_gpu_chan_submit_enter(chan);"));
	EXPECT_EQ(clientCode().Count("tgm_views_gpu_writes("), 0u);
}

// The PBDMA's ACQUIRE interrupt is gpu_acquire.c's: the timeout goes off in the PBDMA before the bit is cleared (so the
// same acquire cannot raise it again), and nvkm's handler, which would kill the waiter, never sees it.
TEST(GM20BAcquireSource, TheAcquireInterruptIsTakenBeforeNvkmsHandler)
{
	const CodeText runq =
		Block(SourceText("../recover.c"),
		      "static bool runq_intr(struct nvkm_runq *runq, struct nvkm_runl *runl)");
	EXPECT_TRUE(
		runq.Before("tgm_acquire_intr(device, runq->id)",
			    "nvkm_wr32(device, PBDMA_INTR_0(runq->id), TGM_PBDMA_INTR_ACQUIRE)"));
	EXPECT_TRUE(runq.Before("TGM_PBDMA_INTR_ACQUIRE)", "ctxsw.runq_nvkm->intr(runq, runl)"));
	const CodeText intr =
		Block(gpu(), "void tgm_acquire_intr(struct nvkm_device *device, int runq)");
	EXPECT_TRUE(intr.Before("word & ~TGM_ACQUIRE_TIMEOUT_EN", "nvxnu_work_schedule(acq.work)"));
	EXPECT_TRUE(
		intr.Has("nvkm_mask(device, PBDMA_INTR_EN_0(runq), TGM_PBDMA_INTR_ACQUIRE, 0)"));
	EXPECT_FALSE(intr.Has("mutex_lock"));
	EXPECT_TRUE(Block(SourceText("../recover.c"), "void tgm_recover_suspend(void)")
			    .Has("tgm_acquire_suspend();"));
}

// Every channel's RAMFC carries the timeout before its first submission (the subchannel binds), and a channel whose
// timeout an interrupt turned off gets it back only while it is idle and off the PBDMA, as a level change's timeslice.
TEST(GM20BAcquireSource, EveryChannelIsArmedBeforeItsFirstKickAndReArmedWhenIdle)
{
	const CodeText create = Block(gpu(), "static int chan_new(");
	EXPECT_TRUE(create.Before("chan->chid = args.chan.chid;", "tgm_acquire_arm(chan->chid);"));
	EXPECT_TRUE(create.Before("tgm_acquire_arm(chan->chid);", "PUSH_KICK(push);"));
	const CodeText due = Block(gpu(), "void tgm_chan_ramfc_due(struct tgm_chan *chan)");
	EXPECT_TRUE(due.Before("tgm_sched_loaded(tgm_gpu.device, chan->chid)",
			       "tgm_acquire_arm(chan->chid);"));
	EXPECT_EQ(gpuCode().Count("tgm_chan_ramfc_due(chan);"), 2u);
}

// A late wait and an acquire timeout blame by what the candidates' RAMFC says they wait on, and a check that finds a
// channel parked past its timeout with no interrupt says so.
TEST(GM20BAcquireSource, TheHangRuleReadsWhatEachCandidateWaitsOn)
{
	const CodeText recover = Block(gpu(), "enum tgm_hang tgm_hang_recover(struct tgm_chan "
					      "*late, const char *who, u32 seqno, u64 waited_ns)");
	EXPECT_TRUE(recover.Has("candidate_wait(&waits, chan, &source)"));
	EXPECT_TRUE(recover.Has("kind = candidate_wait(&waits, chans[i], &source);"));
	EXPECT_TRUE(
		recover.Has("c[i].victim = kind == TGM_WAITS_VICTIM || kind == TGM_WAITS_UNREAD;"));
	EXPECT_TRUE(recover.Has("tgm_acquire_unfired(chans[i], now);"));
	EXPECT_FALSE(recover.Has("chan->waited.cpu"));
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
			if (read.head.Has(
				    "static int engn_cxid(struct nvkm_engn *engn, bool *cgid)"))
				continue;
			const CodeText guard = read.before.AfterLast("nvkm_runl_foreach_engn");
			EXPECT_TRUE(guard.Has("engn->func->cxid") || guard.Has("NVKM_ENGINE_GR"))
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
	EXPECT_TRUE(report.Has("os_log_error(OS_LOG_DEFAULT, \"%s\", line)"));
	EXPECT_FALSE(report.Has("%{"));
}

// A channel made in a slot continues its predecessor's fence sequence, so every cursor the interrupt pass compares with
// the completed seqno starts at that fence. From 0, the idle clock would count the predecessor's whole sequence retired,
// its in-flight count (kicked - retired) would underflow, and the GPU would never drop its clock or rail-gate.
TEST(GM20BChannelSource, SeqnoCursorsStartAtTheSlotsFence)
{
	const CodeText create = Block(gpu(), "static int chan_new(");
	EXPECT_TRUE(create.Has("chan->submitted = READ_ONCE(*fence_cpu);"));
	for (const char *cursor :
	     {"stat_done", "woken_done", "gov_done", "progress_done", "seen_done"})
		EXPECT_TRUE(create.Has(std::string("chan->") + cursor + " = chan->submitted;"))
			<< cursor;
}

// nvkm_chan_error preempts from the interrupt pass with interrupts off under the channel's spinlock: the wrapped preempt
// decides by tgm_preempt_how and waits for a preempt in flight only in thread context.
TEST(GM20BSchedSource, ThePreemptWaitsOnlyWithInterruptsOn)
{
	const CodeText preempt = Block(SourceText("../sched.c"),
				       "static void sched_chan_preempt(struct nvkm_chan *chan)");
	EXPECT_TRUE(preempt.Has("switch (tgm_preempt_how(irqs_disabled(), pending)) {"));
	EXPECT_TRUE(
		preempt.Before("case TGM_PREEMPT_RECOVER: nvkm_runl_rc_cgrp(chan->cgrp); return;",
			       "case TGM_PREEMPT_AFTER_WAIT:"));
	EXPECT_TRUE(preempt.Before("case TGM_PREEMPT_AFTER_WAIT:", "nvkm_wait_msec("));
	EXPECT_TRUE(preempt.Before("nvkm_wait_msec(", "case TGM_PREEMPT_NOW:"));
	EXPECT_EQ(preempt.Count("nvkm_wait_msec("), 1u);
}

// Linux's work items, as kits/SwitchLinux's calls (whose host test holds that a call never runs twice at once): nvkm's
// shim allocates them there, a flush runs one still queued, and irqs_disabled() answers.
TEST(GM20BShimSource, WorkItemsKeepLinuxSemantics)
{
	const CodeText nvxnu(SourceText("../shim/include/nvxnu.h"));
	EXPECT_TRUE(nvxnu.Has("return switch_linux_call_alloc(fn, arg);"));
	EXPECT_TRUE(nvxnu.Has("return switch_linux_irqs_disabled() ? 0 : 1;"));
	const CodeText os(SourceText("../shim/include/nvif/os.h"));
	EXPECT_TRUE(os.Has("static inline bool flush_work(struct work_struct *w) { return "
			   "switch_linux_call_flush(w->thread_call, nvxnu_work_trampoline, w); }"));
	EXPECT_TRUE(os.Has("#define irqs_disabled() (!nvxnu_irqs_enabled())"));
}

// Rail-down stops what reaches the GPU from callouts as system sleep does: the deadline's holds released while the GPU
// is up and its callouts stopped before the fences drain, back at the rail-up; a context-switch tick's work stopped once
// interrupts are off.
TEST(GM20BRailSource, RailDownStopsTheGpuCallouts)
{
	const CodeText down = Block(gpu(), "int tgm_rail_down(void)");
	EXPECT_TRUE(down.Before("sleep_gate();", "tgm_deadline_suspend();"));
	EXPECT_TRUE(down.Before("tgm_deadline_suspend();", "sleep_fences();"));
	EXPECT_TRUE(Block(gpu(), "void tgm_rail_up_locked(const void *site)")
			    .Before("__atomic_store_n(&tgm_gpu.railed, 0, __ATOMIC_SEQ_CST);",
				    "tgm_deadline_resume();"));
	const CodeText power = Block(gpu(), "static void sleep_power_down(void)");
	EXPECT_TRUE(power.Before("nvxnu_irq_suspend();", "tgm_recover_suspend();"));
	EXPECT_TRUE(power.Before("tgm_recover_suspend();", "regulator_disable("));
}

// IOGPUDeviceSetDisplayParams is scalar method 0x21 in iOS 17's IOGPU.framework and IOConnectTrap2 index 8 in iOS 27's;
// XNU answers a trap index with no trap behind it kIOReturnSuccess, so the device connection serves index 8 and refuses
// every other by name instead of letting it pass unseen.
TEST(GM20BDeviceSource, DisplayParamsArriveByMethodAndByTrap)
{
	const std::string client = SourceText("../TegraGM20BDeviceClient.cpp");
	EXPECT_TRUE(Block(client, "IOExternalTrap "
				  "*TegraGM20BDeviceUserClient::getTargetAndTrapForIndex(")
			    .Has("*targetP = this; if (index == kSetDisplayParamsTrap) return "
				 "&displayParams; switch_refusal_k("));
	EXPECT_TRUE(CodeText(client).Has("constexpr UInt32 kSetDisplayParamsTrap = 8;"));
	EXPECT_TRUE(Block(client, "IOReturn TegraGM20BDeviceUserClient::setDisplayParams(")
			    .Has("storeDeadline(reinterpret_cast<uint64_t>(nextVblank), "
				 "reinterpret_cast<uint64_t>(interval));"));
	EXPECT_TRUE(
		CodeText(client).Has("case kSetDisplayParams: if (arguments->scalarInputCount != "
				     "2) return kIOReturnBadArgument; storeDeadline("));
}

// A hang's line reads the hung submission's pushes through the user client that claimed the channel's space, and only
// that: the kernel's copiers (the tiling move, the copy engine's plane copies) claim theirs with their own state, which
// is no AGXDeviceUserClient. A live channel's client is not freed under it: hang recovery describes channels on the
// live list under tgm_live_lock, and the client's free() takes every channel off it before it frees its lock.
TEST(GM20BHangSource, OnlyAConnectionsPushesAreRead)
{
	EXPECT_TRUE(Block(gpu(), "void tgm_chan_describe(")
			    .Has("CHAN_PUSH_BYTES, chan->vm->connection, note, sizeof(note));"));
	EXPECT_TRUE(Block(client(), "bool AGXDeviceUserClient::setUp(")
			    .Has("tgm_vm_claim_connection(vm_, this);"));
	EXPECT_FALSE(clientCode().Has("tgm_vm_claim("));
	// The kernel copy queues' spaces are claimed by their producers' state, never as a connection's.
	EXPECT_TRUE(Block(SourceText("../SwitchGpuKqueue.h"), "bool openSpace(")
			    .Has("tgm_vm_claim(vm, owner);"));
	const std::string relayout = SourceText("../TegraGM20BRelayout.cpp");
	EXPECT_TRUE(CodeText(relayout).Has("r.space.openSpace(kShape, &gRelayout)"));
	EXPECT_FALSE(CodeText(relayout).Has("tgm_vm_claim_connection("));
	const std::string copy = SourceText("../TegraGM20BCopy.cpp");
	EXPECT_TRUE(Block(copy, "bool openSpace()").Has("queue.space.openSpace(kShape, &queue)"));
	EXPECT_FALSE(CodeText(copy).Has("tgm_vm_claim_connection("));
	EXPECT_TRUE(Block(client(), "void AGXDeviceUserClient::free()")
			    .Before("drainResources();", "IOLockFree(lock_);"));
	EXPECT_TRUE(Block(client(), "void AGXDeviceUserClient::drainResources()")
			    .Before("for (auto &chan : chans_) tgm_chan_del(&chan);",
				    "thread_call_cancel_wait(retireCall_);"));
	EXPECT_TRUE(Block(gpu(), "void tgm_chan_del(struct tgm_chan **pchan)")
			    .Before("mutex_lock(&tgm_live_lock); list_del(&chan->live);",
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
	EXPECT_TRUE(waits.Has("nwaits = tgm_relayout_follow(self, running, fenceVa, n, waits, "
			      "nwaits, &refused);"));
	EXPECT_TRUE(
		waits.Has("\"gpu-order\", \"writer-unordered\", \"a kernel copy cannot acquire"));
	EXPECT_TRUE(waits.Has("return TGM_RELAYOUT_WAIT_FAILED;"));
	EXPECT_FALSE(waits.Has("tgm_writer_wait("));
	const CodeText move = Block(relayout, "int move(");
	EXPECT_TRUE(
		move.Has("tgm_relayout_waits(r.space.vm, tgm_chan_uid(chan), writers, nwriters, "
			 "waits, 0)"));
	// its wait is late on the writer an acquire sits on, so recovery finds that writer, not the copy behind it
	EXPECT_TRUE(move.Has("acquired[i] = {waits[i].uid, waits[i].seqno};"));
	EXPECT_TRUE(move.Has("tgm_copy_wait(tgm_chan_uid(chan), seqno, acquired, nwaits);"));
	EXPECT_FALSE(move.Has("tgm_writer_wait("));
	const CodeText late = Block(gpu(), "static struct tgm_chan *late_writer(");
	EXPECT_TRUE(late.Has("(s32)(tgm_chan_completed(chan) - acquired[i].seqno) < 0"));
	EXPECT_TRUE(late.Has("*late_seqno = seqno; return live_by_uid(uid);"));
	EXPECT_TRUE(
		Block(gpu(), "static void writer_wait(")
			.Has("chan = late_writer(uid, seqno, acquired, nacquired, &late); if "
			     "(chan) verdict = tgm_hang_recover(chan, \"IOSurface lock\", late, "));
	EXPECT_TRUE(CodeText(relayout).Has(
		"{\"relayout\", TGM_FENCE_KERNEL_RELAYOUT, 1, kPushVa, 0, 1}"));
	EXPECT_TRUE(Block(SourceText("../SwitchGpuKqueue.h"), "bool openSpace(")
			    .Has("tgm_fence_kernel(shape.fenceSlot, &fenceCpu, &fencePage, "
				 "&fenceOffset)"));
	const CodeText copy(SourceText("../TegraGM20BCopy.cpp"));
	EXPECT_TRUE(copy.Has("switch_gpu_backing_dependencies(&transaction, queue.dependencies,"));
	EXPECT_TRUE(copy.Has("queue.events.captureDependencies(queue.dependencies, count)"));
	EXPECT_TRUE(copy.Has("switch_gpu_chan_exec_visibility("));
	EXPECT_FALSE(copy.Has("tgm_writer_wait("));
	EXPECT_TRUE(copy.Has("{\"ce-copy\", TGM_FENCE_KERNEL_COPY, Jobs, PushVa, 1, 0}"));
	const CodeText &fences = gpuCode();
	EXPECT_TRUE(fences.Has("#define TGM_FENCE_PAGES (TGM_FENCE_USER_PAGES + 1)"));
	EXPECT_TRUE(Block(gpu(), "int tgm_fence_page_get(volatile uint32_t **va, uint64_t *pa)")
			    .Has("i = (tgm_fence_pages.next + n) % TGM_FENCE_USER_PAGES;"));
	EXPECT_TRUE(Block(gpu(), "int tgm_fence_kernel(")
			    .Has("TGM_FENCE_USER_PAGES * TGM_FENCE_SLOT + which * sizeof(struct "
				 "TegraGM20BAbiFence);"));
	EXPECT_TRUE(Block(gpu(), "int tgm_writer_fence_va(").Has("off < TGM_FENCE_BYTES"));
}

TEST(GM20BSurfaceIdentitySource, ImportedSurfaceIdentityOutlivesLegacyCoherency)
{
	const CodeText imported = Block(client(), "IOReturn AGXDeviceUserClient::boImport(");
	EXPECT_TRUE(
		imported.Before("bo->surface = switch_gpu_surface_hold(", "tgm_coherency_add("));
	const CodeText released = Block(client(), "void AGXDeviceUserClient::releaseBo(Bo *bo)");
	EXPECT_TRUE(
		released.Before("tgm_coherency_put(bo->coh);", "OSSafeReleaseNULL(bo->surface);"));
}

// A held command buffer's writer promise (TegraGM20BMethodWriterPromise) is the connection's: method 34 reaches it under
// the connection lock with the submit gate open, it names writes of the connection's IOSurface imports only, and each
// release from the CPU reaches a parked GPU reader through the write-combined fence page.
TEST(GM20BJobSource, AWriterPromiseIsTheConnectionsAndEndsWithIt)
{
	const CodeText run = Block(client(), "IOReturn AGXDeviceUserClient::run(uint32_t method,");
	EXPECT_TRUE(run.Has(
		"case TegraGM20BMethodWriterPromise: wantIn = sizeof(TegraGM20BAbiWriterPromise); "
		"wantOut = sizeof(TegraGM20BAbiWriterPromiseReply);"));
	EXPECT_TRUE(run.Has("submitGate_.closed() ? kIOReturnNotOpen : writerPromise("));
	const std::string promises = SourceText("../TegraGM20BUserClientPromise.cpp");
	const CodeText promise =
		Block(promises, "IOReturn AGXDeviceUserClient::writerPromise(const "
				"TegraGM20BAbiWriterPromise *in,");
	EXPECT_TRUE(promise.Has("if (!bo || !bo->backing || !bo->surface || uses[i].access != "
				"TegraGM20BSurfaceWrite"));
	EXPECT_FALSE(promise.Has("switch_gpu_promise_release("));
	const CodeText release =
		Block(promises,
		      "void AGXDeviceUserClient::releasePromise(uint32_t promise, bool abandoned)");
	EXPECT_TRUE(release.Before(
		"tgm_promise_abandon(offset, value);",
		"switch_gpu_promise_release(&promises_[promise], promiseWord(promise))"));
	EXPECT_TRUE(release.Before(
		"switch_gpu_promise_release(&promises_[promise], promiseWord(promise))",
		"__asm__ volatile(\"dsb sy\" ::: \"memory\");"));
	EXPECT_TRUE(Block(client(), "void AGXDeviceUserClient::drainResources()")
			    .Before("releasePromises(true);", "tgm_chan_del(&chan);"));
}

// A direct present whose producer still runs at the offer has no job (SwitchGpuPresentWatch.h): the producer's own
// interrupt pass makes it due with plain loads, the pass's end delivers it with interrupts on, and a kill, a hidden
// producer and the deadline go to the work item. The arm comes before the offer's own look.
TEST(GM20BPresentSource, AWatchedPresentIsCompletedByItsProducersInterruptPass)
{
	const CodeText fenced = Block(gpu(), "static inline void present_watch_fenced(");
	EXPECT_TRUE(fenced.Has("switch_gpu_present_watch_fenced(&tgm_present.watch, chan->id, "
			       "tgm_chan_completed(chan))"));
	EXPECT_FALSE(fenced.Has("mutex_lock"));
	EXPECT_FALSE(fenced.Has("finish_visible"));
	const CodeText killed =
		Block(gpu(), "int tgm_chan_killed(struct nvif_event *event, void *repv, u32 repc)");
	EXPECT_TRUE(killed.Before(
		"__atomic_store_n(chan->fence_cpu, chan->submitted, __ATOMIC_RELEASE);",
		"present_watch_fenced(chan, 1);"));
	const CodeText pass =
		Block(SourceText("../shim/xnu_glue.cpp"),
		      "void gpuInterrupt(OSObject *, IOInterruptEventSource *source, int)");
	EXPECT_TRUE(pass.Before("nvxnu_poll_irqs();", "tgm_present_pass();"));
	EXPECT_TRUE(pass.Before("tgm_present_pass();", "nvxnu_wake();"));
	const CodeText deliver = Block(gpu(), "void tgm_present_pass(void)");
	EXPECT_TRUE(deliver.Has("switch_gpu_present_watch_take_visible("));
	// the pass's own thread: nothing may wait for a GPU interrupt
	EXPECT_TRUE(deliver.Has("switch_gpu_present_tokens_finish_ready("));
	EXPECT_FALSE(deliver.Has("finish_visible"));
	EXPECT_FALSE(deliver.Has("_wait("));
	EXPECT_FALSE(Block(SourceText("../SwitchGpuPresentTokens.h"),
			   "switch_gpu_present_tokens_finish_ready(")
			     .Has("token_wait"));
	EXPECT_TRUE(deliver.Has("nvxnu_work_schedule(tgm_present.watch_work);"));
	EXPECT_FALSE(deliver.Has("mutex_lock"));
	const CodeText arm = Block(
		gpu(), "int tgm_present_watch_dependencies(void (*done)(void *, int), void *arg,");
	EXPECT_TRUE(
		arm.Before("switch_gpu_present_watch_arm(", "mutex_unlock(&tgm_present.lock);"));
	EXPECT_TRUE(arm.Before("gate_leave();", "present_physical(&tgm_present.watch.token)"));
	// every producer is asked, of the channel its token names, between the arm and the last look: the job that keeps a
	// writer promise in the promise's place once it is submitted
	const char *const ask = "tgm_chan_need_tokens(kept.engine ? &kept : dependencies, "
				"kept.engine ? 1 : dependency_count);";
	EXPECT_TRUE(arm.Before("switch_gpu_present_watch_arm(", ask));
	EXPECT_TRUE(arm.Before("gate_leave();", ask));
	EXPECT_TRUE(arm.Before("tgm_present_promise_watch(&kept);",
			       "switch_gpu_present_watch_promise("));
	// the job that keeps a watched promise takes its place, retained before the watch is handed over
	const CodeText kept = Block(SourceText("../gpu_promise.c"),
				    "void tgm_present_promise_kept(uint32_t offset, uint32_t "
				    "value, const struct switch_gpu_backing_token *producer)");
	EXPECT_TRUE(kept.Before("!switch_gpu_backing_token_retain(producer)",
				"switch_gpu_present_watch_kept("));
	EXPECT_TRUE(kept.Before("switch_gpu_present_watch_rearm(&tgm_present.watch);",
				"tgm_chan_need_tokens(producer, 1);"));
	EXPECT_TRUE(kept.Before("tgm_chan_need_tokens(producer, 1);",
				"switch_gpu_backing_token_physical_status(producer, 0)"));
	EXPECT_TRUE(
		arm.Before("tgm_chan_need_tokens(", "present_physical(&tgm_present.watch.token)"));
	EXPECT_FALSE(arm.Has("tgm_chan_need_uid("));
	EXPECT_TRUE(
		arm.Has("nvxnu_work_schedule_after(tgm_present.deadline_work, TGM_PRESENT_NS);"));
	const CodeText outstanding = Block(gpu(), "int tgm_present_outstanding(const void *arg)");
	EXPECT_TRUE(outstanding.Has("switch_gpu_present_watch_armed(&tgm_present.watch)"));
	// the deadline never delivers a watch: it follows the work an asked interrupt is behind, or panics for a lost one
	const CodeText deadline = Block(gpu(), "void tgm_present_deadline(void *arg)");
	EXPECT_FALSE(deadline.Has("present_watch_work("));
	EXPECT_TRUE(deadline.Before(
		"watched = switch_gpu_backing_token_physical_status(&tgm_present.watch.token, 0);",
		"mutex_lock(&tgm_live_lock);"));
	EXPECT_TRUE(deadline.Before("if (watch_late(watched, age, &waited, &seqno)) {",
				    "tgm_hang_recover("));
	const CodeText late = Block(gpu(), "static int watch_late(");
	EXPECT_TRUE(late.Before("switch_gpu_quiet_interrupt(", "nvxnu_panic("));
	EXPECT_TRUE(late.Has("if (now - tgm_present.late < TGM_PRESENT_WAKE_GRACE_NS) {"));
	EXPECT_FALSE(late.Has("watch_claim"));
	EXPECT_FALSE(late.Has("watch_work"));
	EXPECT_TRUE(
		Block(gpu(), "int tgm_present_watch_dependencies(").Has("tgm_present.late = 0;"));
	const CodeText del = Block(gpu(), "void tgm_chan_del(struct tgm_chan **pchan)");
	EXPECT_TRUE(del.Before("chan_abandon(chan);", "present_watch_fenced(chan, 1);"));
	EXPECT_TRUE(del.Before("present_watch_fenced(chan, 1);", "list_del(&chan->live);"));
	EXPECT_TRUE(deadline.Has(
		"if (tgm_present.pending && (s32)(tgm_chan_completed(tgm_present.chan) - "
		"tgm_present.pending) >= 0)"));
	const CodeText offer = Block(client(), "extern \"C\" int tegra_gm20b_scanout_copy(");
	EXPECT_TRUE(offer.Has("ret = watch && !nreads ? presentWatched() : 1;"));
	EXPECT_TRUE(offer.Has("ret = watch ? presentWatched() : 1;"));
}

// A context-switch timeout on the present's channel resets the copy engine and copies the frame again on the GPU: the cut
// present's completion waits for the redo, so the display never gets a partial-frame status and never copies on the CPU.
// Scrub jobs a reset cut go back dirty.
TEST(GM20BPresentSource, ACopyEngineResetRedoesThePresentOnTheGpu)
{
	const CodeText fenced = Block(gpu(), "static inline void present_fenced(");
	EXPECT_TRUE(fenced.Before(
		"redo = pending; if (__atomic_compare_exchange_n(&tgm_present.redo, &redo, 0,",
		"nvxnu_work_schedule(tgm_present.completion_work);"));
	EXPECT_TRUE(
		fenced.Before("__atomic_load_n(&tgm_present.redoing, __ATOMIC_ACQUIRE)) return;",
			      "redo = pending; if ("));
	EXPECT_TRUE(fenced.Before("nvxnu_work_schedule(tgm_present.redo_work); return; }",
				  "nvxnu_work_schedule(tgm_present.completion_work);"));
	const CodeText redo = Block(gpu(), "void tgm_present_redo_work(");
	EXPECT_TRUE(redo.Before("|| !gate_try()) return;", "tgm_present_push_copy(push);"));
	EXPECT_TRUE(redo.Before("tgm_present_push_copy(push);", "chan_fence_volatile(chan);"));
	EXPECT_TRUE(redo.Before("__atomic_store_n(&tgm_present.redoing, 0, __ATOMIC_RELEASE);",
				"#endif mutex_unlock(&tgm_present.lock); gate_leave();"));
	EXPECT_TRUE(Block(gpu(), "static void gate_open(").Has("tgm_present_redo_kick();"));
	EXPECT_FALSE(redo.Has("done(arg"));
	EXPECT_FALSE(redo.Has("tgm_present.done"));
	EXPECT_FALSE(redo.Has("SEMAPHORED"));
	// a redo with no room in the ring goes to the hang rule and is tried again, never a panic
	EXPECT_TRUE(redo.Before(
		"tgm_chan_stalled(chan, \"present redo: push buffer still full after 1 s\"",
		"nvxnu_work_schedule(tgm_present.redo_work);"));
	EXPECT_FALSE(redo.Has("nvxnu_panic("));
	const CodeText mark = Block(gpu(), "int tgm_present_redo(");
	EXPECT_TRUE(mark.Before("__atomic_store_n(&tgm_present.redo, pending, __ATOMIC_SEQ_CST);",
				"if (__atomic_load_n(&tgm_present.pending, __ATOMIC_SEQ_CST) == "
				"pending) return 1;"));
	EXPECT_TRUE(Block(gpu(), "static inline void scrub_fenced(")
			    .Has("nvxnu_pool_free_dirty(tgm_scrub.job[slot].pa, "
				 "tgm_scrub.job[slot].bytes);"));
	const CodeText decide = Block(gpu(), "enum tgm_ctxsw tgm_ctxsw_decide(");
	// a stuck kernel channel never panics here: tgm_kernel_stall_locked decides, and marks the redo before the reset
	EXPECT_TRUE(decide.Has(
		"if (chan->kernel && (verdict == TGM_CTXSW_REDO || verdict == TGM_CTXSW_FATAL)) "
		"verdict = tgm_kernel_stall_locked(chan, engine_name, verdict == TGM_CTXSW_FATAL, "
		"0, 1, &killed);"));
	EXPECT_FALSE(decide.Has("nvxnu_panic("));
	const CodeText stall = Block(gpu(), "enum tgm_ctxsw tgm_kernel_stall_locked(");
	EXPECT_TRUE(stall.Before("if (verdict == TGM_CTXSW_FATAL) {", "*killed = kill_clients("));
	EXPECT_TRUE(stall.Before("*killed = kill_clients(", "tgm_ctxsw_redo_mark(chan);"));
	EXPECT_TRUE(stall.Has("if (!fatal_ok) return verdict;"));
	// an idle channel the engine cannot switch off has no drain to wait for: a kernel one is reset, a client's killed,
	// at the first tick
	EXPECT_TRUE(decide.Has(
		"chan_stuck_for(chan, now, tgm_stuck_window(gr)) || chan_unowned(chan)"));
	// progress after the reset: the redone work landing, or with nothing to redo any job completing
	EXPECT_TRUE(Block(gpu(), "void tgm_ctxsw_redo_mark(")
			    .Has("chan->ctxsw_reset_after = marked ? chan->submitted : "
				 "tgm_chan_completed(chan);"));
}

// gpu.recover.kernel_second_timeout's hook takes each cut of the present as a tick takes a stuck kernel channel, the second
// before the first's redo landed, and gets the hardware-failure verdict back instead of its panic
TEST(GM20BPresentSource, TheCopyResetHookCutsTwiceThroughTheKernelStallRuleAndNeverPanics)
{
	const std::string hook = SourceText("../gpu_copy_reset.c");
	EXPECT_TRUE(Block(hook, "static void copy_reset_mark(")
			    .Has("again == AGAIN_REDO, 0, 0, &killed);"));
	EXPECT_FALSE(CodeText(hook).Has("nvxnu_panic("));
	const CodeText redo = Block(gpu(), "void tgm_present_redo_work(");
	EXPECT_TRUE(redo.Before("#if TGM_STATS again = (u32)tgm_copy_reset_again_take(); #endif",
				"chan_push_wait("));
	EXPECT_TRUE(redo.Before("if (again) { PUSH_MTHD(push, NV906F, NON_STALL_INTERRUPT, 0);",
				"chan_fence_volatile(chan);"));
}

// A context load an engine is stuck in again after a reset under it: a client's is that client's alone and kills it, a
// kernel channel's goes to the kernel stall rule, whose only panic is the engine that did not come back
TEST(GM20BRecoverSource, ASecondStuckLoadKillsTheClientOrGoesToTheKernelStallRule)
{
	const CodeText load = Block(gpu(), "static enum tgm_ctxsw load_verdict(");
	EXPECT_TRUE(load.Before("if (chan->kernel) return tgm_kernel_stall_locked(chan, engine, "
				"verdict == TGM_CTXSW_FATAL, 1, 1, &killed);",
				"tgm_chan_isolate(chan, report, nvxnu_nanotime());"));
	EXPECT_TRUE(load.Before("tgm_chan_isolate(chan, report, nvxnu_nanotime());",
				"return TGM_CTXSW_KILL;"));
	EXPECT_FALSE(load.Has("nvxnu_panic("));
	const CodeText stall = Block(gpu(), "enum tgm_ctxsw tgm_kernel_stall_locked(");
	EXPECT_EQ(stall.Count("nvxnu_panic("), 1u);
	EXPECT_TRUE(
		stall.Has("hardware failure: %s did not come back after a reset with no client "));
	EXPECT_TRUE(stall.Has("chan->ctxsw_clean_ns = n ? 0 : now;"));
	EXPECT_TRUE(Block(gpu(), "static u32 kill_clients(")
			    .Has("tgm_chan_isolate(clients[i], report, now); "
				 "tgm_recover_fault(tgm_gpu.device, (int)clients[i]->chid);"));
}

TEST(GM20BPresentSource, TypedProducerValidationAndReleaseNeverRunUnderTheIrqEventLock)
{
	const CodeText irq = Block(gpu(), "static inline void present_fenced(");
	EXPECT_TRUE(irq.Has("nvxnu_work_schedule(tgm_present.completion_work)"));
	EXPECT_FALSE(irq.Has("switch_gpu_present_tokens_finish"));
	const CodeText complete = Block(gpu(), "static void present_typed_complete(");
	EXPECT_TRUE(complete.Before("mutex_unlock(&tgm_present.lock);",
				    "switch_gpu_present_tokens_finish_visible("));
	EXPECT_TRUE(complete.Before("switch_gpu_present_tokens_finish_visible(",
				    "&tgm_present.typed_pending, 0"));
	const CodeText copy = Block(gpu(), "static int present_copy(");
	EXPECT_TRUE(copy.Before(
		"tgm_backing_dependencies(chan, tgm_present.tokens.tokens, dependency_count, 1)",
		"if (dst_va) tgm_present_push_copy(push);"));
	EXPECT_TRUE(copy.Before("&tgm_present.typed_pending, 1", "chan_fence(chan);"));
	EXPECT_TRUE(copy.Has("dependency_count * 5"));
	EXPECT_TRUE(copy.Has("push->cur = push_start"));
}

TEST(GM20BPresentSource, ExactFrameSkipsLiveChannelEnumerationEvenWithoutGpuTokens)
{
	const CodeText copy = Block(gpu(), "static int present_copy(");
	EXPECT_FALSE(copy.Has("exact"));
	EXPECT_FALSE(copy.Has("list_for_each_entry"));
	const CodeText exact = Block(gpu(), "int tgm_present_copy_dependencies(");
	EXPECT_TRUE(exact.Has("dependencies, dependency_count);"));
	const CodeText scanout = Block(client(), "extern \"C\" int tegra_gm20b_scanout_copy(");
	EXPECT_TRUE(scanout.Has("gPresentSnapshot.add(sourceBacking)"));
	EXPECT_TRUE(scanout.Has("gPresentSnapshot.tokens, gPresentSnapshot.count"));
}

TEST(GM20BVmSource, MappingSerializesPerAddressSpaceWithSuspendAdmission)
{
	for (const char *signature :
	     {"int tgm_vm_map_raw(", "int tgm_vm_unmap_pages(", "int switch_gpu_vm_map_volatile(",
	      "void switch_gpu_vm_unmap(", "int tgm_vm_kmap_raw("}) {
		const CodeText body = Block(gpu(), signature);
		EXPECT_TRUE(body.Before("gate_enter();", "mutex_lock(&vm->map_lock);"));
		EXPECT_TRUE(body.Before("mutex_unlock(&vm->map_lock);", "gate_leave();"));
		EXPECT_FALSE(body.Has("gpu_lock();"));
		EXPECT_FALSE(body.Has("mem_prepare_big_views("));
	}
	const CodeText memory = Block(gpu(), "static int mem_new(");
	EXPECT_TRUE(memory.Before("gpu_lock();", "mem_prepare_big_views(mem);"));
	EXPECT_TRUE(memory.Before("mem_prepare_big_views(mem);", "mutex_unlock(&tgm_gpu.lock);"));
	EXPECT_TRUE(Block(gpu(), "void tgm_mem_del(").Has("gpu_lock();"));
	EXPECT_TRUE(Block(gpu(), "void tgm_vm_del(").Has("gpu_lock();"));
}

// A command buffer's batches are one SubmitAddress (TegraGM20BPushFence): its uses are owned against the seqno of the
// job's own fence, after every internal one (switch_gpu_job_fences_test runs the emission order).
TEST(GM20BJobFencesSource, OneJobOwnsItsUsesUntilItsLastFence)
{
	const CodeText exec = Block(client(), "IOReturn AGXDeviceUserClient::exec(");
	EXPECT_TRUE(exec.Has("const uint32_t fenceFlag = native && scratch ? "
			     "uint32_t(TegraGM20BPushFence) : 0u;"));
	EXPECT_TRUE(exec.Has(
		"if (switch_gpu_job_fences(pushes, count, &fences)) return kIOReturnBadArgument;"));
	EXPECT_TRUE(exec.Has("switch_gpu_chan_job_token(chan, fences, &completion);"));
	EXPECT_FALSE(exec.Has("switch_gpu_chan_next_token("));
	// resolved once: a written import is marked from what the uses resolved, not looked up again
	EXPECT_TRUE(exec.Has("job.written[writtenCount++] = bo->coh;"));
	EXPECT_TRUE(
		exec.Has("tgm_coherency_wrote_listed(job.written[i], tgm_chan_uid(chan), seqno);"));
	EXPECT_FALSE(exec.Has("findAddress("));
	const CodeText emit = Block(gpu(), "int chan_exec_body(");
	const CodeText room = Block(gpu(), "static void chan_submission_room(");
	EXPECT_TRUE(
		room.Has("*entries = count + 6 + stamps + job_fences + CHAN_WAKE_ENTRIES + 1;"));
	EXPECT_TRUE(
		room.Has("SWITCH_GPU_FENCE_POINT_DWORDS * job_fences + CHAN_WAKE_DWORDS + 16 +"));
	EXPECT_TRUE(
		emit.Has("last = switch_gpu_job_emit(pushes, count, chan->submitted, &emitter);"));
	EXPECT_TRUE(Block(client(), "uint32_t switch_gpu_info_flags(void)")
			    .Has("TegraGM20BInfoPushFences;"));
}

// A use's handle is found by the index (switch_gpu_bo_index_test), never by walking the connection's BOs; every BO
// that joins bos_ joins the index, and every one that leaves it leaves the index.
TEST(GM20BBoIndexSource, EveryBoListChangeKeepsTheIndex)
{
	const CodeText find =
		Block(client(), "AGXDeviceUserClient::Bo *AGXDeviceUserClient::findBo(");
	EXPECT_TRUE(find.Has("return boIndex_.find(handle);"));
	const std::string sources = client() + SourceText("../SwitchGpuNativeImport.h") +
				    SourceText("../SwitchGpuClientRetirement.h");
	EXPECT_EQ(CodeText(sources).Count("bos_ = bo;"),
		  CodeText(sources).Count("boIndex_.insert(bo->handle, bo);"));
	EXPECT_EQ(CodeText(sources).Count("boIndex_.reserve()"), 3u);
	EXPECT_TRUE(Block(SourceText("../SwitchGpuClientRetirement.h"),
			  "IOReturn AGXDeviceUserClient::boFree(")
			    .Before("*p = bo->next;", "boIndex_.erase(bo->handle);"));
}

TEST(GM20BVisibilitySource, InternalFenceKeepsExecutionOrderAndExternalFencePublishesVisibleFirst)
{
	const CodeText fence = Block(gpu(), "static u32 chan_fence_job(");
	EXPECT_TRUE(fence.Has("if (external && (l2_on() || chan->kernel))"));
	EXPECT_TRUE(fence.Before("NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY",
				 "if (external && chan->visible_cpu)"));
	EXPECT_TRUE(
		fence.Before("lower_32_bits(chan->visible_gpu)", "lower_32_bits(chan->fence_gpu)"));
	EXPECT_TRUE(fence.Has("RELEASE_WFI, EN"));
	const CodeText exec = Block(gpu(), "int chan_exec_body(");
	EXPECT_TRUE(exec.Has("if ((l2_on() && host_written) || tlb)"));
	EXPECT_TRUE(exec.Before("NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY",
				"NVB06F_MEM_OP_D_OPERATION_L2_SYSMEM_INVALIDATE"));
	EXPECT_TRUE(exec.Has(
		"*seqno = chan_fence_job(chan, external, chan->submitted + 1, last, quiet, "
		"releases, release_count);"));
	// A batch's internal fence orders execution only: no L2 flush, no visible release, no interrupt. Its kick is the
	// GPFIFO entry that keeps it between the batches; the job's own kick writes GP_PUT.
	const CodeText internal = Block(gpu(), "static void chan_job_fence(");
	EXPECT_TRUE(internal.Has("RELEASE_WFI, EN"));
	EXPECT_FALSE(internal.Has("L2_FLUSH_DIRTY"));
	EXPECT_FALSE(internal.Has("visible_gpu"));
	EXPECT_FALSE(internal.Has("NON_STALL_INTERRUPT"));
	EXPECT_FALSE(internal.Has("defer_kick"));
	EXPECT_TRUE(internal.Before("RELEASE_SIZE, 4BYTE));", "PUSH_KICK(push);"));
	EXPECT_TRUE(exec.Before("chan->chan.defer_kick = true;", "last = switch_gpu_job_emit("));
	EXPECT_TRUE(fence.Before("chan->chan.defer_kick = false;", "PUSH_KICK(push);"));
	EXPECT_TRUE(fence.Before(
		"job = switch_gpu_jobs_kick(&chan->jobs, first, next, chan->submit_ns, !announce);",
		"PUSH_KICK(push);"));
	for (const char *signature : {"int tgm_vm_unmap_pages(", "void switch_gpu_vm_unmap("}) {
		const CodeText unmap = Block(gpu(), signature);
		EXPECT_TRUE(
			unmap.Before("nvkm_ltc_flush(tgm_gpu.device->ltc)", "nvif_vmm_raw_unmap("));
	}
}

TEST(GM20BVisibilitySource, ColdGlobalInvalidationPreservesInternalDirtyData)
{
	const CodeText body = Block(SourceText("../nouveau/nvkm/subdev/ltc/base.c"),
				    "nvkm_ltc_invalidate(struct nvkm_ltc *ltc)");
	EXPECT_TRUE(body.Before("mutex_lock(&ltc->mutex)", "ltc->func->flush(ltc)"));
	EXPECT_TRUE(body.Before("ltc->func->flush(ltc)", "ltc->func->invalidate(ltc)"));
	EXPECT_TRUE(body.Before("ltc->func->invalidate(ltc)", "mutex_unlock(&ltc->mutex)"));
}

// nvkm_runl_chan_get_chid returns with spinlocks held and interrupts off; nvkm_kmap takes ltc->mutex.
TEST(GM20BChannelSource, RamfcIsMappedOnlyAfterTheChannelLookupUnlocks)
{
	const CodeText lookup = Block(gpu(), "static struct nvkm_gpuobj *chan_inst(");
	EXPECT_TRUE(lookup.Before("nvkm_runl_chan_get_chid(", "nvkm_chan_put(&nchan, flags)"));
	for (const char *signature : {"void tgm_ramfc_write(", "static int ramfc_read_waits("}) {
		const CodeText body = Block(gpu(), signature);
		EXPECT_TRUE(body.Before("chan_inst(chid)", "nvkm_kmap("));
		EXPECT_TRUE(body.Before("nvkm_kmap(", "nvkm_done("));
	}
	EXPECT_TRUE(Block(gpu(), "static void chan_timeslice(").Has("tgm_ramfc_write("));
}

TEST(GM20BChannelSource, CopyChannelCreatesOnlyRequestedObjectsAndKeepsVerifiedRunlist)
{
	const CodeText create = Block(gpu(), "static int chan_new(");
	EXPECT_TRUE(create.Has("NV_DEVICE_HOST_RUNLIST_ENGINES_GR"));
	EXPECT_TRUE(create.Has("args.chan.runq = 0"));
	EXPECT_TRUE(create.Has("i = switch_gpu_channel_first_object(copy_only); i < 5"));
	EXPECT_TRUE(create.Has("switch_gpu_channel_bind_words(bind_words, classes, copy_only)"));
	EXPECT_TRUE(create.Has("chan_push_wait(chan, bind_count)"));
	EXPECT_TRUE(create.Has("PUSH_DATA(push, bind_words[i])"));
	const CodeText objects = create.Between("i = switch_gpu_channel_first_object(copy_only)",
						"host->version = 0");
	EXPECT_TRUE(CodeText(objects).Before("if (ret)", "goto fail"));
	EXPECT_TRUE(gpuCode().Has("tgm_chan_new_copy(tgm_present.vm"));
	EXPECT_TRUE(gpuCode().Has("tgm_chan_new_copy(tgm_scrub.vm"));
	EXPECT_TRUE(
		CodeText(SourceText("../SwitchGpuKqueue.h")).Has("tgm_chan_new_copy(vm, owner"));
	for (const char *producer : {"../TegraGM20BCopy.cpp", "../TegraGM20BRelayout.cpp",
				     "../SwitchGpuCachedZero.cpp", "../SwitchGpuBridge.cpp"}) {
		EXPECT_TRUE(CodeText(SourceText(producer)).Has("space.openChannel(kShape"));
		EXPECT_FALSE(CodeText(SourceText(producer)).Has("tgm_chan_new_copy("));
	}
}

// Every channel's acquire on another channel's fence reads the address space's view of the fence pool: a space without
// it would refuse every cross-channel dependency for its life, so it is not made.
TEST(GM20BVmSource, AnAddressSpaceWithoutItsFenceViewIsNotMade)
{
	const CodeText make = Block(gpu(), "int tgm_vm_new(struct tgm_vm **pvm)");
	EXPECT_TRUE(make.Before("ret = tgm_fences_map(vm);", "tgm_vm_del(&vm);"));
	EXPECT_FALSE(make.Has("vm->fences_va = 0;"));
}

// A client unmaps a pool BO after its BoFree: the pages wait for that unmap, which releases them, and are not a leak.
TEST(GM20BPoolSource, FreedPoolPagesGoWithTheirLastMapping)
{
	EXPECT_FALSE(Block(client(), "void AGXDeviceUserClient::releaseBo(Bo *bo)")
			     .Has("still mapped by its"));
	for (const char *signature : {"IOReturn AGXDeviceUserClient::unmapToken(",
				      "void AGXDeviceUserClient::releaseMappings()"})
		EXPECT_TRUE(Block(client(), signature).Has("scheduleOrphanRelease();"));
}

// Pool allocations take zeroed pages only (nvxnu_pool_alloc_ready, chunks): the refusal rules count those.
TEST(GM20BPoolSource, PoolRefusalsCountOnlyZeroedFreePages)
{
	EXPECT_TRUE(Block(gpu(), "uint64_t tgm_pool_avail(void)").Has("nvxnu_pool_ready_pages()"));
}

// A layout decision may move the surface on the copy engine and wait for it: the connection's lock and mutation gate
// are not held meanwhile, and what it uses is held for it.
TEST(GM20BLayoutSource, LayoutDecisionHoldsNeitherTheConnectionLockNorItsGate)
{
	const CodeText info = Block(client(), "IOReturn AGXDeviceUserClient::surfaceLayoutInfo(");
	EXPECT_TRUE(info.Before("tgm_coh_hold_asker(coh);", "IOLockUnlock(lock_);"));
	EXPECT_TRUE(info.Before("IOLockUnlock(lock_);", "tgm_coherency_layout(coh,"));
	EXPECT_TRUE(info.Before("tgm_coh_put_asker(coh);", "IOLockLock(lock_);"));
	const CodeText run = Block(client(), "IOReturn AGXDeviceUserClient::run(");
	EXPECT_TRUE(run.Has("method != TegraGM20BMethodSurfaceLayout &&"));
	EXPECT_TRUE(run.Has("method != TegraGM20BMethodSurfaceLayoutInfo;"));
}

// The asker's hold (gm20b_layout_ask_test) is the only reference the decision does not count as another user: it is
// taken once and put once, with no return between them.
TEST(GM20BLayoutSource, TheAskersHoldIsPutOnEveryPath)
{
	const CodeText info = Block(client(), "IOReturn AGXDeviceUserClient::surfaceLayoutInfo(");
	EXPECT_EQ(info.Count("tgm_coh_hold_asker("), 1u);
	EXPECT_EQ(info.Count("tgm_coh_put_asker("), 1u);
	EXPECT_FALSE(info.Has("tgm_coh_hold("));
	EXPECT_FALSE(
		info.Between("tgm_coh_hold_asker(coh);", "tgm_coh_put_asker(coh);").Has("return"));
}

// nvgpu's gr_gm20b_init_fs_state writes ZROP's and CROP's settings whole with the L2 slice count (0x12006c,
// pri_ringmaster_enum_ltc), not the FBP count gk104's helper reads.
TEST(GM20BRopSource, ZropAndCropCountTheL2SlicesAsNvgpuDoes)
{
	const std::string gr = SourceText("../nouveau/nvkm/engine/gr/gm20b.c");
	const CodeText init = Block(gr, "gm20b_gr_init_rop_active_ltcs(struct gf100_gr *gr)");
	EXPECT_TRUE(init.Has("nvkm_rd32(device, 0x12006c)"));
	EXPECT_TRUE(init.Has("nvkm_wr32(device, 0x408850, ltc_count);"));
	EXPECT_TRUE(init.Has("nvkm_wr32(device, 0x408958, ltc_count);"));
	EXPECT_TRUE(CodeText(gr).Has(".init_rop_active_fbps = gm20b_gr_init_rop_active_ltcs,"));
}

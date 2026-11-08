// SPDX-License-Identifier: MIT
// Guards on how TegraGM20B's kernel code uses what gm20b_pool_policy_test and gm20b_push_scan_test exercise, and on the
// pool's zeroing, which no host model runs (it lives in nvkm's shim and gpu.c's channels):
//  - the user client decides a coherent BO's memory through TegraGM20BPoolPolicy.h, serves a BO the pool does not from
//    ordinary pages under a write-combined physical view, never refuses a coherent BO for want of pool room, and the
//    stats line names the pool's holders;
//  - the CPU does not clear the reserved pool at boot; every page an allocation hands out is zero whether the CPU or
//    the copy engine zeroed it; the scrubber gives a range back clean only from its fence's interrupt pass, never waits
//    on the CPU, and panics with one line on a range not back in time;
//  - the user client reads pushes only through TegraGM20BPushScan.h and profiles only what it read;
//  - a fence-only present (direct scanout) copies nothing.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cctype>
#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::Count;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace
{
const std::string &client()
{
	static const std::string source = SourceText("../TegraGM20BUserClient.cpp");
	return source;
}

const std::string &gpu()
{
	static const std::string source = SourceText("../gpu.c");
	return source;
}

// The body of the function whose definition (not a declaration of it) starts with `signature`.
std::string definition(const std::string &source, const std::string &signature)
{
	for (size_t at = source.find(signature); at != std::string::npos;
	     at = source.find(signature, at + 1)) {
		const std::string rest = source.substr(at + signature.size());
		if (rest.substr(0, rest.find('{')).find(';') != std::string::npos)
			continue;
		return rest.substr(0, rest.find("\n}\n"));
	}
	ADD_FAILURE() << "no definition of " << signature;
	return {};
}

// `text` with every run of whitespace one space (the checks read code, not its line breaks).
std::string flat(const std::string &text)
{
	std::string out;
	bool space = false;
	for (const char c : text) {
		if (std::isspace((unsigned char)c)) {
			space = true;
			continue;
		}
		if (space)
			out += ' ';
		space = false;
		out += c;
	}
	if (space)
		out += ' ';
	return out;
}

// `first` occurs in `text`, and before `second`.
void expectBefore(const std::string &text, const char *first, const char *second)
{
	const size_t a = text.find(first), b = text.find(second);
	ASSERT_NE(a, std::string::npos) << first;
	ASSERT_NE(b, std::string::npos) << second;
	EXPECT_LT(a, b) << first << " before " << second;
}
}  // namespace

TEST(GM20BPoolPolicySource, BoAllocNeverRefusesACoherentBoForPoolRoom)
{
	EXPECT_THAT(client(), HasSubstr("#include \"TegraGM20BPoolPolicy.h\""));
	const std::string refusal = flat(definition(
		client(),
		"const char *poolRefusal(uint64_t bytes, uint64_t held, bool compositor)"));
	EXPECT_THAT(refusal, HasSubstr("tgm_pool::refusal("));
	EXPECT_THAT(refusal, HasSubstr("compositor);"));  // the compositor keeps nvkm's floor
	const std::string alloc =
		flat(definition(client(), "IOReturn AGXDeviceUserClient::boAlloc("));
	EXPECT_THAT(alloc, HasSubstr("refused = poolRefusal(in->bytes, pool_bytes_, critical_);"));
	EXPECT_THAT(alloc, HasSubstr("tgm_pool::coherentSource(refused, bo->pool_va != nullptr)"));
	// A refusal does not fail the BO: no NoMemory between the tier check and the pool allocation.
	EXPECT_THAT(Between(alloc, "if (!ret && (in->flags & TegraGM20BBoCoherent)) {",
			    "if (!ret && bo->pool_va)"),
		    Not(HasSubstr("kIOReturnNoMemory")));
	EXPECT_THAT(client(), Not(HasSubstr("GPU pool BoAlloc of %llu KiB refused")));
	// A system BO: a write-combined physical view over its pages.
	EXPECT_THAT(
		Between(alloc, "if (!ret && bo->bmd && system) {", "} else if (!ret && bo->bmd) {"),
		HasSubstr("kIOMemoryTypePhysical64) | kPoolCacheMode"));
}

TEST(GM20BPoolPolicySource, SystemBosGoAsPoolViewsDo)
{
	const std::string release =
		flat(definition(client(), "void AGXDeviceUserClient::releaseBo(Bo *bo)"));
	EXPECT_THAT(release, HasSubstr("if ((bo->pool_va || bo->system) && bo->view) orphan = "
				       "IONew(PoolOrphan, 1); if (orphan) {"));
	EXPECT_THAT(release, HasSubstr("bo->system ? bo->bmd : nullptr"));
	EXPECT_THAT(
		release,
		HasSubstr(
			"!bo->system && !(bo->flags & TegraGM20BBoContiguous) &&"));  // its spare goes by its orphan
	expectBefore(flat(definition(client(), "void releaseOrphans()")),
		     "orphan->view->release();", "orphan->backing->release();");
	// Never read through the cacheable alias: the stats and a hang's line both read pushes through pushView.
	EXPECT_THAT(flat(definition(client(), "const uint32_t *AGXDeviceUserClient::pushView(")),
		    HasSubstr("if (hit && !hit->bo->system) {"));
	EXPECT_THAT(flat(definition(client(), "void AGXDeviceUserClient::scanPushes(")),
		    HasSubstr("pushView(va, bytes, hit)"));
	EXPECT_THAT(flat(definition(client(), "int AGXDeviceUserClient::hungInterlocks(")),
		    HasSubstr("pushView(va[i], static_cast<uint64_t>(dwords[i]) * 4, hit)"));
}

// A system BO takes a ready spare as a cached BO does, and its pages go back to the spares once its view is released.
// Every source leaves the pages in one state: zero in DRAM, no line in the cacheable kernel alias (cleanZeros: KMA_ZERO's
// zeros cleaned and invalidated, or a recycled spare zeroed through the alias and then cleaned and invalidated).
TEST(GM20BPoolPolicySource, SystemBosTakeAndReturnSpares)
{
	const std::string alloc =
		flat(definition(client(), "IOReturn AGXDeviceUserClient::boAlloc("));
	EXPECT_THAT(alloc,
		    HasSubstr("if (!ret && !bo->pool_va && !(in->flags & TegraGM20BBoContiguous)) "
			      "spare = spareTake(in->bytes);"));
	EXPECT_THAT(alloc, Not(HasSubstr("|| system ? kClasses")));
	EXPECT_THAT(
		alloc,
		HasSubstr("(in->flags & TegraGM20BBoContiguous) ? kClasses : classUp(in->bytes);"));
	EXPECT_THAT(alloc, HasSubstr("cleanZeros(va, backing, false);"));
	EXPECT_THAT(flat(definition(client(), "Spare *spareMake(uint64_t bytes)")),
		    HasSubstr("cleanZeros(bmd->getBytesNoCopy(), bytes, false);"));
	const std::string work = flat(definition(
		client(), "void spareWork(thread_call_param_t param, thread_call_param_t)"));
	expectBefore(work, "cleanZeros(s->bmd->getBytesNoCopy(), s->bytes, true);",
		     "p->ready[c] = s;");
	const std::string zeros =
		flat(definition(client(), "void cleanZeros(void *va, uint64_t bytes, bool zero)"));
	expectBefore(zeros, "bzero(va, bytes);", "switch_dcache_clean_invalidate(va, bytes);");
	// A spare is only ever a BO's pages once no task maps them, nothing imports them (the view's release waits for
	// every import's UPL) and the freeing connection's channels finished (reap).
	const std::string orphans = flat(definition(client(), "void releaseOrphans()"));
	expectBefore(orphans, "if (orphan->view->getRetainCount() > 1) {",
		     "orphan->view->release();");
	expectBefore(orphans, "orphan->view->release();",
		     "spareKeep(orphan->backing, orphan->backingPas, orphan->backingBytes)");
	EXPECT_THAT(orphans, HasSubstr("orphan->zero &&"));
	EXPECT_THAT(flat(definition(client(), "bool spareKeep(IOBufferMemoryDescriptor *bmd, "
					      "uint64_t *pas, uint64_t bytes)")),
		    HasSubstr("bmd->getRetainCount() == 1"));
	const std::string release =
		flat(definition(client(), "void AGXDeviceUserClient::releaseBo(Bo *bo)"));
	expectBefore(release, "tgm_mem_del(&bo->mem);", "bo->system ? bo->pas : nullptr");
	EXPECT_THAT(release,
		    HasSubstr("bo->system ? static_cast<uint64_t>(bo->backing_pages) << 12 : 0"));
	const std::string free =
		flat(definition(client(), "IOReturn AGXDeviceUserClient::boFree("));
	expectBefore(free, "unbindOverlaps(binds_[i].va, binds_[i].range, bo);", "reap(false);");
}

// The import costs (the "imports" and "slow import" lines, the refused import's region) are the debug kernel's alone:
// a release kernel neither reads the thread's CPU time nor walks the task's map.
TEST(GM20BImportCostSource, ImportCostsAreDebugOnly)
{
	const std::string stats =
		Between(gpu(), "#if TGM_STATS\n/* The \"imports\" line's histograms", "#endif\n");
	EXPECT_THAT(stats, HasSubstr("thread_get_runtime_self()"));
	EXPECT_EQ(Count(gpu(), "thread_get_runtime_self"), Count(stats, "thread_get_runtime_self"));
	const std::string header = SourceText("../TegraGM20BGpu.h");
	EXPECT_THAT(
		Between(header, "#else\nstatic inline uint64_t tgm_stats_cpu_ns(void)", "#endif"),
		HasSubstr("return 0;"));
	const std::string import =
		flat(definition(client(), "IOReturn AGXDeviceUserClient::boImport("));
	EXPECT_THAT(import, HasSubstr("#if TGM_STATS describeRegion(task_, in->address, region, "
				      "sizeof(region)); #endif"));
	EXPECT_THAT(import, HasSubstr("tgm_mem_new_waited(vm_, bo->pas, bo->npages, &bo->mem, "
				      "&cost.lock_ns)"));
	EXPECT_THAT(flat(definition(gpu(), "static int mem_new(")),
		    HasSubstr("asked = stats_on() && lock_wait_ns ? nvxnu_nanotime() : 0;"));
}

TEST(GM20BPoolPolicySource, StatsNameThePoolHolders)
{
	const std::string stats = flat(gpu());
	EXPECT_THAT(
		stats,
		HasSubstr("tgm_pool_breakdown(&pool_clients, &pool_orphan_bytes, &pool_orphans, "
			  "&pool_system_bytes, &pool_systems);"));
	EXPECT_THAT(
		stats,
		HasSubstr(
			"pool %lu/%llu MiB (compositor %llu, clients \" \"%llu, orphans %llu in"));
}

TEST(GM20BPoolScrubSource, NoCpuClearOfTheReservedPool)
{
	const std::string init = definition(SourceText("../shim/xnu_glue.cpp"),
					    "extern \"C\" int nvxnu_pool_init(int *zeroed)");
	EXPECT_THAT(
		Between(init, "switch_ram_gpu_pool(&start, &end)", "IOBufferMemoryDescriptor *md"),
		Not(HasSubstr("memset")));
	EXPECT_THAT(init, HasSubstr("*zeroed = 0;"));
	EXPECT_THAT(init, HasSubstr("*zeroed = 1;"));  // the fallback still zeroes its own pages
	EXPECT_THAT(definition(SourceText("../shim/linux.c"), "int nvxnu_pool_setup(void)"),
		    HasSubstr("bitmap_set(pool_dirty, 0, pool_npages)"));
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
		EXPECT_THAT(definition(shim, signature), HasSubstr("pool_zero_dirty("))
			<< signature;
	const std::string zero = definition(
		shim, "static void pool_zero_dirty(unsigned long first, unsigned long n, int all)");
	EXPECT_THAT(zero, HasSubstr("test_bit(first + k, pool_dirty)"));
	EXPECT_THAT(zero, HasSubstr("bitmap_clear(pool_dirty, first, n)"));
	// A plain free marks pages dirty; only a zeroed free (CPU or scrubber) marks them clean.
	EXPECT_THAT(definition(shim, "static void pool_free(unsigned long first, unsigned long n)"),
		    HasSubstr("bitmap_set(pool_dirty, first, n)"));
	const std::string claim = definition(shim, "unsigned long nvxnu_pool_claim_dirty(unsigned "
						   "long max_bytes, unsigned long long *pa)");
	EXPECT_THAT(claim, HasSubstr("pool_dirty[w] & ~pool_map[w]"));
	EXPECT_THAT(
		claim,
		HasSubstr(
			"bitmap_set(pool_map, first, n)"));  // claimed ranges are allocated: nobody takes them
}

// A process maps whole VM pages (16 KiB, four pool pages): every pool BO and fence slot it can map is whole VM pages,
// so its mapping never reaches another BO's, another connection's fence or nvkm's own memory.
TEST(GM20BPoolIsolationSource, CpuMappedPoolMemoryIsWholeVmPages)
{
	const std::string alloc = flat(definition(
		gpu(), "void *tgm_pool_alloc(uint64_t bytes, uint64_t align, uint64_t *pa)"));
	EXPECT_THAT(alloc, HasSubstr("nvxnu_pool_alloc_clean(pool_bo_bytes(bytes), "
				     "max_t(u64, align, nvxnu_vm_page_size()), &h)"));
	EXPECT_THAT(definition(gpu(), "void tgm_pool_free(void *va, uint64_t bytes, uint64_t pa)"),
		    HasSubstr("pool_bo_bytes(bytes)"));
	EXPECT_THAT(definition(gpu(),
			       "void tgm_pool_free_zeroed(void *va, uint64_t bytes, uint64_t pa)"),
		    HasSubstr("bytes = pool_bo_bytes(bytes)"));
	EXPECT_THAT(definition(SourceText("../shim/linux.c"),
			       "void *nvxnu_pool_alloc_clean(unsigned long size, unsigned long "
			       "align, unsigned long long *pa)"),
		    HasSubstr("align >> PAGE_SHIFT"));
	EXPECT_THAT(
		definition(gpu(), "int tgm_fence_page_get(volatile uint32_t **va, uint64_t *pa)"),
		HasSubstr("i * TGM_FENCE_SLOT"));
	EXPECT_THAT(client(), HasSubstr("IOAddressRange range = {fence_pa_, "
					"static_cast<IOByteCount>(PAGE_SIZE)};"));
}

TEST(GM20BPoolScrubSource, ScrubberFreesOnlyFromItsFence)
{
	const std::string fenced =
		definition(gpu(), "static void scrub_fenced(struct tgm_chan *chan)");
	EXPECT_THAT(fenced, HasSubstr("nvxnu_pool_free_zeroed("));
	EXPECT_THAT(fenced, HasSubstr("tgm_chan_completed(chan)"));
	EXPECT_EQ(Count(gpu(), "nvxnu_pool_free_zeroed("), 2u)
		<< "tgm_pool_free_zeroed's and the scrubber's";
	const std::string submit = definition(gpu(), "static int scrub_submit(u64 pa, u64 bytes)");
	EXPECT_THAT(submit, HasSubstr("SET_REMAP_CONST_A, 0)"));
	EXPECT_THAT(
		submit,
		HasSubstr(
			"chan_fence(chan)"));  // WFI, FB flush, L2 flush, release, nonstall interrupt
	EXPECT_THAT(submit, HasSubstr("nvxnu_work_schedule_after(scrub.deadline, SCRUB_NS)"));
	EXPECT_THAT(submit, Not(HasSubstr("nvxnu_idle")));  // no CPU wait for the copy engine
	EXPECT_THAT(definition(gpu(), "static void scrub_deadline(void *arg)"),
		    HasSubstr("nvxnu_panic("));
	EXPECT_THAT(
		definition(
			gpu(),
			"static int chan_nonstall(struct nvif_event *event, void *repv, u32 repc)"),
		HasSubstr("chan->kernel == CHAN_SCRUB"));
}

TEST(GM20BPoolScrubSource, FreedPoolBosGoToTheGpuFirst)
{
	expectBefore(
		definition(client(), "bool poolZeroLater(void *va, uint64_t pa, uint64_t bytes)"),
		"tgm_pool_scrub(pa, bytes)", "spares()");
}

// The user client reads pushes only through the header gm20b_push_scan_test tests, and profiles only what it read.
TEST(GM20BPushScanSource, KernelUsesTheHeader)
{
	EXPECT_THAT(client(), HasSubstr("#include \"TegraGM20BPushScan.h\""));
	EXPECT_THAT(client(), Not(HasSubstr("void pushMethod(")));
	EXPECT_THAT(client(), Not(HasSubstr("void pushParse(")));
	const std::string scan =
		Between(client(), "void AGXDeviceUserClient::scanPushes(", "\n}\n");
	EXPECT_THAT(scan, HasSubstr("scanPush(c, p, whole, nullptr, 0)"));
	EXPECT_THAT(scan, HasSubstr("prof->cls[i] = cls;"));
	// A stamp only after a push whose class is not SKIP, only on a profiled submission.
	EXPECT_THAT(Between(gpu(), "static int chan_exec(", "\n}\n"),
		    HasSubstr("if (prof && prof->cls[i] != TGM_PC_SKIP) {"));
}

// Direct scanout's fence-only present (NativeMobileDisplay, TEGRA_SCANOUT_COPY_FENCE_ONLY) launches no copy.
TEST(GM20BPresentSource, FenceOnlyPresentHasNoCopy)
{
	expectBefore(Between(gpu(), "static int present_copy(", "\n}\n"), "if (dst_va) {",
		     "NVB0B5_LAUNCH_DMA_NON_PIPELINED");
}

// A tiled (block-linear) source: the copy engine reads it block-linear with the tiled block height, never as pitch, through
// a mapping of the kind its texels were written under; a surface's layout comes from its decision (SurfaceLayout), never
// from how one client bound it.
TEST(GM20BPresentSource, TiledSourceIsCopiedBlockLinear)
{
	const std::string copy = flat(Between(gpu(), "static int present_copy(", "\n}\n"));
	expectBefore(copy, "if (src_block) { PUSH_NVSQ(push, NVA0B5, NVB0B5_SET_SRC_BLOCK_SIZE",
		     "NVB0B5_LAUNCH_DMA");
	EXPECT_THAT(copy, HasSubstr("(src_block ? 0 : NVB0B5_LAUNCH_DMA_SRC_PITCH)"));
	const std::string surface = flat(Between(client(), "uint64_t presentSurface(", "\n}\n"));
	EXPECT_THAT(surface, HasSubstr("const uint32_t kind = blockLinear ? TGM_LAYOUT_KIND : 0;"));
	EXPECT_THAT(surface, HasSubstr("s.mem, bytes, static_cast<uint8_t>(kind))"));
	EXPECT_THAT(flat(Between(client(), "IOReturn AGXDeviceUserClient::vmBind(", "\n}\n")),
		    Not(HasSubstr("tiled")));
	EXPECT_THAT(
		flat(Between(client(), "IOReturn AGXDeviceUserClient::surfaceLayout(", "\n}\n")),
		HasSubstr("tgm_coherency_layout(bo->coh,"));
}

// System sleep (gpu.c "system sleep"): every path that touches the GPU outside gpu.lock goes through the gate
// gm20b_sleep_test runs, and sleep and wake keep the orders the hardware needs.
TEST(GM20BSleepSource, GpuTouchingPathsGoThroughTheGate)
{
	for (const char *waits : {"int tgm_chan_exec(", "int tgm_chan_new("}) {
		const std::string body = flat(definition(gpu(), waits));
		expectBefore(body, "gate_enter();", "gate_leave();");
	}
	for (const char *refused :
	     {"int tgm_present_copy(", "int tgm_pool_scrub(uint64_t pa, uint64_t bytes)",
	      "static void scrub_work(void *arg)"}) {
		const std::string body = flat(definition(gpu(), refused));
		expectBefore(body, "if (!gate_try())", "gate_leave();");
	}
	// the drain's and the wake's fence waits are woken by the scrubber's fences too
	EXPECT_THAT(flat(definition(gpu(), "static int chan_nonstall(struct nvif_event *event, "
					   "void *repv, u32 repc)")),
		    HasSubstr("if (tgm_gate_is_shut(&sc7_gate)) nvxnu_wake();"));
	// a pass for an interrupt taken before the GPU's power went reads nothing
	EXPECT_THAT(flat(definition(
			    SourceText("../shim/xnu_glue.cpp"),
			    "void gpuInterrupt(OSObject *, IOInterruptEventSource *source, int)")),
		    HasSubstr("if (__atomic_load_n(&gGpuOff, __ATOMIC_ACQUIRE)) return;"));
}

// Fence waits sleep on their channel's word: a fence interrupt wakes only the waiters of channels whose fence moved,
// a kill or a teardown wakes that channel's, and the interrupt pass's own wake is left to the kernel's waits.
TEST(GM20BFenceWakeSource, WaitersWakeByChannel)
{
	const std::string nonstall = flat(definition(
		gpu(), "static int chan_nonstall(struct nvif_event *event, void *repv, u32 repc)"));
	EXPECT_THAT(nonstall, HasSubstr("} else { chan_wake_waiters(chan); }"));
	EXPECT_EQ(Count(nonstall, "nvxnu_wake();"),
		  1u);  // the scrubber's, while the sleep gate is shut
	EXPECT_THAT(flat(definition(gpu(), "static void chan_wake_waiters(struct tgm_chan *chan)")),
		    HasSubstr("if (done == chan->woken_done) return;"));
	EXPECT_THAT(flat(definition(gpu(), "static int chan_killed(struct nvif_event *event, void "
					   "*repv, u32 repc)")),
		    HasSubstr("chan_wake_all_waiters(chan);"));
	expectBefore(flat(definition(gpu(), "void tgm_chan_del(struct tgm_chan **pchan)")),
		     "*pchan = NULL;", "nvxnu_word_wake(&waiter_words[waiter]);");
	EXPECT_THAT(flat(definition(
			    SourceText("../shim/xnu_glue.cpp"),
			    "void gpuInterrupt(OSObject *, IOInterruptEventSource *source, int)")),
		    HasSubstr("nvxnu_wake();"));
	const std::string wait =
		flat(Between(client(), "IOReturn AGXDeviceUserClient::waitFence(", "\n}\n"));
	expectBefore(wait, "seq = tgm_waiter_seq(waiter);", "tgm_chan_completed(chan) - in->seqno");
	EXPECT_THAT(wait, HasSubstr("tgm_waiter_idle(waiter, seq, deadline)"));
	EXPECT_THAT(wait, Not(HasSubstr("tgm_idle(")));
}

// A killed channel is dead alone (client fault isolation): what names it refuses, the connection's memory, binds and other
// channels go on, a new channel can be made in it, and nothing another channel waits for stays on the dead one.
TEST(GM20BKillSource, AKilledChannelLeavesItsConnectionWorking)
{
	const std::string run =
		flat(definition(client(), "IOReturn AGXDeviceUserClient::run(uint32_t method,"));
	EXPECT_THAT(client(), Not(HasSubstr("dead_")));
	EXPECT_THAT(run, Not(HasSubstr("tgm_chan_error(")));
	EXPECT_THAT(flat(definition(gpu(), "static int chan_exec(struct tgm_chan *chan,")),
		    HasSubstr("if (chan->error) return -EIO;"));
	const std::string order =
		flat(definition(gpu(), "int tgm_chan_order(struct tgm_chan *chan,"));
	EXPECT_THAT(order, HasSubstr("if (READ_ONCE(chan->error)) return -EIO;"));
	EXPECT_THAT(order, HasSubstr("READ_ONCE(other->error)) continue;"));
	const std::string killed = flat(definition(
		gpu(), "static int chan_killed(struct nvif_event *event, void *repv, u32 repc)"));
	EXPECT_THAT(killed, HasSubstr("WRITE_ONCE(*chan->fence_cpu, chan->submitted);"));
	EXPECT_THAT(killed, HasSubstr("chan->kernel || !tgm_all_clients()"));
	EXPECT_THAT(flat(definition(client(), "void AGXDeviceUserClient::reap(bool force)")),
		    HasSubstr("chans_[i] && !tgm_chan_error(chans_[i]) &&"));
}

// The raw path's reopen after its channel is killed: the shader heap's BO and VA outlive the channel, so every program
// placed in it (Device::place) keeps its slot and every state that baked its offset stays drawable; the new channel gets
// what lives in a channel's context again, the program region at the heap, the texture and sampler pools and the root
// constant buffer (emitContext, from the pack's relocations) and the 2D state, and the first command buffer's prologue
// invalidates the shader caches before any draw.
TEST(GM20BRawReopenSource, ReopenKeepsTheHeapAndReplaysTheContext)
{
	const std::string raw = SourceText("../../TegraGM20BMetal/raw/runtime/raw_gpu.cpp");
	const std::string reopen = flat(definition(raw, "bool Device::reopen()"));
	EXPECT_THAT(reopen, Not(HasSubstr("placed_")));
	EXPECT_THAT(reopen, Not(HasSubstr("heapPlaced_")));
	EXPECT_THAT(reopen, Not(HasSubstr("pipelines_")));
	EXPECT_THAT(reopen, Not(HasSubstr("heap_")));
	expectBefore(reopen, "k_.channel(kEngines, channel_, fence_)", "emitContext(p);");
	expectBefore(reopen, "emitContext(p);", "fx::emitTwodInit(p);");
	// the interlock's ticket buffer zeroed by the copy engine in the new channel, after the context names it and before
	// the 3D engine runs again
	expectBefore(reopen, "emitContext(p);",
		     "uint32_t(tickets_.va >> 32), uint32_t(tickets_.va)}, SUBC_COPY);");
	expectBefore(reopen, "uint32_t(tickets_.va)}, SUBC_COPY);",
		     "p.m(CE_LAUNCH_DMA, kLaunchPitch, SUBC_COPY);");
	expectBefore(reopen, "p.m(CE_LAUNCH_DMA, kLaunchPitch, SUBC_COPY);",
		     "p.m(WAIT_FOR_IDLE, 0);");
	const std::string context = flat(definition(raw, "void Device::emitContext(Push &p)"));
	EXPECT_THAT(context, HasSubstr("p.m(LOAD_CB_OFFSET, kRootInterlockTickets);"));
	// the init template relocated to this device's own addresses (raw_pack.cpp relocated, shared with the runtime's capture)
	EXPECT_THAT(context, HasSubstr("const InitAddresses here = {heap_.va, zero_.va, root_.va, "
				       "tic_.va, tsc_.va, kTicCount - 1, kTscCount - 1};"));
	EXPECT_THAT(context, HasSubstr("relocated(im[i], here)"));
	const std::string relocated = flat(
		definition(SourceText("../../TegraGM20BMetal/raw/runtime/raw_pack.cpp"),
			   "uint32_t relocated(const PackInitMethod &m, const InitAddresses &a)"));
	for (const char *reloc :
	     {"case RelocHeapHi:", "case RelocHeapLo:", "case RelocTicHi:", "case RelocTicLo:",
	      "case RelocTscHi:", "case RelocTscLo:", "case RelocRootHi:", "case RelocRootLo:"})
		EXPECT_THAT(relocated, HasSubstr(reloc));
	EXPECT_THAT(context, HasSubstr("p.inc(CB_SELECTOR_A,"));
	const std::string begin = flat(definition(raw, "CommandBuffer *Device::begin()"));
	expectBefore(begin, "!reopen())", "p.m(INVALIDATE_SHADER_CACHES_NO_WFI,");
}

// No spin for GPFIFO or push room: every wait for room sleeps until a fence's interrupt, and the fence reports GET to nvif
// before that interrupt, so the room the GPU freed is there when the waiter looks.
TEST(GM20BRoomSource, RoomIsWaitedForByInterrupt)
{
	EXPECT_EQ(Count(gpu(), "PUSH_WAIT("), 0u);
	EXPECT_EQ(Count(gpu(), "nvif_chan_gpfifo_wait("),
		  2u);  // the room check's comment and chan_wait_room
	const std::string wait = flat(definition(
		gpu(),
		"static int chan_wait_room(struct tgm_chan *chan, u32 gpfifo_nr, u32 push_nr)"));
	expectBefore(wait, "if (!nvxnu_idle(seq, end)",
		     "nvif_chan_gpfifo_wait(&chan->chan, gpfifo_nr, push_nr)");
	expectBefore(flat(definition(gpu(), "static u32 chan_fence(struct tgm_chan *chan)")),
		     "nvif_chan_gpfifo_post(&chan->chan);", "NON_STALL_INTERRUPT, 0);");
}

// The L2 invalidate that makes the CPU's writes visible is ordered on the GPU, in the submission's own stream, ahead of
// a first push that is not prefetched; the CPU does not wait for it. A channel behind its space's GPU-ordered maps
// invalidates its TLB after the L2 (the walk reads page tables through it) in the same entry.
TEST(GM20BL2Source, InvalidateOnTheGpu)
{
	EXPECT_EQ(Count(gpu(), "FLUSH_L2_SYSTEM_INVALIDATE"), 0u);
	const std::string exec = flat(Between(gpu(), "static int chan_exec(", "\n}\n"));
	expectBefore(exec, "tlb = tgm_tlb_take(&chan->tlb, &chan->vm->tlb);",
		     "NVB06F_MEM_OP_D_OPERATION_L2_SYSMEM_INVALIDATE);");
	expectBefore(
		exec, "NVB06F_MEM_OP_D_OPERATION_L2_SYSMEM_INVALIDATE);",
		"NVB06F_MEM_OP_D_OPERATION_MMU_TLB_INVALIDATE); } PUSH_KICK(&chan->chan.push);");
	expectBefore(
		exec,
		"NVB06F_MEM_OP_D_OPERATION_MMU_TLB_INVALIDATE); } PUSH_KICK(&chan->chan.push);",
		"pushes[i].no_prefetch || ((l2_on() || tlb) && !i));");
}

// A CPU IOSurfaceLock waits for the submissions that may write the surface: Exec names each one once it has its seqno.
TEST(GM20BCoherencySource, ExecNamesTheWriterOnceTheSubmissionHasItsSeqno)
{
	const std::string exec = flat(definition(client(), "IOReturn AGXDeviceUserClient::exec("));
	expectBefore(exec, "ioreturn(tgm_chan_exec(chan, pushes, count, &seqno,",
		     "surfaces_.after(listed, importsWritten_, tgm_chan_uid(chan), seqno);");
	const std::string after = flat(definition(SourceText("../TegraGM20BExecSurfaces.cpp"),
						  "void ExecSurfaces::after("));
	EXPECT_THAT(after, HasSubstr("tgm_coherency_wrote(refs_[i].coh, always, uid, seqno);"));
	// only a listed write is a writer another channel waits for on the GPU (gm20b_fences_test)
	EXPECT_THAT(after, HasSubstr("tgm_coherency_wrote_listed(r->coh, uid, seqno);"));
}

// Method 17's acquires go into the submission's own stream: queued (order_add, under live_lock) before tgm_chan_exec makes
// it, every exit of Exec past the channel check having taken the list; the decision is TegraGM20BFences.h's, which
// gm20b_fences_test runs.
TEST(GM20BFencesSource, ExecQueuesItsWritersAcquiresBeforeItSubmits)
{
	const std::string exec = flat(definition(client(), "IOReturn AGXDeviceUserClient::exec("));
	expectBefore(exec, "const bool listed = surfaces_.before(in->channel - 1, chan, count);",
		     "return kIOReturnBadArgument; if (p.flags & TegraGM20BPushImportsWritten)");
	expectBefore(exec, "surfaces_.before(", "ioreturn(tgm_chan_exec(");
	for (const char *signature :
	     {"int tgm_chan_follow(struct tgm_chan *chan,", "int tgm_chan_follow_importers("}) {
		const std::string follow = flat(definition(gpu(), signature));
		expectBefore(follow, "mutex_lock(&live_lock);", "tgm_fence_wanted(");
		expectBefore(follow, "tgm_fence_wanted(", "ret = order_add(chan,");
		expectBefore(follow, "ret = order_add(chan,", "mutex_unlock(&live_lock);");
	}
	EXPECT_THAT(flat(definition(client(), "IOReturn AGXDeviceUserClient::getInfo(")),
		    HasSubstr("| TegraGM20BInfoExecSurfaces"));
}

// A SurfaceWrites a client could not deliver never leaves a GPU write unmarked: the client stops saying its imports are
// only sampled and flags every submission TegraGM20BPushImportsWritten, which makes the connection mark every writable
// import at every Exec.
TEST(GM20BCoherencySource, AFailedSurfaceWritesFallsBackToMarkingEveryImport)
{
	const std::string exec = flat(definition(client(), "IOReturn AGXDeviceUserClient::exec("));
	expectBefore(exec, "importsWritten_ = true;",
		     "surfaces_.after(listed, importsWritten_, tgm_chan_uid(chan), seqno);");
	const std::string raw = SourceText("../../TegraGM20BMetal/raw/runtime/raw_kernel.cpp");
	const std::string declare = flat(definition(raw, "bool Kernel::surfaceGpuWrites("));
	EXPECT_THAT(declare, HasSubstr("importsWritten_.store(true"));
	EXPECT_THAT(flat(definition(raw, "bool Kernel::exec(")),
		    HasSubstr("e.pushes[0].flags |= TegraGM20BPushImportsWritten;"));
	EXPECT_THAT(flat(definition(raw, "bool Kernel::importPages(")),
		    HasSubstr("!importsWritten_.load("));
	const std::string nvk = SourceText(
		"../../../third_party/mesa/src/nouveau/vulkan/nvkmd/darwin/nvkmd_darwin.c");
	EXPECT_THAT(flat(definition(nvk, "switch_nvk_surface_gpu_writes(uint32_t surface_id)")),
		    HasSubstr("p_atomic_set(&surface_writes_failed, true);"));
	EXPECT_THAT(flat(definition(nvk, "nvkmd_darwin_exec_ctx_flush(struct nvkmd_ctx *_ctx,")),
		    HasSubstr("ctx->req.pushes[0].flags |= TegraGM20BPushImportsWritten;"));
	EXPECT_THAT(nvk,
		    HasSubstr("if (req.surface_id && !p_atomic_read(&surface_writes_failed))"));
	// The frontend and the raw path's kernel connection (raw_kernel.cpp) reach these with dlsym; Mesa builds hidden by
	// default. Without the connection's, the raw path opens no device and the render server stays on m2v alone.
	for (const char *exported :
	     {"__attribute__((visibility(\"default\"))) void\nswitch_nvk_import_surface(",
	      "__attribute__((visibility(\"default\"))) bool\nswitch_nvk_surface_gpu_writes(",
	      "__attribute__((visibility(\"default\"))) bool\nswitch_nvk_connection(",
	      "__attribute__((visibility(\"default\"))) void\nswitch_nvk_connection_release(",
	      "__attribute__((visibility(\"default\"))) void\nswitch_nvk_import_foreign("})
		EXPECT_THAT(nvk, HasSubstr(exported));
}

// NVK names a submission's host imports (method 17) before every Exec of it, a retry after kIOReturnBusy included, and a
// GPFIFO the kernel found full sleeps in WaitFence, never a timed poll.
TEST(GM20BFencesSource, NvkSendsItsSurfacesBeforeEachExecAndSleepsWhenFull)
{
	const std::string nvk = SourceText(
		"../../../third_party/mesa/src/nouveau/vulkan/nvkmd/darwin/nvkmd_darwin.c");
	const std::string flush =
		flat(definition(nvk, "nvkmd_darwin_exec_ctx_flush(struct nvkmd_ctx *_ctx,"));
	expectBefore(flush, "for (;;) {", "TegraGM20BMethodExecSurfaces");
	expectBefore(flush, "TegraGM20BMethodExecSurfaces", "TegraGM20BMethodExec,");
	EXPECT_THAT(flush, Not(HasSubstr("usleep")));
	expectBefore(flush, "if (!ctx->submitted) darwin_exec_busy_unsubmitted(ctx->channel);",
		     "darwin_wait_seqno(dev, ctx->channel, ctx->last_seqno,");
	const std::string queue =
		SourceText("../../../third_party/mesa/src/nouveau/vulkan/nvk_queue.c");
	const std::string submit =
		flat(definition(queue, "nvk_queue_submit(struct vk_queue *vk_queue,"));
	expectBefore(submit, "nvkmd_darwin_submit_surfaces(queue->exec_ctx, submit->driver_data);",
		     "result = nvk_queue_submit_exec(queue, submit);");
	expectBefore(submit, "result = nvk_queue_submit_exec(queue, submit);",
		     "nvkmd_darwin_submit_surfaces_end(queue->exec_ctx);");
	EXPECT_THAT(queue, HasSubstr("__attribute__((visibility(\"default\"))) "
				     "void\nswitch_nvk_submit_surfaces("));
	const std::string call = flat(definition(queue, "nvk_QueueSubmit2(VkQueue _queue,"));
	expectBefore(call, "nvkmd_darwin_submit_call(queue, true);", "vk_common_QueueSubmit2(");
	expectBefore(call, "vk_common_QueueSubmit2(", "nvkmd_darwin_submit_call(queue, false);");
	EXPECT_THAT(flat(queue),
		    HasSubstr("queue->vk.driver_submit_data = nvk_queue_submit_data;"));
	// Mesa gives each submission its data on the client's thread, before a submit thread can take it, and frees it with
	// the submission.
	const std::string runtime =
		SourceText("../../../third_party/mesa/src/vulkan/runtime/vk_queue.c");
	expectBefore(flat(definition(runtime, "vk_queue_submit(struct vk_queue *queue,")),
		     "submit->driver_data = queue->driver_submit_data(queue);",
		     "VK_QUEUE_SUBMIT_MODE_THREADED");
	EXPECT_THAT(flat(definition(runtime, "vk_queue_submit_free(struct vk_queue *queue,")),
		    HasSubstr("queue->driver_submit_data_free(submit->driver_data);"));
}

// Only a connection's maps skip the CPU's L2 and TLB maintenance; every removal keeps it (TegraGM20BTlb.h).
TEST(GM20BTlbSource, MapsOrderedOnTheGpuRemovalsSynchronous)
{
	EXPECT_THAT(client(), HasSubstr("tgm_vm_map_ordered(vm_, op.va"));
	EXPECT_EQ(Count(client(), "tgm_vm_map("), 0u);
	EXPECT_EQ(Count(gpu(), "nvxnu_gpu_ordered_begin();"), 1u);
	const std::string map = flat(definition(gpu(), "static int vm_map("));
	expectBefore(map, "nvxnu_gpu_ordered_end();", "nvif_vmm_raw_put(");
	const std::string unmap = flat(definition(gpu(), "int tgm_vm_unmap_pages("));
	EXPECT_THAT(unmap, Not(HasSubstr("gpu_ordered")));
	EXPECT_THAT(flat(definition(gpu(), "void tgm_vm_del(")),
		    HasSubstr("mmu_invalidate_all();"));
}

// Rail-gating (TegraGM20BRail.h): every other gpu.lock taker goes through gpu_lock(), which wakes a rail-gated GPU in
// the caller under the lock it took; the rail-up opens the gate before it clears `railed`, so a closer that has seen the
// GPU up never has its close undone (gm20b_rail_test's model).
TEST(GM20BRailSource, GpuLockWakesARailGatedGpu)
{
	EXPECT_EQ(Count(gpu(), "mutex_lock(&gpu.lock);"), 6u)
		<< "gpu_lock, tgm_timestamp_ns, tgm_sleep, tgm_rail_down, tgm_rail_up, "
		   "tgm_rail_sc7_hold";
	EXPECT_THAT(
		flat(definition(gpu(), "static noinline void gpu_lock(void)")),
		HasSubstr(
			"if (unlikely(gpu.railed)) rail_up_locked(__builtin_return_address(0));"));
	expectBefore(flat(definition(gpu(), "static void rail_up_locked(const void *site)")),
		     "gate_open();", "__atomic_store_n(&gpu.railed, 0, __ATOMIC_SEQ_CST);");
	// rail_up_locked takes these under the caller's gpu.lock: no gpu_lock() caller may hold one
	const std::string &text = gpu();
	for (size_t at = text.find("\tgpu_lock();"); at != std::string::npos;
	     at = text.find("\tgpu_lock();", at + 1)) {
		const size_t body = text.rfind("\n{\n", at);
		const std::string before = text.substr(body, at - body);
		for (const char *lock : {"(&live_lock)", "(idle.clk)", "(&gate_lock)"})
			EXPECT_EQ(Count(before, std::string("mutex_lock") + lock),
				  Count(before, std::string("mutex_unlock") + lock))
				<< lock << " held at the gpu_lock() at offset " << at;
	}
	// GPU time never backwards (gm20b_rail_test's model): the floor read before every power-down, PTIMER reseeded to it
	// after nvkm_timer_init, every time handed out clamped to it
	expectBefore(
		flat(definition(gpu(), "static void sleep_nvkm(void)")),
		"stamp_last = tgm_stamp_monotonic(stamp_last, nvkm_timer_read(device->timer));",
		"nvkm_device_fini(device, NVKM_SUSPEND)");
	expectBefore(flat(definition(gpu(), "static void wake_m1(void)")),
		     "ret = nvkm_subdev_init(&device->timer->subdev);", "stamp_reseed(device);");
	EXPECT_THAT(flat(definition(gpu(), "uint64_t tgm_timestamp_ns(void)")),
		    HasSubstr("stamp_last = t = tgm_stamp_monotonic(stamp_last, t);"));
	const std::string down = flat(definition(gpu(), "int tgm_rail_down(void)"));
	expectBefore(down, "sleep_gate();", "mutex_lock(&gpu.lock);");
	expectBefore(down, "__atomic_store_n(&gpu.railed, 1, __ATOMIC_SEQ_CST);",
		     "nvxnu_wakeup(&sc7_gate.shut);");
}

TEST(GM20BSleepSource, SleepAndWakeOrder)
{
	const std::string body = flat(definition(gpu(), "void tgm_sleep(void)"));
	const std::string sleep = body.substr(body.find("case TGM_SLEEP_FULL:"));
	expectBefore(sleep, "sleep_gate();", "sleep_fences();");
	expectBefore(sleep, "sleep_fences();", "mutex_lock(&gpu.lock);");
	expectBefore(sleep, "sleep_idle_off();", "sleep_nvkm();");
	expectBefore(sleep, "sleep_nvkm();", "sleep_power_down();");
	const std::string nvkm = flat(definition(gpu(), "static void sleep_nvkm(void)"));
	expectBefore(nvkm, "nvif_client_suspend(&gpu.client, false)", "nvkm_ltc_flush(");
	// nvgpu_mm_suspend: the L2's comptag lines written back (FLUSH_L2_CLEAN_COMPTAGS) before the data flush
	expectBefore(nvkm, "nvkm_wr32(device, 0x07000c, 0x00000001);", "nvkm_ltc_flush(");
	expectBefore(nvkm, "nvkm_ltc_flush(", "nvkm_device_fini(device, NVKM_SUSPEND)");
	expectBefore(flat(definition(gpu(), "static void sleep_power_down(void)")),
		     "nvxnu_irq_suspend();", "regulator_disable(");
	// the boot's order: the GPCPLL relock (clk_raise) before ACR, the PMU and GR (m2_device_init); interrupts on before
	// nvkm's first init that may wait for one
	const std::string woken = flat(definition(gpu(), "void tgm_wake(void)"));
	const std::string wake = woken.substr(woken.find("case TGM_SLEEP_FULL:"));
	expectBefore(wake, "wake_power_up();", "wake_m1();");
	expectBefore(wake, "wake_m1();", "wake_clock();");
	expectBefore(wake, "wake_clock();", "wake_device();");
	expectBefore(wake, "wake_device();", "wake_nvkm();");
	expectBefore(wake, "wake_nvkm();", "wake_fence();");
	expectBefore(wake, "wake_fence();", "wake_open();");
	expectBefore(flat(definition(gpu(), "static void wake_power_up(void)")), "power_up(",
		     "nvxnu_irq_resume();");
	EXPECT_THAT(flat(definition(gpu(), "static void wake_clock(void)")),
		    HasSubstr("clk_raise(line, sizeof(line))"));
	EXPECT_THAT(flat(definition(gpu(), "static void wake_device(void)")),
		    HasSubstr("m2_device_init(device, &at)"));
}

// NVK binds a plane with a PTE kind over a VA rounded to the bind alignment (16 KiB: the page size), so a D32_SFLOAT_S8_UINT
// image's stencil copy temp starts on it, like its planes, and its bind ends inside the image's memory (TegraGM20B refuses a
// bind past its BO: RenderBox's memoryless depth/stencil targets came back nil and the widget drew black).
TEST(NvkImageSource, StencilCopyTempStartsOnTheBindAlignment)
{
	const std::string image =
		flat(SourceText("../../../third_party/mesa/src/nouveau/vulkan/nvk_image.c"));
	const std::string temp =
		image.substr(image.find("&image->stencil_copy_temp.nil, &stencil_nil_info);"));
	EXPECT_THAT(temp.substr(0, temp.find("uint64_t plane_offset_B = 0;")),
		    HasSubstr("if (image->stencil_copy_temp.nil.pte_kind) "
			      "image->stencil_copy_temp.plane_align_B = "
			      "MAX2(image->stencil_copy_temp.plane_align_B, "
			      "pdev->nvkmd->bind_align_B);"));
	EXPECT_THAT(flat(definition(image, "nvk_image_plane_alloc_va(struct nvk_device *dev,")),
		    HasSubstr("const uint64_t va_align_B = plane->plane_align_B;"));
}

// Nouveau puts the SW engine on the runlist with id 15 and no status register: PFIFO_ENG_STATUS(15) (0x0026b8)
// PRI-times out, and the GPU's bus error panics. Every per-engine status read is behind a context or a GR check.
TEST(GM20BSchedSource, EngineStatusReadsSkipTheSoftwareEngine)
{
	for (const char *file : {"../sched.c", "../recover.c"}) {
		const std::string source = SourceText(file);
		size_t reads = 0;
		for (size_t at = source.find("PFIFO_ENG_STATUS(engn->id)"); at != std::string::npos;
		     at = source.find("PFIFO_ENG_STATUS(engn->id)", at + 1)) {
			const size_t loop = source.rfind("nvkm_runl_foreach_engn", at);
			ASSERT_NE(loop, std::string::npos) << file;
			const std::string guard = source.substr(loop, at - loop);
			EXPECT_TRUE(guard.find("engn->func->cxid") != std::string::npos ||
				    guard.find("NVKM_ENGINE_GR") != std::string::npos)
				<< file << ": " << guard;
			reads++;
		}
		EXPECT_GT(reads, 0u) << file;
	}
}

// With interrupts off XNU formats an os_log line only through the kernel printf into the message buffer, which has no
// privacy annotations: "%{public}s" prints "{public}s" and drops nvkm's report (the GR and MMU fault lines).
TEST(GM20BShimSource, InterruptPassReportsUsePlainFormats)
{
	const std::string report = flat(definition(SourceText("../shim/xnu_glue.cpp"),
						   "static void nvxnu_report(const char *line)"));
	EXPECT_THAT(report, HasSubstr("os_log_error(OS_LOG_DEFAULT, \"%s\", line)"));
	EXPECT_THAT(report, Not(HasSubstr("%{")));
}

// A channel made in a slot continues its predecessor's fence sequence, so every cursor the interrupt pass compares with
// the completed seqno starts at that fence. From 0, the idle clock would count the predecessor's whole sequence retired,
// its in-flight count (kicked - retired) would underflow, and the GPU would never drop its clock or rail-gate.
TEST(GM20BChannelSource, SeqnoCursorsStartAtTheSlotsFence)
{
	const std::string create = flat(definition(gpu(), "static int chan_new("));
	EXPECT_THAT(create, HasSubstr("chan->submitted = READ_ONCE(*fence_cpu);"));
	for (const char *cursor : {"stat_done", "woken_done", "gov_done", "progress_done"})
		EXPECT_THAT(create,
			    HasSubstr(std::string("chan->") + cursor + " = chan->submitted;"))
			<< cursor;
}

// nvkm_chan_error preempts from the interrupt pass with interrupts off under the channel's spinlock: the wrapped preempt
// decides by tgm_preempt_how and waits for a preempt in flight only in thread context.
TEST(GM20BSchedSource, ThePreemptWaitsOnlyWithInterruptsOn)
{
	const std::string preempt =
		flat(definition(SourceText("../sched.c"),
				"static void sched_chan_preempt(struct nvkm_chan *chan)"));
	EXPECT_THAT(preempt, HasSubstr("switch (tgm_preempt_how(irqs_disabled(), pending)) {"));
	expectBefore(preempt, "case TGM_PREEMPT_RECOVER: nvkm_runl_rc_cgrp(chan->cgrp); return;",
		     "case TGM_PREEMPT_AFTER_WAIT:");
	expectBefore(preempt, "case TGM_PREEMPT_AFTER_WAIT:", "nvkm_wait_msec(");
	expectBefore(preempt, "nvkm_wait_msec(", "case TGM_PREEMPT_NOW:");
	EXPECT_EQ(Count(preempt, "nvkm_wait_msec("), 1u);
}

// Linux's work items, as kits/SwitchLinux's calls (whose host test holds that a call never runs twice at once): nvkm's
// shim allocates them there, a flush runs one still queued, and irqs_disabled() answers.
TEST(GM20BShimSource, WorkItemsKeepLinuxSemantics)
{
	const std::string nvxnu = flat(SourceText("../shim/include/nvxnu.h"));
	EXPECT_THAT(nvxnu, HasSubstr("return switch_linux_call_alloc(fn, arg);"));
	EXPECT_THAT(nvxnu, HasSubstr("return switch_linux_irqs_disabled() ? 0 : 1;"));
	const std::string os = flat(SourceText("../shim/include/nvif/os.h"));
	EXPECT_THAT(
		os,
		HasSubstr("static inline bool flush_work(struct work_struct *w) { return "
			  "switch_linux_call_flush(w->thread_call, nvxnu_work_trampoline, w); }"));
	EXPECT_THAT(os, HasSubstr("#define irqs_disabled() (!nvxnu_irqs_enabled())"));
}

// Rail-down stops what reaches the GPU from callouts as system sleep does: the deadline's holds released while the GPU
// is up and its callouts stopped before the fences drain, back at the rail-up; a context-switch tick's work stopped once
// interrupts are off.
TEST(GM20BRailSource, RailDownStopsTheGpuCallouts)
{
	const std::string down = flat(definition(gpu(), "int tgm_rail_down(void)"));
	expectBefore(down, "sleep_gate();", "tgm_deadline_suspend();");
	expectBefore(down, "tgm_deadline_suspend();", "sleep_fences();");
	expectBefore(flat(definition(gpu(), "static void rail_up_locked(const void *site)")),
		     "__atomic_store_n(&gpu.railed, 0, __ATOMIC_SEQ_CST);",
		     "tgm_deadline_resume();");
	const std::string power = flat(definition(gpu(), "static void sleep_power_down(void)"));
	expectBefore(power, "nvxnu_irq_suspend();", "tgm_recover_suspend();");
	expectBefore(power, "tgm_recover_suspend();", "regulator_disable(");
}

// IOGPUDeviceSetDisplayParams is scalar method 0x21 in iOS 17's IOGPU.framework and IOConnectTrap2 index 8 in iOS 27's;
// XNU answers a trap index with no trap behind it kIOReturnSuccess, so the device connection serves index 8 and refuses
// every other by name instead of letting it pass unseen.
TEST(GM20BDeviceSource, DisplayParamsArriveByMethodAndByTrap)
{
	const std::string client = SourceText("../TegraGM20BDeviceClient.cpp");
	EXPECT_THAT(
		flat(definition(client, "IOExternalTrap "
					"*TegraGM20BDeviceUserClient::getTargetAndTrapForIndex(")),
		HasSubstr("*targetP = this; if (index == kSetDisplayParamsTrap) return "
			  "&displayParams; switch_refusal_k("));
	EXPECT_THAT(flat(client), HasSubstr("constexpr UInt32 kSetDisplayParamsTrap = 8;"));
	EXPECT_THAT(
		flat(definition(client, "IOReturn TegraGM20BDeviceUserClient::setDisplayParams(")),
		HasSubstr("storeDeadline(reinterpret_cast<uint64_t>(nextVblank), "
			  "reinterpret_cast<uint64_t>(interval));"));
	EXPECT_THAT(flat(client),
		    HasSubstr("case kSetDisplayParams: if (arguments->scalarInputCount != "
			      "2) return kIOReturnBadArgument; storeDeadline("));
}

// The tiling move and the copy engine's plane copies follow the surface's running writers through tgm_relayout_waits:
// acquires in their own push (TegraGM20BRelayout.h tgm_relayout_follow), a refusal for one they cannot acquire on, never a
// CPU wait. The kernel's own client channels' fences sit in the fence view's last slot, which every space maps and the
// connections' allocator never hands out, so a copy can acquire on the copy engine's writes.
TEST(GM20BRelayoutSource, KernelCopiesOrderOnTheGpu)
{
	const std::string relayout = SourceText("../TegraGM20BRelayout.cpp");
	const std::string waits =
		flat(definition(relayout, "extern \"C\" uint32_t tgm_relayout_waits("));
	EXPECT_THAT(waits,
		    HasSubstr("nwaits = tgm_relayout_follow(self, running, fenceVa, n, waits, "
			      "nwaits, &refused);"));
	EXPECT_THAT(
		waits,
		HasSubstr("\"gpu-order\", \"writer-unordered\", \"a kernel copy goes ahead of"));
	EXPECT_THAT(waits, Not(HasSubstr("tgm_writer_wait(")));
	EXPECT_THAT(flat(definition(relayout, "int move(")),
		    HasSubstr("tgm_relayout_waits(r.vm, tgm_chan_uid(chan), writers, nwriters, "
			      "waits, 0)"));
	EXPECT_THAT(
		flat(definition(relayout, "bool openSpace()")),
		HasSubstr("tgm_fence_kernel(TGM_FENCE_KERNEL_RELAYOUT, &r.fenceCpu, &fencePage, "
			  "&fenceOffset)"));
	const std::string copy = flat(SourceText("../TegraGM20BCopy.cpp"));
	EXPECT_THAT(copy, HasSubstr("n = tgm_relayout_waits(gCopy.vm, self, w, found, waits, n);"));
	EXPECT_THAT(copy,
		    HasSubstr("tgm_fence_kernel(TGM_FENCE_KERNEL_COPY, &c.fenceCpu, &fencePage, "
			      "&fenceOffset)"));
	const std::string fences = flat(gpu());
	EXPECT_THAT(fences, HasSubstr("#define TGM_FENCE_PAGES (TGM_FENCE_USER_PAGES + 1)"));
	EXPECT_THAT(flat(definition(
			    gpu(), "int tgm_fence_page_get(volatile uint32_t **va, uint64_t *pa)")),
		    HasSubstr("i = (fences.next + n) % TGM_FENCE_USER_PAGES;"));
	EXPECT_THAT(flat(definition(gpu(), "int tgm_fence_kernel(")),
		    HasSubstr("TGM_FENCE_USER_PAGES * TGM_FENCE_SLOT + which * sizeof(struct "
			      "TegraGM20BAbiFence);"));
	EXPECT_THAT(flat(definition(gpu(), "int tgm_writer_fence_va(")),
		    HasSubstr("off < TGM_FENCE_BYTES"));
}

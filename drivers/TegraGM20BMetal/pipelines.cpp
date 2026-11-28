// SPDX-License-Identifier: MIT
// See gpu.h (Device::pipeline, the argument tables, the runtime's own states).
#include "lock.h"
#include "blend_kill.h"
#include "creation_cost.h"
#include "dynamic_push.h"
#include "blend_reads.h"
#include "gpu.h"
#include "pack_preread.h"
#include "pack_window.h"
#include "mme.h"
#include "mip.h"
#include "owned.h"
#include "cbuf.h"
#include "multisample.h"
#include "draw_interlock.h"
#include "ycbcr_store.h"
#include "shader_test_push.h"
#include "slm.h"
#include "stage_switches.h"
#include "storage.h"
#include "xr_pass.h"
#include "written_textures.h"

#include <mach/vm_page_size.h>
#include <os/log.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "gm20b_gpu_internal.h"

namespace gm20b {

#if SWITCH_METAL_DIAGNOSTICS
// The blend-kill pipelines among `pl`'s variant combinations, those of prebuilt words alone where `prebuilt`
static uint32_t combinationBlendKills(const Pipeline &pl, bool prebuilt)
{
    uint32_t kills = 0;
    if (const VariantTable *t = pl.ownedVariantTable.get())
        for (const auto &v : t->pipelines)
            kills += v->blendKill && (!prebuilt || v->prebuiltWords);
    return kills;
}
#endif

// The methods that bind program `prog`'s shader data at its slot of constant buffer group `group` (0 vertex, 4 pixel), leaving
// the root table selected; and the slot it reads set 0 at (0: none)
static uint8_t bindProgramCbufs(Push &w, const PackProgram &prog, uint32_t group, uint64_t data, uint64_t root,
                                bool exact)
{
    uint8_t setSlot = 0;
    for (uint8_t c = 1; c < prog.cbufs; c++) {
        if (prog.cbufType[c] == CbufSet)
            setSlot = c;
        if (prog.cbufType[c] != CbufShaderData)
            continue;
        w.inc(CB_SELECTOR_A, {cbufBindBytes(prog.dataBytes, exact), uint32_t(data >> 32), uint32_t(data)});
        w.m(BIND_GROUP_CB + 0x20 * group, 1 | uint32_t(c) << 4);
        w.inc(CB_SELECTOR_A, {kRootBytes, uint32_t(root >> 32), uint32_t(root)});
    }
    return setSlot;
}

// The slot program `prog` reads set 1, a heap's arrays, at (0: none)
static uint8_t heapCbufSlot(const PackProgram &prog)
{
    for (uint8_t c = 1; c < prog.cbufs; c++)
        if (prog.cbufType[c] == kCbufHeapSet)
            return c;
    return 0;
}

// lock_ held: m2v's handle tables and roots, made at their first use (most processes name no object an argument buffer holds).
// A new binding's first submission invalidates the L2 for it (TegraGM20BTlb.h), after this fill.
bool Device::heapMemory()
{
    if (heapTables_.attached())
        return true;
    if (!k_.allocControl(HeapTables::tablesBytes(kTicCount, kTscCount), handleTables_, ControlUse::HandleTables))
        return false;
    if (!k_.allocControl(HeapTables::kRootsBytes, argumentRoots_, ControlUse::ArgumentRoots)) {
        k_.free(handleTables_);
        handleTables_ = {};
        return false;
    }
    heapTables_.attach({handleTables_.cpu, handleTables_.va, handleTables_.bytes},
                       {argumentRoots_.cpu, argumentRoots_.va, argumentRoots_.bytes}, kTicCount, kTscCount,
                       k_.info().va_start, k_.info().va_end, heapHolds_);
    set_.add(k_, {{&handleTables_, TegraGM20BSurfaceRead}, {&argumentRoots_, TegraGM20BSurfaceWrite}});
    return true;
}

// lock_ held: the CPU wrote the handle tables or a heap set, write-combined: drained here, on the writing core, before the
// next submission's job names them host-written (commit's DSB is the committing core's)
void Device::tablesWritten()
{
#if defined(__aarch64__)
    __asm__ volatile("dsb st" ::: "memory");
#endif
    tablesWritten_.fetch_add(1, std::memory_order_release);
}

// lock_ held: the set 1 of a record's arrays, shared by every record of the same arrays
const HeapSet *Device::heapSet(const HeapLayout &layout)
{
    if (!heapMemory())
        return nullptr;
    if (const HeapSet *set = heapTables_.find(layout))
        return set;
    Bo bo;
    if (!layout.bytes || !k_.allocControl(layout.bytes, bo, ControlUse::HandleTables))
        return nullptr;
    heapSets_.push_back(bo);
    set_.add(k_, bo, TegraGM20BSurfaceRead);
    const HeapSet *set = heapTables_.add(layout, {bo.cpu, bo.va, bo.bytes});
    tablesWritten();
    return set;
}

bool Device::heapTexture(uint32_t tic, const HeapTexture *texture)
{
    std::lock_guard<gm20b::Mutex> l(lock_);
    if (!texture && !heapTables_.attached())
        return true;
    if (!heapMemory())
        return false;
    heapTables_.texture(tic, texture);
    tablesWritten();
    return true;
}

bool Device::heapSampler(uint32_t tsc, const HeapSampler *sampler)
{
    std::lock_guard<gm20b::Mutex> l(lock_);
    if (!sampler && !heapTables_.attached())
        return true;
    if (!heapMemory())
        return false;
    heapTables_.sampler(tsc, sampler);
    tablesWritten();
    return true;
}

HeapTexture Device::heapTextureOf(const Texture &t, const HeapTextureFacts &facts, uint32_t flags)
{
    HeapTexture h;
    h.sampleTic = t.addressMap ? 0 : t.standIn ? t.standIn->tic : t.tic;
    h.flags = flags;
    h.facts = heapFacts(facts);
    return h;
}

bool Device::nameTexture(const Texture &t, const HeapTextureFacts &facts, uint32_t flags)
{
    const HeapTexture h = heapTextureOf(t, facts, flags);
    return heapTexture(t.tic, &h);
}

void Device::forgetTexture(const Texture &t)
{
    heapTexture(t.tic, nullptr);
}

bool Device::nameSampler(const Sampler &s, uint32_t word, Sampler &twin)
{
    HeapSampler h = {s.tsc, word};
    if (s.desc.pixel) {
        SamplerDesc normalized = s.desc;
        normalized.pixel = false;
        h.sampleTsc = twin.tsc || createSampler(normalized, twin) ? twin.tsc : 0;
    }
    return h.sampleTsc && heapSampler(s.tsc, &h);
}

void Device::forgetSampler(const Sampler &s, Sampler &twin)
{
    heapSampler(s.tsc, nullptr);
    if (twin.tsc)
        destroySampler(twin);
}

// lock_ held: what `pl`'s reads through argument buffers take of every draw: each stage's root table address (a stage whose
// root table holds buffer slots takes its own per draw: RenderEncoder::rootTable) and ExecutionStatus address in its push
// words, and its set 1. Where the device has no memory for them its draws are refused until one binds them
// (rebindArguments); the flag is published last, so a draw that reads it clear reads the words.
void Device::bindArguments(Pipeline &pl, const PackPipeline &pp)
{
    const bool roots = pp.rootTableWord[0] || pp.rootTableWord[1] || pp.statusWord[0] || pp.statusWord[1];
    if (!roots && !pp.heapArrays)
        return;
    const HeapSet *set = nullptr;
    const bool bound = heapMemory() && (!pp.heapArrays || (set = heapSet(heapLayout(pp))));
    if (bound) {
        const auto address = [&](uint8_t word, uint64_t va) {
            if (!word || word > 31)
                return;
            pl.push[word - 1] = uint32_t(va);
            pl.push[word] = uint32_t(va >> 32);
            pl.pushMask |= 3u << (word - 1);
        };
        for (uint32_t k = 0; k < 2; k++) {
            address(pp.rootTableWord[k], pp.rootSlots[k] ? 0 : heapTables_.rootTableVa());
            address(pp.statusWord[k], heapTables_.statusVa());
        }
        pl.heapSet = set;
    }
    __atomic_store_n(&pl.heapMissing, !bound, __ATOMIC_RELEASE);
}

bool Device::rebindArguments(Pipeline &pl)
{
    SWITCH_METAL_ENCODE_LOCK(l, lock_);
    if (!__atomic_load_n(&pl.heapMissing, __ATOMIC_ACQUIRE))
        return true;
    if (!pl.rec)
        return false;
    bindArguments(pl, *pl.rec);
    const bool bound = !__atomic_load_n(&pl.heapMissing, __ATOMIC_RELAXED);
    if (!bound)
        os_log_error(gm20b::log(),
                     "SwitchMetal: no memory for a state's argument tables: this draw is refused");
    return bound;
}

// A search of the pack for keyHash's records (Pack::run), counted for the creation's line (creation_cost.h)
static PackRun searched(const Pack &pack, uint64_t keyHash)
{
    SWITCH_METAL_CREATION_SPAN(lookupTicks);
    SWITCH_METAL_CREATION_COUNT(lookups, 1);
    return pack.run(keyHash);
}

static uint8_t setCbufSlot(const PackProgram &prog)
{
    for (uint8_t c = 1; c < prog.cbufs; c++)
        if (prog.cbufType[c] == CbufSet)
            return c;
    return 0;
}

static void guestBasesOf(const PackPipeline &pp, Pipeline::GuestBase (&out)[kGuestBases])
{
    static_assert(kGuestBases == kPackGuestBases, "pack.h kPackGuestBases");
    bool more = true;
    for (uint32_t k = 0; k < kPackGuestBases; k++) {
        const uint8_t slot = pp.guestBaseSlot[k];
        more = more && pp.guestBaseWord[k];
        out[k] = more ? Pipeline::GuestBase{uint8_t(slot & kGuestBaseFragment ? 1 : 0),
                                            uint8_t(slot & ~kGuestBaseFragment), pp.guestBaseWord[k]}
                      : Pipeline::GuestBase{};
    }
}

static bool needsArguments(const PackPipeline &pp)
{
    return pp.rootTableWord[0] || pp.rootTableWord[1] || pp.statusWord[0] || pp.statusWord[1] || pp.heapArrays;
}

// A program in the pack window is one of tier 0's, whose local memory the pack's ceiling covers (pack_scan.h): its
// header, in heap pages no process keeps resident, is not read. Any other's header was copied or imported with it.
uint64_t Device::programSlm(const PlacedProgram &p) const
{
    const uint8_t *heap = pack_.header() ? pack_.section<uint8_t>(PackHeap) : nullptr;
    if (heap && packWindowBytes_ && p.header >= heap && p.header < heap + packWindowBytes_)
        return pack_.header()->tier0SlmPerWarp;
    return slmPerWarpOfHeader(p.header);
}

const PackSegment *Device::prebuiltPipelineSegment(uint32_t recIndex) const
{
    if (!packWindowBytes_)
        return nullptr;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (probeExactCbufs.load(std::memory_order_relaxed))
        return nullptr;
#endif
    const PackSegment *seg = pack_.pipelineSegment(recIndex);
    return seg && packWindowContainsSegment(*seg, packWindowBytes_) ? seg : nullptr;
}

const PackSegment *Device::prebuiltVariantSegment(uint32_t recIndex, uint32_t comboIndex) const
{
    if (!packWindowBytes_)
        return nullptr;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (probeExactCbufs.load(std::memory_order_relaxed))
        return nullptr;
#endif
    const PackSegment *seg = pack_.variantSegment(recIndex, comboIndex);
    return seg && packWindowContainsSegment(*seg, packWindowBytes_) ? seg : nullptr;
}

Pipeline *Device::pipeline(uint64_t keyHash, PackForm form, bool caTransform)
{
    return pipeline(searched(pack_, keyHash), keyHash, form, caTransform);
}

// A state of the pack, made the first time it is looked up (most of the pack's states never are in a process): its registers
// as push words with its programs placed in the heap, the record drawn as the pack holds it (the generator's tests scan
// every record of the pack: pack_scan.h). Else one the runtime made (addRecord). A storage form that computes
// the pipeline's blend has no barrier twin to fall back on: its barrier form leaves the blend to the ROP after a fetch
// outside the interlock, which keeps Metal's result only drawn a triangle at a time. A pass it cannot view as its storage
// images refuses its draws (RenderEncoder::prepare), as Metal fails a state drawn into attachments it was not made for.
// That form is its blendTwin, which a draw takes a triangle at a time where rule fetch.twin holds (twin_rule.h).
Pipeline *Device::pipeline(const PackRun &run, uint64_t keyHash, PackForm form, bool caTransform)
{
    Pipeline *pl = placePipeline(run, keyHash, form, caTransform);
    if (form == PackForm::Default && pl && pl->fetchBits && fetchFreeReachable(run)) {
        if (!__atomic_load_n(&pl->fetchFree, __ATOMIC_ACQUIRE)) {
            Pipeline *free = placePipeline(run, keyHash, PackForm::FetchFree, caTransform);
            if (free && free != pl && free->freeForm.rung != FetchNone)
                __atomic_store_n(&pl->fetchFree, free, __ATOMIC_RELEASE);
        }
    }
    if (form != PackForm::Default || !pl || !pl->storageBits)
        return pl;
    Pipeline **slot = pl->storageBlendBits ? &pl->blendTwin : &pl->barrierTwin;
    if (!__atomic_load_n(slot, __ATOMIC_ACQUIRE)) {
        Pipeline *twin = placePipeline(run, keyHash, PackForm::Barrier, caTransform);
        if (twin && twin != pl && !twin->storageBits)
            __atomic_store_n(slot, twin, __ATOMIC_RELEASE);
    }
    return pl;
}

// Whether the state's fetch-free form, where the pack holds one, is placed (fetch_free.h fetchFreePlaced)
bool Device::fetchFreeReachable(const PackRun &run) const
{
    const PackPipeline *rec = pack_.find(run, PackForm::FetchFree);
    if (!rec)
        return false;
    uint8_t probe = 0;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    probe = probeFetchRung.load(std::memory_order_relaxed);
#endif
    return fetchFreePlaced(fetchFreeForm(*rec, pack_.iterated(*rec), pack_.far(*rec)), probe);
}

std::vector<std::pair<std::string, Pipeline *>> Device::variantPipelines(uint64_t keyHash, bool caTransform)
{
    return variantPipelines(searched(pack_, keyHash), caTransform);
}

std::vector<std::pair<std::string, Pipeline *>> Device::variantPipelines(const PackRun &run, bool caTransform)
{
    ensurePackWindow();
    std::vector<std::pair<std::string, Pipeline *>> out;
    bool anyVariant = false;
    for (uint32_t i = 0; i < run.records; ++i)
        if (run.first[i].flags & PipelineVariantRecord) {
            anyVariant = true;
            break;
        }
    if (!anyVariant)
        return out;
    for (const PackPipeline *rec : pack_.variantRecords(run)) {
        const std::string name = pack_.string(rec->variantName);
        // a barrier record is a storage form's twin unless it is the variant's one record (its storage form refused)
        if ((rec->flags & PipelineBarrierForm) && pack_.variantRecord(run, name, PackForm::Default))
            continue;
        Pipeline *pl = placePackedRecord(*rec, caTransform);
        // a storage form's barrier twin, as Device::pipeline gives a state's own
        const PackPipeline *barrier =
            pl && pl->storageBits ? pack_.variantRecord(run, name, PackForm::Barrier) : nullptr;
        if (barrier) {
            Pipeline **slot = pl->storageBlendBits ? &pl->blendTwin : &pl->barrierTwin;
            if (!__atomic_load_n(slot, __ATOMIC_ACQUIRE)) {
                Pipeline *twin = placePackedRecord(*barrier, caTransform);
                if (twin && twin != pl && !twin->storageBits)
                    __atomic_store_n(slot, twin, __ATOMIC_RELEASE);
            }
        }
        out.emplace_back(name, pl);
    }
    return out;
}

Pipeline *Device::variantPipeline(const PackRun &run, const std::string &name, bool caTransform)
{
    ensurePackWindow();
    const PackPipeline *rec = pack_.variantRecord(run, name, PackForm::Default);
    if (!rec)
        rec = pack_.variantRecord(run, name, PackForm::Barrier);
    if (!rec)
        return nullptr;
    Pipeline *pl = placePackedRecord(*rec, caTransform);
    const PackPipeline *barrier = pl && pl->storageBits ? pack_.variantRecord(run, name, PackForm::Barrier) : nullptr;
    if (barrier) {
        Pipeline **slot = pl->storageBlendBits ? &pl->blendTwin : &pl->barrierTwin;
        if (!__atomic_load_n(slot, __ATOMIC_ACQUIRE)) {
            Pipeline *twin = placePackedRecord(*barrier, caTransform);
            if (twin && twin != pl && !twin->storageBits)
                __atomic_store_n(slot, twin, __ATOMIC_RELEASE);
        }
    }
    return pl;
}

Pipeline *Device::placePipeline(const PackRun &run, uint64_t keyHash, PackForm form, bool caTransform)
{
    ensurePackWindow();
    const PackPipeline *rec = pack_.find(run, form);
    if (rec)
        return placePackedRecord(*rec, caTransform);
    std::lock_guard<gm20b::Mutex> l(lock_);
    const auto made = completed_.find(keyHash);
    return form == PackForm::Default && made != completed_.end() ? &made->second->pipeline : nullptr;
}

// A pack record is drawn as addRecord draws a runtime record of the same state: a tessellation state's control and
// evaluation programs bound beside its own, the registers its draws set back after it, and its stride twin
std::vector<uint32_t> restoreWords(const uint32_t *pairs, uint32_t dwords)
{
    std::vector<uint32_t> words(dwords + 2);
    Push w{words.data(), words.data() + words.size()};
    for (uint32_t k = 0; k + 1 < dwords; k += 2)
        w.m(pairs[k], pairs[k + 1]);
    return {words.data(), w.at};
}

std::unique_ptr<Pipeline> Device::tryBuildTier0LockFree(const PackPipeline &pp, uint32_t idx, bool caTransform)
{
    const PackSegment *prebuilt = prebuiltPipelineSegment(idx);
    if (!prebuilt || needsArguments(pp))
        return nullptr;
    const bool tess = tessellates(pp);
    PlacedProgram vs, fs = noPixelProgram(), tessPlaced[2];
    if (!placePackWindowProgram(pp.vs, vs) || (pixelStage(pp) && !placePackWindowProgram(pp.fs, fs)) ||
        (tess && (!placePackWindowProgram(pp.tessPrograms[0], tessPlaced[0]) ||
                  !placePackWindowProgram(pp.tessPrograms[1], tessPlaced[1]))))
        return nullptr;
    uint64_t slm = std::max(programSlm(vs), programSlm(fs));
    for (uint32_t k = 0; tess && k < 2; k++)
        slm = std::max(slm, programSlm(tessPlaced[k]));
    if (!slmReady(slm))
        return nullptr;
    Pipeline *twin = nullptr;
    if (pp.strideTwin) {
        if (pp.strideTwin - 1 >= tier0Pipelines_.count)
            return nullptr;
        twin = placePackedRecord(pack_.section<PackPipeline>(PackPipelines)[pp.strideTwin - 1], caTransform);
        if (!twin)
            return nullptr;
    }
    auto built = std::make_unique<Pipeline>();
    Pipeline &pl = *built;
    build(pl, pp, vs, fs, pack_.section<uint32_t>(PackBlob) + pp.blob, pack_.string(pp.keyString), caTransform,
          pack_.spills(pp), pack_.far(pp), tess ? tessPlaced : nullptr, prebuilt);
    if (pp.restoreDwords && uint64_t(pp.restoreAt) + pp.restoreDwords <= pack_.bytes(PackBlob) / 4) {
        const std::vector<uint32_t> words =
            restoreWords(pack_.section<uint32_t>(PackBlob) + pp.restoreAt, pp.restoreDwords);
        pl.restore.insert(pl.restore.end(), words.begin(), words.end());
    }
    pl.strideTwin = twin;
    if (!offerVariantsLockFree(pl, pp, idx))
        return nullptr;
    if (const PackSplit *split = pixelStage(pp) ? pack_.split(pp.fs) : nullptr; split && !pl.storageBits) {
        SplitSource source;
        source.rec = *split;
        source.packed = true;
        source.pp = &pp;
        source.blob = pack_.section<uint32_t>(PackBlob) + pp.blob;
        source.far = pack_.far(pp);
        source.keyString = pack_.string(pp.keyString);
        source.vs = vs;
        source.caTransform = caTransform;
        offerSplit(pl, std::move(source));
    }
    return built;
}

Pipeline *Device::publishTier0(uint32_t idx, std::unique_ptr<Pipeline> built)
{
    std::atomic<Pipeline *> &slot = tier0Pipelines_.slots[idx];
    Pipeline *expected = nullptr;
    Pipeline *raw = built.get();
    if (!slot.compare_exchange_strong(expected, raw, std::memory_order_acq_rel, std::memory_order_acquire))
        return expected;
    built.release();
    SWITCH_METAL_DIAGNOSTIC(stats.blendKillStates += raw->blendKill);
    SWITCH_METAL_DIAGNOSTIC(stats.blendKillStates += combinationBlendKills(*raw, false));
    return raw;
}

Pipeline *Device::variantPipelineLockFree(const PackRun &run, const std::string &name, bool caTransform)
{
    ensurePackWindow();
    const PackPipeline *pipelines = pack_.section<PackPipeline>(PackPipelines);
    if (!run.records || uint64_t(run.first - pipelines) >= tier0Pipelines_.count)
        return nullptr;
    const PackPipeline *rec = pack_.variantRecord(run, name, PackForm::Default);
    const uint32_t idx = rec ? uint32_t(rec - pipelines) : UINT32_MAX;
    if (idx >= tier0Pipelines_.count)
        return nullptr;
    Pipeline *pl = tier0Pipelines_.slots[idx].load(std::memory_order_acquire);
    if (!pl) {
        std::unique_ptr<Pipeline> built = tryBuildTier0LockFree(*rec, idx, caTransform);
        if (!built)
            return nullptr;
        pl = publishTier0(idx, std::move(built));
    }
    return pl->storageBits ? nullptr : pl;
}

Pipeline *Device::placePackedRecord(const PackPipeline &pp, bool caTransform)
{
    ensurePackWindow();
    const uint32_t idx = uint32_t(&pp - pack_.section<PackPipeline>(PackPipelines));
    if (idx < tier0Pipelines_.count) {
        if (Pipeline *existing = tier0Pipelines_.slots[idx].load(std::memory_order_acquire))
            return existing;
        if (std::unique_ptr<Pipeline> built = tryBuildTier0LockFree(pp, idx, caTransform))
            return publishTier0(idx, std::move(built));
    } else {
        prereadPackRecord(pack_, pp, uint64_t(vm_page_size));
    }
    std::lock_guard<gm20b::Mutex> l(lock_);
    return placeRecord(pp, caTransform);
}

Pipeline *Device::placeRecord(const PackPipeline &pp, bool caTransform)
{
    const uint32_t idx = uint32_t(&pp - pack_.section<PackPipeline>(PackPipelines));
    const bool tier0 = idx < tier0Pipelines_.count;
    if (tier0) {
        if (Pipeline *existing = tier0Pipelines_.slots[idx].load(std::memory_order_acquire))
            return existing;
    } else if (const auto it = pipelines_.find(idx); it != pipelines_.end() && it->second && it->second->rec) {
        return it->second.get();
    }
    const bool tess = tessellates(pp);
    PlacedProgram vs, fs = noPixelProgram(), tessPlaced[2];
    if (!placePackProgram(pp.vs, vs) || (pixelStage(pp) && !placePackProgram(pp.fs, fs)) ||
        (tess && (!placePackProgram(pp.tessPrograms[0], tessPlaced[0]) ||
                  !placePackProgram(pp.tessPrograms[1], tessPlaced[1]))))
        return nullptr;
    uint64_t slm = std::max(programSlm(vs), programSlm(fs));
    for (uint32_t k = 0; tess && k < 2; k++)
        slm = std::max(slm, programSlm(tessPlaced[k]));
    if (!placeRequiredVariants(pp, slm))
        return nullptr;
    if (!slmNeed(slm)) {
        stats.heapFull++;
        return nullptr;
    }
    Pipeline *twin = nullptr;
    if (pp.strideTwin &&
        !(twin = placeRecord(pack_.section<PackPipeline>(PackPipelines)[pp.strideTwin - 1], caTransform)))
        return nullptr;
    if (tier0) {
        if (Pipeline *existing = tier0Pipelines_.slots[idx].load(std::memory_order_acquire))
            return existing;
    } else if (const auto it = pipelines_.find(idx); it != pipelines_.end() && it->second && it->second->rec) {
        return it->second.get();
    }
    auto owned = std::make_unique<Pipeline>();
    Pipeline &pl = *owned;
    const PackSegment *prebuilt = prebuiltPipelineSegment(idx);
    build(pl, pp, vs, fs, pack_.section<uint32_t>(PackBlob) + pp.blob, pack_.string(pp.keyString), caTransform,
          pack_.spills(pp), pack_.far(pp), tess ? tessPlaced : nullptr, prebuilt);
    if (pp.restoreDwords && uint64_t(pp.restoreAt) + pp.restoreDwords <= pack_.bytes(PackBlob) / 4) {
        const std::vector<uint32_t> words =
            restoreWords(pack_.section<uint32_t>(PackBlob) + pp.restoreAt, pp.restoreDwords);
        pl.restore.insert(pl.restore.end(), words.begin(), words.end());
    }
    pl.strideTwin = twin;
    offerVariants(pl, pp, idx);
    if (const PackSplit *split = pixelStage(pp) ? pack_.split(pp.fs) : nullptr; split && !pl.storageBits) {
        SplitSource source;
        source.rec = *split;
        source.packed = true;
        source.pp = &pp;
        source.blob = pack_.section<uint32_t>(PackBlob) + pp.blob;
        source.far = pack_.far(pp);
        source.keyString = pack_.string(pp.keyString);
        source.vs = vs;
        source.caTransform = caTransform;
        offerSplit(pl, std::move(source));
    }
    if (tier0) {
        Pipeline *expected = nullptr;
        Pipeline *raw = owned.get();
        if (tier0Pipelines_.slots[idx].compare_exchange_strong(expected, raw, std::memory_order_acq_rel,
                                                               std::memory_order_acquire)) {
            owned.release();
            SWITCH_METAL_DIAGNOSTIC(if (prebuilt) stats.blendKillStates += raw->blendKill);
            SWITCH_METAL_DIAGNOSTIC(stats.blendKillStates += combinationBlendKills(*raw, true));
            return raw;
        }
        return expected;
    }
    const auto [it, inserted] = pipelines_.emplace(idx, std::move(owned));
    (void)inserted;
    return it->second.get();
}

// `pl` of record `pp`, whose programs are placed at `vs` and `fs`, whose registers are `blob` (method, value pairs)
// and whose key's attachment and vertex fields are `keyString`
void Device::build(Pipeline &pl, const PackPipeline &pp, const PlacedProgram &vs, const PlacedProgram &fs,
                   const uint32_t *blob, const char *keyString, bool caTransform, const uint32_t *spills,
                   const uint32_t *far, const PlacedProgram *tess, const PackSegment *prebuilt)
{
    SWITCH_METAL_CREATION_SPAN(buildTicks);
    SWITCH_METAL_CREATION_COUNT(builds, 1);
    pl.entries = SetEntries(pp, far);
    auto instructions = [](const PackProgram &program) {
        const uint32_t bytes = program.dataBytes ? program.dataOffset : program.bytes;
        return std::max(1u, (bytes > 128 ? bytes - 128 : 0) / 8);
    };
    pl.vertexInstructions =
        instructions(*vs.record) + (tess ? instructions(*tess[0].record) + instructions(*tess[1].record) : 0);
    pl.fragmentInstructions = instructions(*fs.record);
    const auto has = [](const PlacedProgram &p, uint32_t fact) {
        return (p.record->facts & fact) != 0;
    };
    pl.writesMemory[0] = has(vs, kProgramWritesMemory) ||
                         (tess && (has(tess[0], kProgramWritesMemory) || has(tess[1], kProgramWritesMemory)));
    pl.writesMemory[1] = has(fs, kProgramWritesMemory);
    pl.memoryEntries = pp.memoryEntries;
    std::vector<uint32_t> words;
    Push w{nullptr, nullptr};
    if (!prebuilt) {
        words.resize(64 + 16 + kStageSwitchWords + (tess ? 64 : 0) + pp.blobDwords * 2);
        w = {words.data(), words.data() + words.size()};
    }
    uint32_t writes[8];
    uint32_t interlockControl = 0;
    uint32_t sampleControl = 1;
    bool tests = false, alphaToCoverage = false;
    memcpy(writes, ctWrite_, sizeof writes);
    for (uint32_t k = 0; k < pp.blobDwords; k += 2) {
        const uint32_t m = blob[k], v = blob[k + 1];
        tests |= (m == DEPTH_TEST || m == STENCIL_TEST || m == ANTI_ALIAS_ALPHA_CONTROL) && (v & 1);
        alphaToCoverage |= m == ANTI_ALIAS_ALPHA_CONTROL && (v & 1);
        pl.namesAlphaControl |= m == ANTI_ALIAS_ALPHA_CONTROL;
        if (m == HYBRID_ANTI_ALIAS_CONTROL)
            pl.samplePasses = uint8_t(std::max(1u, v & 0xf));
        if (ownedMethod(m) || swizzleMethod(m))
            continue;  // a pack made before the method was the runtime's: the probe process's value
        if (m >= CT_WRITE && m < CT_WRITE + 32)
            writes[(m - CT_WRITE) / 4] = v;
        if (m >= VERTEX_STREAM_FORMAT && m < VERTEX_STREAM_FORMAT + kStreamMethods * kPackVertexStreams &&
            (m - VERTEX_STREAM_FORMAT) % kStreamMethods == 0)
            pl.streamStride[(m - VERTEX_STREAM_FORMAT) / kStreamMethods] =
                uint16_t(NVVAL_GET(v, NVB197, SET_VERTEX_STREAM_A_FORMAT, STRIDE));
        if (m == PIXEL_SHADER_INTERLOCK_CONTROL) {
            interlockControl = v;
            continue;
        }
        if (m == HYBRID_ANTI_ALIAS_CONTROL) {
            sampleControl = v;
            continue;
        }
        if (m == VIEWPORT_CLIP_CONTROL) {
            pl.viewportClip = v;
            pl.namesViewportClip = true;
        }
        pl.namesViewportClipRange |= m == NVB197_SET_VIEWPORT_CLIP_MIN_Z(0) || m == NVB197_SET_VIEWPORT_CLIP_MAX_Z(0);
        if (!prebuilt)
            w.m(m, v);
    }
    memcpy(pl.ctWrite, writes, sizeof writes);
    const ParsedPipelineKey parsedKey = parsePipelineKey(keyString);
    // what the state writes: a target its key does not name is no attachment of its passes, whatever SET_CT_WRITE the
    // pack's baseline leaves it (24B119: targets 6 and 7 at 0x1111; blend_kill.h)
    const uint8_t keyTargets = parsedKey.colorTargets;
    uint32_t named[8];
    for (uint32_t c = 0; c < 8; c++)
        named[c] = keyTargets >> c & 1 ? writes[c] : 0;
    if (prebuilt) {
        // the GPU gets the word the pack's generator wrote into the segment, whatever this build's rule says: a card's pack
        // may predate it. Its flag (pack/segments.py FLAG_BLEND_KILL) is in the segment table tier 0 wires; the segment's
        // words are heap pages no process keeps resident, an SD read on the creating thread.
        const bool pushed = prebuilt->flags & kPackSegmentBlendKill;
#if SWITCH_METAL_DIAGNOSTICS
        const bool computed =
            iteratedBlendNoop(blob, pp.blobDwords, named, keyString,
                              (pp.flags & PipelineStorageMetadata) ? pp.storageMask : 0, blendPerTarget_,
                              blendPerTargetSeen_, &parsedKey) == kNoopSourceRgba0000;
        stats.blendKillPackDisagreements += pushed != computed;
#endif
        pl.blendKill = pushed;
    } else {
        pl.blendKill = iteratedBlendNoop(blob, pp.blobDwords, named, keyString,
                                         (pp.flags & PipelineStorageMetadata) ? pp.storageMask : 0, blendPerTarget_,
                                         blendPerTargetSeen_, &parsedKey) == kNoopSourceRgba0000;
    }
    if (!prebuilt) {
        w.m(PIXEL_SHADER_INTERLOCK_CONTROL, legalInterlockControl(interlockControl, sampleControl));
        w.m(HYBRID_ANTI_ALIAS_CONTROL, sampleControl);
        w.m(kIteratedBlendOptimization, pl.blendKill ? kNoopSourceRgba0000 : kNoopNever);
        SWITCH_METAL_DIAGNOSTIC(stats.blendKillStates += pl.blendKill);
    }
    for (uint32_t c = 0; c < 8; c++)
        if (named[c] & 0x1111)  // R_ENABLE, G_ENABLE, B_ENABLE, A_ENABLE
            pl.writeBits |= uint8_t(1u << c);
    bool exactCbufs = false;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    exactCbufs = probeExactCbufs.load(std::memory_order_relaxed);
#endif
    if (!prebuilt) {
        w.m(0x2044, vs.program);  // SET_PIPELINE_PROGRAM(VERTEX)
        w.m(PIPELINE_REGISTER_COUNT_VERTEX, vs.record->gprs);
        if (pixelStage(pp)) {
            w.m(0x2144, fs.program);  // SET_PIPELINE_PROGRAM(PIXEL)
            w.m(PIPELINE_REGISTER_COUNT_PIXEL, fs.record->gprs);
        }
        if (fs.record->boundTextures)
            w.m(BINDLESS_TEXTURE, fs.record->boundTextures - 1u);
        pl.setSlot[0] = bindProgramCbufs(w, *vs.record, kSetGroups[0], vs.data, root_.va, exactCbufs);
        pl.setSlot[1] = bindProgramCbufs(w, *fs.record, kSetGroups[1], fs.data, root_.va, exactCbufs);
    } else {
        pl.setSlot[0] = setCbufSlot(*vs.record);
        pl.setSlot[1] = setCbufSlot(*fs.record);
    }
    const PackProgram *programs[4] = {vs.record, fs.record};
    const uint16_t *farUbos[4] = {vs.farUbos, fs.farUbos};
    if (tess && !prebuilt) {
        // SET_PATCH is in kOwnedMethods so the blob loop drops it; m2v's tessellation states always draw 1-point
        // patches.
        w.m(NVB197_SET_PATCH, 1);
    }
    for (uint32_t k = 0; tess && k < 2; k++) {
        if (!prebuilt) {
            w.m(PIPELINE_PROGRAM_TESS + kPipelineStageStride * k, tess[k].program);
            w.m(PIPELINE_REGISTER_COUNT_TESS + kPipelineStageStride * k, tess[k].record->gprs);
            pl.setSlot[2 + k] =
                bindProgramCbufs(w, *tess[k].record, kSetGroups[2 + k], tess[k].data, root_.va, exactCbufs);
        } else {
            pl.setSlot[2 + k] = setCbufSlot(*tess[k].record);
        }
        programs[2 + k] = tess[k].record;
        pl.heapSlot[2 + k] = heapCbufSlot(*tess[k].record);
        farUbos[2 + k] = tess[k].farUbos;
    }
    for (uint32_t k = 0; k < 4; k++)
        for (uint8_t c = 1; programs[k] && c < programs[k]->cbufs; c++) {
            const PackProgram *prog = programs[k];
            const uint8_t group = kSetGroups[k];
            if (cbufKind(prog->cbufType[c]) != CbufUbo)
                continue;
            const uint32_t offset = cbufUboOffset(prog->cbufType, farUbos[k], c);
            const SetEntries &entries = pl.entries;
            uint32_t entry = 0;
            while (entry < entries.size() && !(uniformEntry(entries[entry]) && entries[entry].offset == offset))
                entry++;
            pl.ubosNamed &= entry < entries.size();
            if (entry < entries.size())
                pl.ubos[pl.uboCount++] = {group, c, uint16_t(entry)};
        }
    if (!prebuilt) {
        stageSwitches(pp, parsedKey.rasterOff, w, pl.restore);
        pl.methods.assign(words.data(), w.at);
    } else {
        stageSwitchesRestore(pp, parsedKey.rasterOff, pl.restore);
    }
    pl.stagesOff = !pixelStage(pp) || parsedKey.rasterOff;
    memcpy(pl.push, pp.pushWords, sizeof pl.push);
    pl.pushMask = pp.pushMask;
    guestBasesOf(pp, pl.guestBases);
    pl.heapSlot[0] = heapCbufSlot(*vs.record);
    pl.heapSlot[1] = heapCbufSlot(*fs.record);
    if (needsArguments(pp))
        bindArguments(pl, pp);
    for (uint32_t k = 0; k < 2; k++)
        if (spillEntryOf(pp, k))
            pl.spill[k] = spills ? spills + (k ? pp.spillWords[0] : 0) : nullptr;
    // the color formats of its key, and which colors its draws read as they were (blend_reads.h)
    for (unsigned c = 0; c < 8; c++) {
        const ParsedColorKey &pk = parsedKey.colors[c];
        if (pk.fieldCount < 3)
            continue;
        const ColorKeyFields k = {pk.format, pk.writeMask, pk.blending, pk.srcRGB, pk.dstRGB,
                                  pk.rgbOp,  pk.srcAlpha,  pk.dstAlpha, pk.alphaOp};
        // rendered through its stand-in (Texture::planarStandIn; an extended-range one's, format_standin.h)
        constexpr unsigned kRGB8_444_2P = 542, kRGBA16Float = 115;
        pl.colorFormats[c] = k.format == kRGB8_444_2P || xrTarget(k.format) ? kRGBA16Float : k.format;
        if (colorReadsDestination(k))
            pl.readBits |= uint8_t(1u << c);
        // MTLBlendFactorBlendColor, OneMinusBlendColor, BlendAlpha, OneMinusBlendAlpha
        for (const unsigned factor : {k.srcRGB, k.dstRGB, k.srcAlpha, k.dstAlpha})
            pl.constantBlend |= k.blending && factor >= 11 && factor <= 14;
    }
    // attribute 0 ("a0=<MTLVertexFormat>:<offset>:<buffer>:<stride>:<step function>:..."): MTLVertexFormatFloat2, Float3
    // or Float4, per vertex, from the stream's buffer
    if (parsedKey.hasA0 && parsedKey.a0Format >= 29 && parsedKey.a0Format <= 31 && parsedKey.a0Buffer == pp.vertexSlot &&
        parsedKey.a0Stride == pp.vertexStride && parsedKey.a0Step == 1) {
        pl.positionComponents = uint8_t(parsedKey.a0Format - 27);
        pl.positionOffset = uint16_t(parsedKey.a0Offset);
    }
    pl.caTransform = caTransform;
    if (parsedKey.hasDs) {
        const uint32_t depth = parsedKey.depth, stencil = parsedKey.stencil;
        // a Depth32Float or Depth16Unorm and a Stencil8 apart are rendered as one Depth32Float_Stencil8 (PassDesc::separate)
        pl.depthFormat = (depth == 252 || depth == 250) && stencil == 253 ? kZetaFormat : depth ? depth : stencil;
    }
    pl.samples = uint8_t(parsedKey.samples);
    // SET_DEPTH_TEST, SET_STENCIL_TEST and SET_ANTI_ALIAS_ALPHA_CONTROL's alpha to coverage in bit 0
    pl.primitiveId = has(fs, kProgramPrimitiveId);
    pl.vertexStores = has(vs, kProgramGlobalStore) ||
                      (tess && (has(tess[0], kProgramGlobalStore) || has(tess[1], kProgramGlobalStore)));
    pl.fragmentDepth = has(fs, kProgramWritesDepth);
    pl.viewportIndex = has(tess ? tess[1] : vs, kProgramWritesViewportIndex);
    pl.writesLayer = has(tess ? tess[1] : vs, kProgramWritesLayer);
    pl.dropsPixels = has(fs, kProgramKillsPixels) || has(fs, kProgramWritesSampleMask) || tests ||
                     !parsedKey.samples1Prefix || !parsedKey.hasZeroDsToken;
    // KILLS_PIXELS, OMAP_SAMPLE_MASK or OMAP_DEPTH, or its registers' alpha to coverage; a Metal state's own alpha to
    // coverage is the encoder's, per draw (alpha_control.h)
    pl.shapesCoverage = has(fs, kProgramKillsPixels) || has(fs, kProgramWritesSampleMask) ||
                        has(fs, kProgramWritesDepth) || alphaToCoverage;
    for (uint32_t k = 0; k < pl.entries.size(); k++)
        if (pl.entries[k].kind == EntryFetch && pl.entries[k].slot < 8)
            pl.fetchBits |= uint8_t(1u << pl.entries[k].slot);
    if (pp.flags & PipelineStorageMetadata) {
        pl.storageBits = pp.storageMask;
        pl.storageWriteBits = pp.storageWriteMask;
        pl.storageBlendBits = pp.storageBlendMask & pp.storageWriteMask;
        memcpy(pl.storageTic, pp.storageTic, sizeof pl.storageTic);
        pl.readBits &= ~pl.storageBits;
    }
    pl.freeForm = (pp.flags & PipelineFetchFreeForm) ? fetchFreeForm(pp, pack_.iterated(pp), far) : FetchFreeForm();
    if (pl.freeForm.rung != FetchNone) {
        pl.readBits |= uint8_t(1u << pl.freeForm.colour);
        if (pl.freeForm.rung == FetchIteratedBlend) {
            pl.restore.clear();
            if (prebuilt)
                iteratedBlendRestore(pl.colorFormats[pl.freeForm.colour], pl.restore);
            else
                iteratedBlendWords(*pack_.iterated(pp), pl.freeForm.colour, pl.colorFormats[pl.freeForm.colour],
                                   pl.methods, pl.restore);
        }
    }
    pl.turnRefused = pp.flags & PipelineTurnRefused;
    pl.xrSlots = pp.xrSlots;
    pl.writtenSlots = writtenSlots(pp);
    pl.own = isOwnState(pp.keyHash);
    pl.rec = &pp;
    if (prebuilt) {
        pl.prebuiltWords = reinterpret_cast<const uint32_t *>(pack_.section<uint8_t>(PackHeap) + prebuilt->heapOffset);
        pl.prebuiltWordCount = prebuilt->words;
        pl.reselectRoot = (prebuilt->flags & kPackSegmentReselectRoot) != 0;
        pl.segment = {packWindowVa_ + prebuilt->heapOffset, prebuilt->words,
                      (prebuilt->flags & kPackSegmentKeptAcrossPasses) != 0};
    } else {
        pl.segment = segmentHeap_.place(pl.methods.data(), uint32_t(pl.methods.size()));
    }
}

Pipeline *Device::ycbcrPipeline(uint8_t output)
{
    static const char *const names[] = {"",
                                        "raw.ycbcr.ycbcr_converted_frag",
                                        "raw.ycbcr.ycbcr_srgb_frag",
                                        "raw.ycbcr.ycbcr_codes_frag",
                                        "raw.ycbcr.ycbcr_codes10_frag",
                                        "raw.ycbcr.ycbcr_packed_converted_frag",
                                        "raw.ycbcr.ycbcr_packed_srgb_frag",
                                        "raw.ycbcr.ycbcr_packed_codes10_frag",
                                        "raw.ycbcr.ycbcr_packed_plane_frag",
                                        "raw.ycbcr.ycbcr_codes_frag",
                                        "raw.ycbcr.ycbcr_codes10_frag",
                                        "raw.ycbcr.ycbcr_packed_codes10_frag"};
    if (!output || output >= sizeof names / sizeof *names)
        return nullptr;
    return ownPipeline(names[output]);
}

Pipeline *Device::ycbcrStorePipeline(bool chroma, const YcbcrStoreShape &store)
{
    static const char *const names[2][2][2] = {
        {{"raw.ycbcr_store.ycbcr_store_luma8_frag", "raw.ycbcr_store.ycbcr_store_luma16_frag"},
         {"raw.ycbcr_store.ycbcr_store_chroma8_frag", "raw.ycbcr_store.ycbcr_store_chroma16_frag"}},
        {{"raw.ycbcr_store.ycbcr_store_luma8_srgb_frag", "raw.ycbcr_store.ycbcr_store_luma16_srgb_frag"},
         {"raw.ycbcr_store.ycbcr_store_chroma8_srgb_frag", "raw.ycbcr_store.ycbcr_store_chroma16_srgb_frag"}}};
    static const char *const packed[2][2] = {
        {"raw.ycbcr_store.ycbcr_store_luma_packed_frag", "raw.ycbcr_store.ycbcr_store_chroma_packed_frag"},
        {"raw.ycbcr_store.ycbcr_store_luma_packed_srgb_frag", "raw.ycbcr_store.ycbcr_store_chroma_packed_srgb_frag"}};
    const char *name = store.plane    ? "raw.ycbcr_store.ycbcr_store_plane_packed_frag"
                       : store.words  ? (store.srgb ? "raw.ycbcr_store.ycbcr_store_words_srgb_frag"
                                                    : "raw.ycbcr_store.ycbcr_store_words_frag")
                       : store.packed ? packed[store.srgb != 0][chroma]
                                      : names[store.srgb != 0][chroma][store.max > 255];
    return ownPipeline(name);
}

Pipeline *Device::ycbcrLoadPipeline(uint32_t channels)
{
    return ownPipeline(channels == 1   ? "raw.ycbcr.ycbcr_load_plane_packed_r_frag"
                       : channels == 2 ? "raw.ycbcr.ycbcr_load_plane_packed_rg_frag"
                                       : nullptr);
}

Pipeline *Device::xrPlanesImagePipeline(bool split)
{
    return ownPipeline(split ? "raw.xr.xr_planes_split_frag" : "raw.xr.xr_planes_merge_frag");
}

Pipeline *Device::xrPipeline(XrForm form, bool twoPlanes)
{
    return ownPipeline(xrConversion(form, twoPlanes));
}

Pipeline *Device::resolvePipeline(uint32_t format, uint32_t samples)
{
    const char *name = resolveState(format, samples);
    Pipeline *pl = ownPipeline(name);
    SWITCH_METAL_DIAGNOSTIC(if (pl) __atomic_store_n(&pl->spanName, name, __ATOMIC_RELEASE));
    return pl;
}

Pipeline *Device::depthResolvePipeline(uint32_t format, uint32_t filter)
{
    return ownPipeline(depthResolveState(format, filter));
}

bool Device::depthConvertible(uint32_t samples)
{
    return ownPipeline(turnDepthState(250, samples)) && ownPipeline(turnDepthState(260, samples));
}

Pipeline *Device::mipPipeline(uint32_t format)
{
    return ownPipeline(mipState(format));
}

Pipeline *Device::mip3dPipeline(uint32_t format)
{
    return ownPipeline(mip3dState(format));
}

}  // namespace gm20b

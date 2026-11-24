// SPDX-License-Identifier: MIT
// See gpu.h (Device::pipeline, the argument tables, the runtime's own states).
#include "lock.h"
#include "blend_kill.h"
#include "creation_cost.h"
#include "dynamic_push.h"
#include "blend_reads.h"
#include "gpu.h"
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

#include <os/log.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "gm20b_gpu_internal.h"

namespace gm20b {

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
        os_log_error(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "gm20b"),
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
        Pipeline *free = placePipeline(run, keyHash, PackForm::FetchFree, caTransform);
        std::lock_guard<gm20b::Mutex> l(lock_);
        if (free && free != pl && free->freeForm.rung != FetchNone)
            pl->fetchFree = free;
    }
    if (form != PackForm::Default || !pl || !pl->storageBits)
        return pl;
    Pipeline *twin = placePipeline(run, keyHash, PackForm::Barrier, caTransform);
    std::lock_guard<gm20b::Mutex> l(lock_);
    if (twin && twin != pl && !twin->storageBits)
        (pl->storageBlendBits ? pl->blendTwin : pl->barrierTwin) = twin;
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
    std::vector<std::pair<std::string, Pipeline *>> out;
    for (const PackPipeline *rec : pack_.variantRecords(run)) {
        const std::string name = pack_.string(rec->variantName);
        // a barrier record is a storage form's twin unless it is the variant's one record (its storage form refused)
        if ((rec->flags & PipelineBarrierForm) && pack_.variantRecord(run, name, PackForm::Default))
            continue;
        Pipeline *pl = nullptr;
        {
            std::lock_guard<gm20b::Mutex> l(lock_);
            pl = placeRecord(*rec, caTransform);
        }
        // a storage form's barrier twin, as Device::pipeline gives a state's own
        const PackPipeline *barrier =
            pl && pl->storageBits ? pack_.variantRecord(run, name, PackForm::Barrier) : nullptr;
        if (barrier) {
            std::lock_guard<gm20b::Mutex> l(lock_);
            Pipeline *twin = placeRecord(*barrier, caTransform);
            if (twin && twin != pl && !twin->storageBits)
                (pl->storageBlendBits ? pl->blendTwin : pl->barrierTwin) = twin;
        }
        out.emplace_back(name, pl);
    }
    return out;
}

Pipeline *Device::placePipeline(const PackRun &run, uint64_t keyHash, PackForm form, bool caTransform)
{
    const PackPipeline *rec = pack_.find(run, form);
    std::lock_guard<gm20b::Mutex> l(lock_);
    if (!rec) {
        const auto made = completed_.find(keyHash);
        return form == PackForm::Default && made != completed_.end() ? made->second : nullptr;
    }
    return placeRecord(*rec, caTransform);
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

Pipeline *Device::placeRecord(const PackPipeline &pp, bool caTransform)
{
    Pipeline &pl = pipelines_[&pp - pack_.section<PackPipeline>(PackPipelines)];
    if (pl.rec)
        return &pl;
    const bool tess = tessellates(pp);
    PlacedProgram vs, fs = noPixelProgram(), tessPlaced[2];
    if (!placePackProgram(pp.vs, vs) || (pixelStage(pp) && !placePackProgram(pp.fs, fs)) ||
        (tess && (!placePackProgram(pp.tessPrograms[0], tessPlaced[0]) ||
                  !placePackProgram(pp.tessPrograms[1], tessPlaced[1]))))
        return nullptr;
    uint64_t slm = std::max(slmPerWarpOfHeader(vs.header), slmPerWarpOfHeader(fs.header));
    for (uint32_t k = 0; tess && k < 2; k++)
        slm = std::max(slm, slmPerWarpOfHeader(tessPlaced[k].header));
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
    build(pl, pp, vs, fs, pack_.section<uint32_t>(PackBlob) + pp.blob, pack_.string(pp.keyString), caTransform,
          pack_.spills(pp), pack_.far(pp), tess ? tessPlaced : nullptr);
    if (pp.restoreDwords && uint64_t(pp.restoreAt) + pp.restoreDwords <= pack_.bytes(PackBlob) / 4) {
        const std::vector<uint32_t> words =
            restoreWords(pack_.section<uint32_t>(PackBlob) + pp.restoreAt, pp.restoreDwords);
        pl.restore.insert(pl.restore.end(), words.begin(), words.end());
    }
    pl.strideTwin = twin;
    offerVariants(pl, pp);
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
    return &pl;
}

// lock_ held: `pl` of record `pp`, whose programs are placed at `vs` and `fs`, whose registers are `blob` (method, value pairs)
// and whose key's attachment and vertex fields are `keyString`
void Device::build(Pipeline &pl, const PackPipeline &pp, const PlacedProgram &vs, const PlacedProgram &fs,
                   const uint32_t *blob, const char *keyString, bool caTransform, const uint32_t *spills,
                   const uint32_t *far, const PlacedProgram *tess)
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
    const auto writesMemory = [](const PlacedProgram &p) {
        return (p.record->facts & kProgramWritesMemory) != 0;
    };
    pl.writesMemory[0] = writesMemory(vs) || (tess && (writesMemory(tess[0]) || writesMemory(tess[1])));
    pl.writesMemory[1] = writesMemory(fs);
    pl.memoryEntries = pp.memoryEntries;
    std::vector<uint32_t> words(64 + 16 + kStageSwitchWords + (tess ? 64 : 0) + pp.blobDwords * 2);
    Push w{words.data(), words.data() + words.size()};
    uint32_t writes[8];
    uint32_t interlockControl = 0;
    uint32_t sampleControl = 1;
    memcpy(writes, ctWrite_, sizeof writes);
    for (uint32_t k = 0; k < pp.blobDwords; k += 2) {
        const uint32_t m = blob[k], v = blob[k + 1];
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
        w.m(m, v);
    }
    w.m(PIXEL_SHADER_INTERLOCK_CONTROL, legalInterlockControl(interlockControl, sampleControl));
    w.m(HYBRID_ANTI_ALIAS_CONTROL, sampleControl);
    pl.blendKill = iteratedBlendNoop(blob, pp.blobDwords, writes, keyString,
                                     (pp.flags & PipelineStorageMetadata) ? pp.storageMask : 0) == kNoopSourceRgba0000;
    w.m(kIteratedBlendOptimization, pl.blendKill ? kNoopSourceRgba0000 : kNoopNever);
    SWITCH_METAL_DIAGNOSTIC(stats.blendKillStates += pl.blendKill);
    for (uint32_t c = 0; c < 8; c++)
        if (writes[c] & 0x1111)  // R_ENABLE, G_ENABLE, B_ENABLE, A_ENABLE
            pl.writeBits |= uint8_t(1u << c);
    w.m(0x2044, vs.program);  // SET_PIPELINE_PROGRAM(VERTEX)
    w.m(PIPELINE_REGISTER_COUNT_VERTEX, vs.record->gprs);
    if (pixelStage(pp)) {
        w.m(0x2144, fs.program);  // SET_PIPELINE_PROGRAM(PIXEL)
        w.m(PIPELINE_REGISTER_COUNT_PIXEL, fs.record->gprs);
    }
    if (fs.record->boundTextures)
        w.m(BINDLESS_TEXTURE, fs.record->boundTextures - 1u);
    bool exactCbufs = false;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    exactCbufs = probeExactCbufs.load(std::memory_order_relaxed);
#endif
    pl.setSlot[0] = bindProgramCbufs(w, *vs.record, kSetGroups[0], vs.data, root_.va, exactCbufs);
    pl.setSlot[1] = bindProgramCbufs(w, *fs.record, kSetGroups[1], fs.data, root_.va, exactCbufs);
    const PackProgram *programs[4] = {vs.record, fs.record};
    const uint16_t *farUbos[4] = {vs.farUbos, fs.farUbos};
    if (tess) {
        // SET_PATCH is in kOwnedMethods so the blob loop drops it; m2v's tessellation states always draw 1-point
        // patches.
        w.m(NVB197_SET_PATCH, 1);
    }
    for (uint32_t k = 0; tess && k < 2; k++) {
        w.m(PIPELINE_PROGRAM_TESS + kPipelineStageStride * k, tess[k].program);
        w.m(PIPELINE_REGISTER_COUNT_TESS + kPipelineStageStride * k, tess[k].record->gprs);
        pl.setSlot[2 + k] = bindProgramCbufs(w, *tess[k].record, kSetGroups[2 + k], tess[k].data, root_.va, exactCbufs);
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
    stageSwitches(pp, keyString, w, pl.restore);
    pl.stagesOff = !pixelStage(pp) || rasterOff(keyString);
    pl.methods.assign(words.data(), w.at);
    memcpy(pl.push, pp.pushWords, sizeof pl.push);
    pl.pushMask = pp.pushMask;
    pl.heapSlot[0] = heapCbufSlot(*vs.record);
    pl.heapSlot[1] = heapCbufSlot(*fs.record);
    bindArguments(pl, pp);
    for (uint32_t k = 0; k < 2; k++)
        if (spillEntryOf(pp, k))
            pl.spill[k] = spills ? spills + (k ? pp.spillWords[0] : 0) : nullptr;
    // the color formats of its key, and which colors its draws read as they were (blend_reads.h)
    for (const char *at = keyString; (at = strstr(at, " c")); at += 2) {
        unsigned c = 0;
        ColorKeyFields k = {};
        const int n = sscanf(at, " c%u=%u:%u:%u:%u:%u:%u:%u:%u:%u", &c, &k.format, &k.writeMask, &k.blending, &k.srcRGB,
                             &k.dstRGB, &k.rgbOp, &k.srcAlpha, &k.dstAlpha, &k.alphaOp);
        if (n >= 4 && c < 8) {
            // rendered through its stand-in (Texture::planarStandIn; an extended-range one's, format_standin.h)
            constexpr unsigned kRGB8_444_2P = 542, kRGBA16Float = 115;
            pl.colorFormats[c] = k.format == kRGB8_444_2P || xrTarget(k.format) ? kRGBA16Float : k.format;
            if (colorReadsDestination(k))
                pl.readBits |= uint8_t(1u << c);
            // MTLBlendFactorBlendColor, OneMinusBlendColor, BlendAlpha, OneMinusBlendAlpha
            for (const unsigned factor : {k.srcRGB, k.dstRGB, k.srcAlpha, k.dstAlpha})
                pl.constantBlend |= k.blending && factor >= 11 && factor <= 14;
        }
    }
    // attribute 0 ("a0=<MTLVertexFormat>:<offset>:<buffer>:<stride>:<step function>:..."): MTLVertexFormatFloat2, Float3
    // or Float4, per vertex, from the stream's buffer
    unsigned format = 0, offset = 0, buffer = 0, stride = 0, step = 0;
    if (const char *a0 = strstr(keyString, " a0=");
        a0 && sscanf(a0, " a0=%u:%u:%u:%u:%u", &format, &offset, &buffer, &stride, &step) == 5 && format >= 29 &&
        format <= 31 && buffer == pp.vertexSlot && stride == pp.vertexStride && step == 1) {
        pl.positionComponents = uint8_t(format - 27);
        pl.positionOffset = uint16_t(offset);
    }
    pl.caTransform = caTransform;
    if (const char *ds = strstr(keyString, "ds=")) {
        uint32_t depth = 0, stencil = 0;
        sscanf(ds, "ds=%u/%u", &depth, &stencil);
        // a Depth32Float or Depth16Unorm and a Stencil8 apart are rendered as one Depth32Float_Stencil8 (PassDesc::separate)
        pl.depthFormat = (depth == 252 || depth == 250) && stencil == 253 ? kZetaFormat : depth ? depth : stencil;
    }
    if (const char *n = strstr(keyString, "samples="))
        pl.samples = uint8_t(std::max(1, atoi(n + 8)));
    // SPH word 0 bit 15 KILLS_PIXELS, bit 608 OMAP_SAMPLE_MASK (cla097sph.h); SET_DEPTH_TEST, SET_STENCIL_TEST and
    // SET_ANTI_ALIAS_ALPHA_CONTROL's alpha to coverage in bit 0
    uint32_t sph[20];
    memcpy(sph, fs.header, sizeof sph);
    pl.primitiveId = (sph[5] >> 24) & 1;  // NVA097_SPHV3T2_IMAP_PRIMITIVE_ID, bit 184
    const auto doesGlobalStore = [](const uint8_t *header) {
        uint32_t sph0;
        memcpy(&sph0, header, sizeof sph0);
        return ((sph0 >> 16) & 1) != 0;  // NVA097_SPHV3T1_DOES_GLOBAL_STORE
    };
    pl.vertexStores =
        doesGlobalStore(vs.header) || (tess && (doesGlobalStore(tess[0].header) || doesGlobalStore(tess[1].header)));
    pl.fragmentDepth = (sph[19] >> 1) & 1;  // OMAP_DEPTH, bit 609
    pl.viewportIndex = headerWritesViewportIndex((tess ? tess[1] : vs).header);
    pl.writesLayer = headerWritesLayer((tess ? tess[1] : vs).header);
    bool tests = false;
    for (uint32_t k = 0; k < pp.blobDwords; k += 2) {
        const uint32_t m = blob[k], v = blob[k + 1];
        tests |= (m == DEPTH_TEST || m == STENCIL_TEST || m == ANTI_ALIAS_ALPHA_CONTROL) && (v & 1);
        pl.namesAlphaControl |= m == ANTI_ALIAS_ALPHA_CONTROL;
        if (m == HYBRID_ANTI_ALIAS_CONTROL)
            pl.samplePasses = uint8_t(std::max(1u, v & 0xf));
    }
    pl.dropsPixels = (sph[0] >> 15 & 1) || (sph[19] & 1) || tests || strncmp(keyString, "samples=1 ", 10) ||
                     !strstr(keyString, " ds=0/0 ");
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
            iteratedBlendWords(*pack_.iterated(pp), pl.freeForm.colour, pl.colorFormats[pl.freeForm.colour], pl.methods,
                               pl.restore);
        }
    }
    pl.turnRefused = pp.flags & PipelineTurnRefused;
    pl.xrSlots = pp.xrSlots;
    pl.writtenSlots = writtenSlots(pp);
    pl.own = isOwnState(pp.keyHash);
    pl.rec = &pp;
    pl.segment = segmentHeap_.place(pl.methods.data(), uint32_t(pl.methods.size()));
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
    SWITCH_METAL_DIAGNOSTIC(if (pl) pl->spanName = name);
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

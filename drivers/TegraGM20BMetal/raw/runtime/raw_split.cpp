// SPDX-License-Identifier: MIT
// Split fragment programs (raw_pack.h PackSplit): a state whose fragment program computes much from per-draw values alone
// has a twin whose per-pixel program reads those values from a constant buffer of its own, the frontier, that a compute
// prologue wrote for the draw. A large enough draw takes the twin; its prologue, one thread with the draw's constant buffers
// bound, is launched in one compute segment ahead of the command buffer's 3D work. Method numbers are NVIDIA's
// (kits/SwitchGM20B: Mesa's clb197.h, clb1c0.h, clb1c0qmd.h, MIT).
#include <algorithm>
#include <cstring>
#include "raw_gpu.h"
#include "raw_cbuf.h"
#include "../../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "raw_compute_visibility.h"
#include "raw_set_reads.h"
#include "raw_slm.h"
#include "raw_split_metadata.h"

namespace raw {
namespace {

enum : uint32_t {
    WAIT_FOR_IDLE = NVB1C0_WAIT_FOR_IDLE,
    INVALIDATE_SHADER_CACHES = NVB1C0_INVALIDATE_SHADER_CACHES,
    INVALIDATE_SKED_CACHES = NVB1C0_INVALIDATE_SKED_CACHES,
    SEND_PCAS_A = NVB1C0_SEND_PCAS_A,
    SEND_SIGNALING_PCAS_B = NVB1C0_SEND_SIGNALING_PCAS_B,
    CB_SELECTOR_A = NVB197_SET_CONSTANT_BUFFER_SELECTOR_A,
    BIND_GROUP_CB = NVB197_BIND_GROUP_CONSTANT_BUFFER(0),
};

constexpr uint32_t kRootBytes = 0x800, kMaxCbufBytes = 0x10000, kPixelGroup = 4;
constexpr uint32_t kFlushData = NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, FLUSH_DATA, TRUE),
                   kInvalidateData = NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, DATA, TRUE),
                   kInvalidateConstant = NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, CONSTANT, TRUE);
constexpr uint32_t kPcasInvalidateSchedule =
    NVDEF(NVB1C0, SEND_SIGNALING_PCAS_B, INVALIDATE, TRUE) | NVDEF(NVB1C0, SEND_SIGNALING_PCAS_B, SCHEDULE, TRUE);
// QMDV01_07 (cla1c0qmd.h): PROGRAM_OFFSET, and constant buffer i's ADDR_LOWER, then ADDR_UPPER (bits 0-7) and SIZE (15-31)
constexpr uint32_t kQmdProgramOffset = SWITCH_NVB1C0_QMD_WORD(NVB1C0_QMDV01_07_PROGRAM_OFFSET),
                   kQmdRasterWidth = SWITCH_NVB1C0_QMD_WORD(NVB1C0_QMDV01_07_CTA_RASTER_WIDTH),
                   kQmdRasterHeightDepth = SWITCH_NVB1C0_QMD_WORD(NVB1C0_QMDV01_07_CTA_RASTER_HEIGHT),
                   kQmdCbufSizeShift = SWITCH_NVB1C0_QMD_SHIFT(NVB1C0_QMDV01_07_CONSTANT_BUFFER_SIZE(0));

constexpr uint32_t qmdCbufLo(uint32_t i)
{
    return 29 + 2 * i;
}

// The set-0 entry whose uniform buffer the pixel program reads at constant buffer `slot` (Pipeline::ubos), or -1
int pixelUboEntry(const Pipeline &pl, uint8_t slot)
{
    for (uint8_t u = 0; u < pl.uboCount; u++)
        if (pl.ubos[u].group == kPixelGroup && pl.ubos[u].slot == slot)
            return pl.ubos[u].entry;
    return -1;
}

// The twin reads each uniform buffer from the set-0 entry its state does: its per-pixel program's constant buffer types name
// the same offsets (a UBO's type holds its offset)
bool bindsAsWhole(const Pipeline &twin, const Pipeline &whole)
{
    if (!twin.ubosNamed)
        return false;
    for (uint8_t u = 0; u < whole.uboCount; u++)
        if (whole.ubos[u].group == kPixelGroup && pixelUboEntry(twin, whole.ubos[u].slot) != whole.ubos[u].entry)
            return false;
    return true;
}

}  // namespace

void Device::offerSplit(Pipeline &pl, SplitSource &&source)
{
    if (!usableSplitMetadata(source.rec))
        return;
    splitSources_.push_back(std::move(source));
    __atomic_store_n(&pl.splitSource, &splitSources_.back(), __ATOMIC_RELEASE);
}

bool Device::placeSplitPrograms(const SplitSource &source, PlacedProgram &fs, uint32_t &prologue,
                                uint64_t &prologueData, uint32_t &prologueDataBytes)
{
    if (source.packed) {
        const PackProgram *progs = pack_.section<PackProgram>(PackPrograms);
        if (!place(source.rec.fs, fs.program, fs.data) || !place(source.rec.prologue, prologue, prologueData))
            return false;
        fs.record = &progs[source.rec.fs];
        fs.header = pack_.section<uint8_t>(PackHeap) + progs[source.rec.fs].program;
        prologueDataBytes = std::min(progs[source.rec.prologue].dataBytes, kMaxCbufBytes);
        return true;
    }
    uint64_t pixel = 0, compute = 0;
    if (!heapPlace(source.fsSlot.data(), uint32_t(source.fsSlot.size()), pixel) ||
        !heapPlace(source.prologueSlot.data(), uint32_t(source.prologueSlot.size()), compute))
        return false;
    RAW_DIAGNOSTIC(stats.programsPlaced += 2);
    fs.record = &source.fsInfo;
    fs.program = uint32_t(pixel) + source.fsInfo.program;
    fs.data = source.fsInfo.dataBytes ? heap_.va + pixel + source.fsInfo.dataOffset : 0;
    fs.header = source.fsSlot.data() + source.fsInfo.program;
    prologue = uint32_t(compute);
    prologueData = source.prologueDataBytes ? heap_.va + compute + source.prologueDataAt : 0;
    prologueDataBytes = std::min(source.prologueDataBytes, kMaxCbufBytes);
    return true;
}

// `pl`'s twin, made once, at its first draw that takes it: the pack's states and the runtime's records only offer theirs
// (offerSplit), so a split no draw is large enough for takes no heap. None where the heap or local memory has no room (the
// state draws whole from then on).
SplitState *Device::makeSplit(Pipeline &pl)
{
    RAW_ENCODE_LOCK(l, lock_);
    if (pl.split)
        return pl.split;
    const SplitSource *source = pl.splitSource;
    if (!source || !usableSplitMetadata(source->rec))
        return nullptr;
    PlacedProgram fs;
    uint32_t prologue = 0, prologueDataBytes = 0;
    uint64_t prologueData = 0;
    if (!placeSplitPrograms(*source, fs, prologue, prologueData, prologueDataBytes) ||
        !slmNeed(std::max(slmPerWarpOfHeader(fs.header), slmPerWarpOfQmd(source->rec.qmd)))) {
        stats.heapFull++;
        __atomic_store_n(&pl.splitSource, nullptr, __ATOMIC_RELEASE);
        return nullptr;
    }
    SplitState &s = splitStates_.emplace_back();
    s.rec = source->rec;
    s.prologue = prologue;
    s.prologueData = prologueData;
    s.prologueDataBytes = prologueDataBytes;
    // a state with argument tables is never split (raw_direct.cpp directRecord)
    build(s.pipeline, *source->pp, source->vs, fs, source->blob, source->keyString, source->caTransform, nullptr);
    s.pipeline.memoryEntries |= placedSetReads(fs.header, *fs.record, *source->pp);
    if (!bindsAsWhole(s.pipeline, pl)) {
        splitStates_.pop_back();
        __atomic_store_n(&pl.splitSource, nullptr, __ATOMIC_RELEASE);
        return nullptr;
    }
    s.pipeline.restore = pl.restore;
    s.pipeline.splitOf = &s;
    __atomic_store_n(&pl.split, &s, __ATOMIC_RELEASE);
    return &s;
}

Pipeline *RenderEncoder::splitTwin(Pipeline *pl, WrittenRegion::Analysis &analysis) const
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (cb_->forceWholePrograms_)
        return pl;
#endif
    const SplitSource *source = __atomic_load_n(&pl->splitSource, __ATOMIC_ACQUIRE);
    if (!source)
        return pl;
    // priced by its pixel limit, as the pass's plan admitted its state (RawMetalSplitPlan.h): no vertex is read
    const PixelBox box = analysis.geometry().limit;
    bool forced = false;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    forced = cb_->splitForced_;
#endif
    if (box.empty() || (!forced && !splitPlan_.accepts(pl, source->rec, uint64_t(box.x1 - box.x0) * (box.y1 - box.y0))))
        return pl;
    for (uint8_t k = 0; k < source->rec.uboCount; k++) {
        const int entry = pixelUboEntry(*pl, source->rec.uboCbuf[k]);
        if (entry < 0)
            return pl;
        const PackEntry &e = pl->rec->entry[entry];
        const Bound &b = bufs_[e.stage][e.slot];
        if (!b.va || (b.buffer && cb_->buffersWritten_))
            return pl;
    }
    SplitState *s = __atomic_load_n(&pl->split, __ATOMIC_ACQUIRE);
    if (!s)
        s = dev_->makeSplit(*pl);
    return s ? &s->pipeline : pl;
}

void RenderEncoder::launchSplit(Push &p, const Pipeline &twin, const uint32_t *set)
{
    RAW_ENCODE_SPAN(SplitLaunch);
    Device &d = *dev_;
    const SplitState &s = *twin.splitOf;
    SplitLaunch l;
    l.split = &s;
    for (uint8_t k = 0; k < s.rec.uboCount; k++) {
        const uint32_t *desc = set + twin.rec->entry[pixelUboEntry(twin, s.rec.uboCbuf[k])].offset / 4;
        l.cbuf[1 + k] = desc[0] | uint64_t(desc[1]) << 32;
        l.cbufBytes[1 + k] = std::min(desc[2], kMaxCbufBytes);
    }
    if (!s.rec.batchVersion && s.rec.shaderDataCbuf) {
        l.cbuf[s.rec.shaderDataCbuf] = s.prologueData;
        l.cbufBytes[s.rec.shaderDataCbuf] = s.prologueDataBytes;
    }
    SplitInputs inputs;
    inputs.program = &s;
    std::copy(std::begin(cb_->rootPush_), std::end(cb_->rootPush_), inputs.push.begin());
    std::copy(std::begin(l.cbuf), std::end(l.cbuf), inputs.buffers.begin());
    std::copy(std::begin(l.cbufBytes), std::end(l.cbufBytes), inputs.bytes.begin());
    // splitTwin already excludes GPU-written inputs; push words and bindings are CPU-cached snapshots.
    uint64_t frontier = cb_->splitReuse_.find(inputs);
    if (!frontier) {
        d.alloc(d.upload_, cbufBindBytes(s.rec.frontierBytes), kCbufUnit, frontier);
        if (s.rec.batchVersion == 1) {
            l.batchRecord = uint32_t(cb_->splitRecords_.size());
            SplitBatchRecord &record = cb_->splitRecords_.emplace_back();
            memcpy(record.push, cb_->rootPush_, sizeof record.push);
            record.frontier = frontier;
            for (unsigned i = 0; i < 8; ++i)
                record.buffers[i] = {l.cbuf[i], l.cbufBytes[i], 0};
        } else {
            uint8_t *params = d.alloc(d.upload_, kSplitParamsBytes, 256, l.cbuf[0]);
            memset(params, 0, kSplitParamsBytes);
            memcpy(params, cb_->rootPush_, sizeof cb_->rootPush_);
            memcpy(params + kSplitFrontierAddress, &frontier, sizeof frontier);
            l.cbufBytes[0] = kSplitParamsBytes;
            RAW_DIAGNOSTIC(d.stats.uploadBytes += kSplitParamsBytes);
        }
        cb_->useAddress(d.upload_.bo.va, d.upload_.bo.bytes, true);
        cb_->splits_.push_back(l);
        cb_->splitReuse_.remember(inputs, frontier);
        RAW_DIAGNOSTIC(d.stats.uploadBytes += s.rec.frontierBytes);
    }
    p.inc(CB_SELECTOR_A, {cbufBindBytes(s.rec.frontierBytes), uint32_t(frontier >> 32), uint32_t(frontier)});
    p.m(BIND_GROUP_CB + 0x20 * kPixelGroup, 1 | uint32_t(s.rec.frontierCbuf) << 4);
    p.inc(CB_SELECTOR_A, {kRootBytes, uint32_t(d.root_.va >> 32), uint32_t(d.root_.va)});
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
void RenderEncoder::captureSplit(const Pipeline &pl, WrittenRegion::Analysis &analysis, uint32_t primitive,
                                 uint32_t first, uint32_t count)
{
    uint32_t left = dev_->splitCaptureLeft.load(std::memory_order_relaxed);
    do {
        if (!left)
            return;
    } while (!dev_->splitCaptureLeft.compare_exchange_weak(left, left - 1, std::memory_order_relaxed));
    const PackPipeline &rec = *pl.rec;
    SplitCaptureRecord r;
    r.key = rec.keyHash;
    r.ca = pl.caTransform;
    if (const Texture *t = pass_.color[0].texture)
        r.format = t->format, r.width = t->width, r.height = t->height;
    PixelBox box;
    if (!analysis.bounds(box))
        box = analysis.geometry().limit;
    r.box[0] = box.x0, r.box[1] = box.y0, r.box[2] = box.x1, r.box[3] = box.y1;
    r.primitive = primitive, r.first = first, r.count = count, r.stride = rec.vertexStride;
    memcpy(r.push, cb_->rootPush_, sizeof r.push);
    auto copy = [](const Bound &b, uint64_t most) {
        return b.cpu ? std::vector<uint8_t>(b.cpu, b.cpu + std::min<uint64_t>(b.size, most)) : std::vector<uint8_t>();
    };
    // a draw whose vertices pass the cap is replayed over its box with the template's quad
    const uint64_t vertexBytes = uint64_t(first + count) * rec.vertexStride;
    if (rec.vertexSlot != 0xff && vertexBytes <= kSplitCaptureVertexBytes)
        r.vertices = copy(bufs_[1][rec.vertexSlot], vertexBytes);
    for (uint32_t k = 0; k < rec.entries && k < kPackEntries && r.blocks.size() < kSplitCaptureBlocks; k++) {
        const PackEntry &e = rec.entry[k];
        if (e.kind != EntryUbo)
            continue;
        const Bound &b = bufs_[e.stage][e.slot];
        if (b.cpu && b.size)
            r.blocks.push_back({e.stage, e.slot, uint32_t(b.va & 255), copy(b, kSplitCaptureBlockBytes)});
    }
    cb_->splitCaptures_.push_back(std::move(r));
}
#endif

// Inputs are immutable and splitTwin excludes preceding GPU writes, so these launches can share one producer boundary.
void CommandBuffer::launchPrologues()
{
    if (splits_.empty())
        return;
    RAW_ENCODE_SPAN(Prologues);
    Device &d = *dev_;
    splitBatches_.clear(splits_.size());
    uint32_t scalars = 0;
    for (uint32_t i = 0; i < splits_.size(); ++i) {
        const SplitState &split = *splits_[i].split;
        if (split.rec.batchVersion == 1)
            splitBatches_.append({split.prologue, split.prologueData, split.prologueDataBytes, split.rec.batchThreads},
                                 i);
        else
            ++scalars;
    }
    const uint32_t launches = scalars + uint32_t(splitBatches_.runs().size());
    const uint32_t words = 16 + 4 * launches;
    uint64_t va = 0;
    uint32_t *start = reinterpret_cast<uint32_t *>(d.alloc(d.push_, words * 4, 4, va));
    Push p{start, start + words};
    p.m(WAIT_FOR_IDLE, 0);
    p.m(INVALIDATE_SHADER_CACHES, kInvalidateData | kFlushData | kInvalidateConstant, SUBC_COMPUTE);
    p.m(INVALIDATE_SKED_CACHES, 0, SUBC_COMPUTE);
    auto launch = [&](const SplitState &split, const uint64_t *cbuf, const uint32_t *bytes, uint32_t groups) {
        uint64_t qmdVa = 0;
        uint32_t *qmd = reinterpret_cast<uint32_t *>(d.alloc(d.upload_, sizeof split.rec.qmd, 256, qmdVa));
        memcpy(qmd, split.rec.qmd, sizeof split.rec.qmd);
        qmd[kQmdProgramOffset] = split.prologue;
        qmd[kQmdRasterWidth] = groups;
        qmd[kQmdRasterHeightDepth] = 1 | 1u << 16;
        for (uint32_t i = 0; i < split.rec.prologueCbufs; ++i) {
            qmd[qmdCbufLo(i)] = uint32_t(cbuf[i]);
            qmd[qmdCbufLo(i) + 1] = (qmd[qmdCbufLo(i) + 1] & 0x7f00) | (uint32_t(cbuf[i] >> 32) & 0xff) |
                                    cbufBindBytes(bytes[i]) << kQmdCbufSizeShift;
        }
        p.m(SEND_PCAS_A, uint32_t(qmdVa >> 8), SUBC_COMPUTE);
        p.m(SEND_SIGNALING_PCAS_B, kPcasInvalidateSchedule, SUBC_COMPUTE);
    };
    for (const SplitLaunch &l : splits_)
        if (!l.split->rec.batchVersion)
            launch(*l.split, l.cbuf, l.cbufBytes, 1);
    for (const auto &run : splitBatches_.runs()) {
        const SplitState &split = *splits_[run.first].split;
        uint64_t tableVa = 0;
        const uint32_t tableBytes = run.count * sizeof(SplitBatchRecord);
        uint8_t *table = d.alloc(d.upload_, tableBytes, 256, tableVa);
        uint32_t row = 0;
        for (uint32_t i = run.first; i != SplitBatchPlanner::kEnd; i = splitBatches_.next(i))
            memcpy(table + sizeof(SplitBatchRecord) * row++, &splitRecords_[splits_[i].batchRecord],
                   sizeof(SplitBatchRecord));
        uint64_t cbuf[8] = {};
        uint32_t bytes[8] = {kSplitParamsBytes};
        uint8_t *parameters = d.alloc(d.upload_, kSplitParamsBytes, 256, cbuf[0]);
        const SplitBatchHeader header{tableVa, run.count, sizeof(SplitBatchRecord)};
        memset(parameters, 0, kSplitParamsBytes);
        memcpy(parameters, &header, sizeof header);
        if (split.rec.shaderDataCbuf) {
            cbuf[split.rec.shaderDataCbuf] = split.prologueData;
            bytes[split.rec.shaderDataCbuf] = split.prologueDataBytes;
        }
        launch(split, cbuf, bytes, (run.count + run.key.threads - 1) / run.key.threads);
        RAW_DIAGNOSTIC(d.stats.uploadBytes += tableBytes + kSplitParamsBytes);
    }
    computeToGraphics(p, false);
    RAW_DIAGNOSTIC(d.stats.uploadBytes += uint64_t(launches) * sizeof(PackSplit::qmd));
    RAW_DIAGNOSTIC(d.stats.pushBytes += uint64_t(p.at - start) * 4);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    RAW_DIAGNOSTIC(d.stats.splitPrologues += splits_.size());
    RAW_DIAGNOSTIC(d.stats.splitLaunches += launches);
#endif
    // the submission prologue may share segments_[0] with what followed it in the ring
    const TegraGM20BAbiPush head = segments_[0];
    segments_[0].dwords = prologueDwords_;
    if (head.dwords > prologueDwords_)
        segments_.insert(1, {head.va + uint64_t(prologueDwords_) * 4, head.dwords - prologueDwords_, head.flags});
    segments_.insert(1, {va, uint32_t(p.at - start), 0});
}

}  // namespace raw

// SPDX-License-Identifier: MIT
// Split fragment programs (raw_pack.h PackSplit): a state whose fragment program computes much from per-draw values alone
// has a twin whose per-pixel program reads those values from a constant buffer of its own, the frontier, that a compute
// prologue wrote for the draw. A large enough draw takes the twin; its prologue, one thread with the draw's constant buffers
// bound, is launched in one compute segment ahead of the command buffer's 3D work. Method numbers are NVIDIA's (Mesa
// src/nouveau/headers/nvidia/classes/clb197.h, clb1c0.h, cla1c0qmd.h, MIT).
#include <algorithm>
#include <cstring>
#include "raw_gpu.h"
#include "raw_slm.h"

namespace raw {
namespace {

enum : uint32_t {
    WAIT_FOR_IDLE = 0x0110,
    INVALIDATE_SHADER_CACHES = 0x021c,
    INVALIDATE_SKED_CACHES = 0x0298,
    SEND_PCAS_A = 0x02b4,
    SEND_SIGNALING_PCAS_B = 0x02bc,
    INVALIDATE_SHADER_CACHES_NO_WFI_3D = 0x0da4,
    CB_SELECTOR_A = 0x2380,
    BIND_GROUP_CB = 0x2410,
};

constexpr uint32_t kRootBytes = 0x800, kMaxCbufBytes = 0x10000, kPixelGroup = 4;
constexpr uint32_t kFlushData = 1u << 2, kInvalidateData = 1u << 4, kInvalidateConstant = 1u << 12;
constexpr uint32_t kPcasInvalidateSchedule = 3;
// QMDV01_07 (cla1c0qmd.h): PROGRAM_OFFSET, and constant buffer i's ADDR_LOWER, then ADDR_UPPER (bits 0-7) and SIZE (15-31)
constexpr uint32_t kQmdProgramOffset = 8, kQmdCbufSizeShift = 15;

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
    stats.programsPlaced += 2;
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
    std::lock_guard<std::mutex> l(lock_);
    if (pl.split)
        return pl.split;
    const SplitSource *source = pl.splitSource;
    if (!source)
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
    build(s.pipeline, *source->pp, source->vs, fs, source->blob, source->keyString, source->caTransform);
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

Pipeline *RenderEncoder::splitTwin(Pipeline *pl, uint32_t primitive, int32_t first, uint32_t count,
                                   const PixelBox &limit) const
{
    const SplitSource *source = __atomic_load_n(&pl->splitSource, __ATOMIC_ACQUIRE);
    if (!source)
        return pl;
    PixelBox box;
    if (!WrittenRegion::bounds(geometry(*pl, primitive, first, count, limit), box))
        box = limit;
    if (box.empty() || uint64_t(box.x1 - box.x0) * (box.y1 - box.y0) < source->rec.thresholdPixels)
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
    Device &d = *dev_;
    const SplitState &s = *twin.splitOf;
    SplitLaunch l;
    l.split = &s;
    uint64_t frontier = 0;
    d.alloc(d.upload_, (uint32_t(s.rec.frontierBytes) + 255) & ~255u, 256, frontier);
    uint8_t *params = d.alloc(d.upload_, kSplitParamsBytes, 256, l.cbuf[0]);
    memset(params, 0, kSplitParamsBytes);
    memcpy(params, cb_->rootPush_, sizeof cb_->rootPush_);
    memcpy(params + kSplitFrontierAddress, &frontier, sizeof frontier);
    l.cbufBytes[0] = kSplitParamsBytes;
    for (uint8_t k = 0; k < s.rec.uboCount; k++) {
        const uint32_t *desc = set + twin.rec->entry[pixelUboEntry(twin, s.rec.uboCbuf[k])].offset / 4;
        l.cbuf[1 + k] = desc[0] | uint64_t(desc[1]) << 32;
        l.cbufBytes[1 + k] = std::min(desc[2], kMaxCbufBytes);
    }
    if (s.rec.shaderDataCbuf) {
        l.cbuf[s.rec.shaderDataCbuf] = s.prologueData;
        l.cbufBytes[s.rec.shaderDataCbuf] = s.prologueDataBytes;
    }
    cb_->splits_.push_back(l);
    d.stats.uploadBytes += kSplitParamsBytes + s.rec.frontierBytes;
    p.inc(CB_SELECTOR_A, {s.rec.frontierBytes, uint32_t(frontier >> 32), uint32_t(frontier)});
    p.m(BIND_GROUP_CB + 0x20 * kPixelGroup, 1 | uint32_t(s.rec.frontierCbuf) << 4);
    p.inc(CB_SELECTOR_A, {kRootBytes, uint32_t(d.root_.va >> 32), uint32_t(d.root_.va)});
}

// The command buffer's prologues, one launch each, between its submission prologue and its first pass: ordered after the work
// submitted before (a wait for idle) with the caches they read through invalidated (the upload ring reuses addresses), and
// before its 3D work, which reads their frontiers through the constant cache (a wait for idle, the 3D constant cache dropped)
void CommandBuffer::launchPrologues()
{
    if (splits_.empty())
        return;
    Device &d = *dev_;
    const uint32_t words = 16 + 4 * uint32_t(splits_.size());
    uint64_t va = 0;
    uint32_t *start = reinterpret_cast<uint32_t *>(d.alloc(d.push_, words * 4, 4, va));
    Push p{start, start + words};
    p.m(WAIT_FOR_IDLE, 0);
    p.m(INVALIDATE_SHADER_CACHES, kInvalidateData | kFlushData | kInvalidateConstant, SUBC_COMPUTE);
    p.m(INVALIDATE_SKED_CACHES, 0, SUBC_COMPUTE);
    for (const SplitLaunch &l : splits_) {
        uint64_t qmdVa = 0;
        uint32_t *qmd = reinterpret_cast<uint32_t *>(d.alloc(d.upload_, sizeof l.split->rec.qmd, 256, qmdVa));
        memcpy(qmd, l.split->rec.qmd, sizeof l.split->rec.qmd);
        qmd[kQmdProgramOffset] = l.split->prologue;
        for (uint32_t i = 0; i < l.split->rec.prologueCbufs; i++) {
            qmd[qmdCbufLo(i)] = uint32_t(l.cbuf[i]);
            qmd[qmdCbufLo(i) + 1] = (uint32_t(l.cbuf[i] >> 32) & 0xff) | l.cbufBytes[i] << kQmdCbufSizeShift;
        }
        p.m(SEND_PCAS_A, uint32_t(qmdVa >> 8), SUBC_COMPUTE);
        p.m(SEND_SIGNALING_PCAS_B, kPcasInvalidateSchedule, SUBC_COMPUTE);
    }
    p.m(WAIT_FOR_IDLE, 0, SUBC_COMPUTE);
    p.m(INVALIDATE_SHADER_CACHES_NO_WFI_3D, kInvalidateData | kInvalidateConstant);
    d.stats.uploadBytes += uint64_t(splits_.size()) * sizeof(PackSplit::qmd);
    d.stats.pushBytes += uint64_t(p.at - start) * 4;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    d.stats.splitPrologues += splits_.size();
#endif
    // the submission prologue may share segments_[0] with what followed it in the ring
    const TegraGM20BAbiPush head = segments_[0];
    segments_[0].dwords = prologueDwords_;
    if (head.dwords > prologueDwords_)
        segments_.insert(segments_.begin() + 1,
                         {head.va + uint64_t(prologueDwords_) * 4, head.dwords - prologueDwords_, head.flags});
    segments_.insert(segments_.begin() + 1, {va, uint32_t(p.at - start), 0});
}

}  // namespace raw

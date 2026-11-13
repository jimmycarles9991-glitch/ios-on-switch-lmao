// SPDX-License-Identifier: MIT
// See raw_compute.h. Method numbers are NVIDIA's (kits/SwitchGM20B: Mesa's clb1c0.h, clb1c0qmd.h, MIT).
#include "raw_lock.h"
#include "raw_compute.h"
#include "raw_compute_slot.h"
#include "raw_compute_opening.h"
#include "../../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmitPredicate.h"
#include "raw_kit.h"
#include "raw_compute_visibility.h"
#include "raw_dispatch_bands.h"
#include "raw_stamps.h"
#include "raw_icb_compute.h"
#include "raw_residual.h"
#include "raw_slm.h"
#include "raw_view.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

namespace raw {
namespace {

enum : uint32_t {
    SEND_PCAS_A = NVB1C0_SEND_PCAS_A,
    SEND_SIGNALING_PCAS_B = NVB1C0_SEND_SIGNALING_PCAS_B,
    INVALIDATE_SHADER_CACHES = NVB1C0_INVALIDATE_SHADER_CACHES,
    INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI = NVB1C0_INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI,
};

constexpr uint32_t kRootBytes = 0x800, kRootSets = 0x38, kRootPush = 0x700, kUploadChunk = 0x100;
constexpr uint32_t kPcasInvalidateSchedule =
    NVDEF(NVB1C0, SEND_SIGNALING_PCAS_B, INVALIDATE, TRUE) | NVDEF(NVB1C0, SEND_SIGNALING_PCAS_B, SCHEDULE, TRUE);
constexpr uint32_t kInvalidateData = NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, DATA, TRUE),
                   kFlushData = NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, FLUSH_DATA, TRUE),
                   kInvalidateConstant = NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, CONSTANT, TRUE);
constexpr uint32_t kEncoderWords = 1 << 12;
// PackKernelEntry.slot of a constexpr sampler: its index in the pack's (pack/kernels.py CONSTEXPR)
constexpr uint8_t kConstexprSampler = 0x80;

// QMDV01_07 words
constexpr uint32_t kQmdProgramOffset = SWITCH_NVB1C0_QMD_WORD(NVB1C0_QMDV01_07_PROGRAM_OFFSET),
                   kQmdRasterWidth = SWITCH_NVB1C0_QMD_WORD(NVB1C0_QMDV01_07_CTA_RASTER_WIDTH),
                   kQmdRasterHeightDepth = SWITCH_NVB1C0_QMD_WORD(NVB1C0_QMDV01_07_CTA_RASTER_HEIGHT),
                   kQmdCbufValid = SWITCH_NVB1C0_QMD_WORD(NVB1C0_QMDV01_07_CONSTANT_BUFFER_VALID(0));

constexpr uint32_t qmdCbufLo(uint32_t i)
{
    return 29 + 2 * i;
}

// The states made, by device and key (a pack kernel's keyHash, a record's address): a device lives as long as its process
raw::Mutex gStatesLock;
std::map<std::pair<const Device *, uint64_t>, ComputeState> gStates;

}  // namespace

const ComputeState *computeState(Device &dev, uint64_t keyHash)
{
    std::lock_guard<raw::Mutex> hold(gStatesLock);
    if (auto it = gStates.find({&dev, keyHash}); it != gStates.end())
        return &it->second;
    const PackKernel *k = dev.pack().findKernel(keyHash);
    if (!k)
        return nullptr;
    ComputeState c;
    c.rec = k;
    {
        std::lock_guard<raw::Mutex> l(dev.lock_);
        if (!dev.slmNeed(slmPerWarpOfQmd(k->qmd)) || !dev.place(k->program, c.program, c.data)) {
            dev.stats.heapFull++;
            return nullptr;
        }
    }
    const PackProgram &p = dev.pack().section<PackProgram>(PackPrograms)[k->program];
    c.instructions = std::max(1u, (p.dataBytes ? p.dataOffset : p.bytes) / 8);
    memcpy(c.cbufType, p.cbufType, std::min<size_t>(sizeof c.cbufType, p.cbufs));
    c.spill = dev.pack().spills(*k);
    dev.bindKernelArguments(c, *k);
    return &gStates.emplace(std::make_pair(&dev, keyHash), c).first->second;
}

const ComputeState *placeCompute(Device &dev, const ComputeRecord &r)
{
    std::lock_guard<raw::Mutex> hold(gStatesLock);
    const uint64_t key = uint64_t(uintptr_t(&r));
    if (auto it = gStates.find({&dev, key}); it != gStates.end())
        return &it->second;
    const std::vector<uint8_t> slot = computeSlot(r);
    ComputeState c;
    c.placed = r.kernel;
    c.instructions = uint32_t(std::max<size_t>(1, r.code.size() / 8));
    uint64_t at = 0;
    {
        std::lock_guard<raw::Mutex> l(dev.lock_);
        for (uint32_t i = 0; i < c.placed.entries; i++) {
            PackKernelEntry &e = c.placed.entry[i];
            uint32_t index = 0;
            if (e.kind != KernelSampler || !(e.slot & kConstexprSampler))
                continue;
            if ((e.slot & ~kConstexprSampler) >= r.samplers.size() ||
                !dev.constexprSampler(r.samplers[e.slot & ~kConstexprSampler], index) || index >= kConstexprSampler)
                return nullptr;
            e.slot = uint8_t(kConstexprSampler | index);
        }
        if (!dev.slmNeed(slmPerWarpOfQmd(r.kernel.qmd)) || !dev.heapPlace(slot.data(), uint32_t(slot.size()), at)) {
            dev.stats.heapFull++;
            return nullptr;
        }
    }
    RAW_DIAGNOSTIC(dev.stats.programsPlaced++);
    memcpy(c.cbufType, r.cbufType, sizeof c.cbufType);
    c.program = uint32_t(at);
    c.data = r.data.empty() ? 0 : dev.heap_.va + at + computeSlotDataAt(r);
    c.spill = r.spill.size() == r.kernel.spillWords ? r.spill.data() : nullptr;
    dev.bindKernelArguments(c, r.kernel);
    ComputeState &placed = gStates.emplace(std::make_pair(&dev, key), c).first->second;
    placed.rec = &placed.placed;
    return &placed;
}

// What a kernel's reads through argument buffers take of every dispatch, as bindArguments for a state: its root table
// address (a kernel whose root table holds buffer slots takes its own per dispatch) and ExecutionStatus address words,
// and its set 1. Its dispatches are refused where the device has no memory for them.
void Device::bindKernelArguments(ComputeState &c, const PackKernel &k)
{
    const PackKernelEntry *end = k.entry + std::min<uint32_t>(k.entries, kKernelEntries);
    const bool tables = std::any_of(k.entry, end, [](const PackKernelEntry &e) {
        return e.kind == KernelTextureTable || e.kind == KernelSamplerTable;
    });
    if (!k.rootTableWord && !k.statusWord && !k.heapArrays && !tables)
        return;
    std::lock_guard<raw::Mutex> l(lock_);
    if (!heapMemory()) {
        c.heapMissing = true;
        return;
    }
    const auto address = [&](uint8_t word, uint64_t va) {
        if (!word || word > 31)
            return;
        c.argumentPush[word - 1] = uint32_t(va);
        c.argumentPush[word] = uint32_t(va >> 32);
        c.argumentMask |= 3u << (word - 1);
    };
    address(k.rootTableWord, k.rootSlots ? 0 : heapTables_.rootTableVa());
    address(k.statusWord, heapTables_.statusVa());
    if (k.heapArrays && !(c.heapSet = heapSet(heapLayout(k))))
        c.heapMissing = true;
}

ComputeEncoder::ComputeEncoder(CommandBuffer &cb, Device &dev) : cb_(cb), dev_(dev)
{
    cb_.heldSegment_.lose();
    uint64_t va;
    start_ = reinterpret_cast<uint32_t *>(dev_.alloc(dev_.push_, kEncoderWords * 4, 256, va));
    p_ = Push{start_, start_ + kEncoderWords};
    cb_.loops_.openSegment(stream());
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    probeComputeOpening(p_, cb.forms_, cb.lastEngine_, cb.taken_);
    cb.lastEngine_ = ProbeEngine::Compute;
#else
    computeOpening(p_);
#endif
}

void ComputeEncoder::room(uint32_t words)
{
    if (p_.room() >= words + 16)
        return;
    cb_.loops_.closeSegment(stream());
    dev_.push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - dev_.push_.bo.cpu);
    uint64_t va;
    auto *chunk = reinterpret_cast<uint32_t *>(dev_.alloc(dev_.push_, kEncoderWords * 4, 256, va));
    p_ = Push{chunk, chunk + kEncoderWords};
    cb_.loops_.openSegment(stream());
}

LoopStream ComputeEncoder::stream()
{
    return cb_.loopStream(p_, start_);
}

void ComputeEncoder::setBytes(uint32_t slot, const void *bytes, uint32_t length)
{
    if (slot >= 31)
        return;
    uint64_t va;
    const uint32_t chunk = std::max((length + kUploadChunk - 1) & ~(kUploadChunk - 1), kUploadChunk);
    uint8_t *m = dev_.alloc(dev_.upload_, chunk, kUploadChunk, va);
    memcpy(m, bytes, length);
    memset(m + length, 0, chunk - length);
    bindBuffer(slot, {va, length, nullptr, 0, dev_.upload_.bo.va});
    RAW_DIAGNOSTIC(dev_.stats.uploadBytes += chunk);
}

void ComputeEncoder::setBytesAt(uint32_t slot, uint64_t va, uint32_t length, const Buffer *allocation)
{
    if (slot < 31) {
        bindBuffer(slot, {va, length, nullptr, 0, allocation ? allocation->va : va});
        if (allocation) {
            cb_.use(*allocation, false);
            if (allocation->hostVisible && !allocation->wc && allocation->cpu && va >= allocation->va &&
                va - allocation->va <= allocation->length && length <= allocation->length - (va - allocation->va))
                cb_.clean(allocation->cpu + (va - allocation->va), length);
        }
    }
}

void ComputeEncoder::setBuffer(uint32_t slot, const Buffer *b, uint64_t offset)
{
    if (slot >= 31)
        return;
    if (!b) {
        bindBuffer(slot, {});
        return;
    }
    bindBuffer(slot, {b->va + offset,
                      uint32_t(std::min<uint64_t>(b->length - std::min(offset, b->length), 0xffffffffu)), b, offset});
}

void ComputeEncoder::setBufferOffset(uint32_t slot, uint64_t offset)
{
    if (slot < 31 && bufs_[slot].buffer)
        setBuffer(slot, bufs_[slot].buffer, offset);
}

void ComputeEncoder::setBufferStride(uint32_t slot, uint32_t stride)
{
    if (slot < 31 && stageIn_.strides[slot] != stride) {
        stageIn_.strides[slot] = stride;
        rootVa_ = 0;
    }
}

void ComputeEncoder::setStageInRegion(const std::array<uint32_t, 6> &region)
{
    const std::array<uint32_t, 8> words = {region[0], region[1], region[2], 0, region[3], region[4], region[5], 0};
    if (words != stageIn_.region) {
        stageIn_.region = words;
        rootVa_ = 0;
    }
}

void ComputeEncoder::barrier()
{
    room(8);
    p_.m(INVALIDATE_SHADER_CACHES, kInvalidateData | kFlushData | kInvalidateConstant, SUBC_COMPUTE);
    p_.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0, SUBC_COMPUTE);
}

Predication::Answer ComputeEncoder::startIf(uint64_t wordVa, PredicateCompare compare, uint32_t reference)
{
    room(Predication::kStartWords);
    uint64_t va;
    uint8_t *cpu = dev_.alloc(dev_.upload_, Predication::kSlotBytes, Predication::kSlotAlign, va);
    return cb_.predication_.startIf(p_, {cpu, va}, wordVa, predicateTest(compare, reference));
}

Predication::Answer ComputeEncoder::startElse()
{
    room(Predication::kStartWords);
    uint64_t va;
    uint8_t *cpu = dev_.alloc(dev_.upload_, Predication::kSlotBytes, Predication::kSlotAlign, va);
    return cb_.predication_.startElse(p_, {cpu, va});
}

Predication::Answer ComputeEncoder::endIf()
{
    return cb_.predication_.endIf();
}

void ComputeEncoder::setTexture(uint32_t slot, const Texture *t)
{
    if (slot < 31 && textures_[slot] != t) {
        textures_[slot] = t;
        ++bindingRevision_;
    }
}

void ComputeEncoder::setSampler(uint32_t slot, const Sampler *s)
{
    if (slot < 16 && samplers_[slot] != s) {
        samplers_[slot] = s;
        ++bindingRevision_;
    }
}

void ComputeEncoder::bindBuffer(uint32_t slot, const Bound &binding)
{
    if (!(bufs_[slot] == binding)) {
        bufs_[slot] = binding;
        ++bindingRevision_;
    }
}

bool ComputeEncoder::prepareDescriptors(const char **why)
{
    if (setVa_ && describedState_ == state_ && describedRevision_ == bindingRevision_)
        return true;
    // A refused rebuild may have changed scratch words/residuals. Never let it make the previous cache entry appear valid.
    setVa_ = rootVa_ = 0;
    describedState_ = nullptr;
    const PackKernel &r = *state_->rec;
    const uint32_t setBytes = std::max<uint32_t>((r.setBytes + 255) & ~255u, 256);
    descriptorWords_.assign(setBytes / 4, 0);
    uint32_t *set = descriptorWords_.data();
    residuals_.fill(0);
    residualMask_ = 0;
    spilled_ = {};
    if (r.spillEntry && !state_->spill) {
        *why = "an argument table whose words the kernel's record does not hold";
        return false;
    }
    for (uint32_t i = 0; i < r.entries; i++) {
        const PackKernelEntry &e = r.entry[i];
        const bool writes = kernelEntryWrites(r, i);
        uint32_t *at = set + e.offset / 4;
        if (e.kind == KernelBuffer) {
            const Bound &b = bufs_[e.slot];
            if (b.buffer)
                cb_.use(*b.buffer, writes);
            else
                cb_.useAddress(b.va, b.size, writes);
            BufferDescriptor desc;
            if (!bufferDescriptor(e, b.va, b.size, desc)) {
                *why = "a buffer bound at an offset that is not a multiple of 16";
                return false;
            }
            if (spilledWord(desc.word)) {
                spilled_.set(desc.word, desc.residual);
            } else if (desc.word >= 0) {
                residuals_[desc.word] = desc.residual;
                residualMask_ |= 1u << desc.word;
            }
            if (b.buffer && b.buffer->hostVisible && !b.buffer->wc && b.buffer->cpu)
                cb_.clean(b.buffer->cpu + b.offset, std::min<uint64_t>(desc.range - desc.residual, b.size));
            at[0] = uint32_t(desc.base);
            at[1] = uint32_t(desc.base >> 32);
            at[2] = b.va ? desc.range : 0;
            at[3] = 0;
        } else if (e.kind == KernelTexture) {
            const Texture *t = writes ? textures_[e.slot] : cb_.readTexture(textures_[e.slot]);
            if (t && !t->tic) {
                *why = "a texture the raw path cannot sample";
                return false;
            }
            if (t) {
                cb_.use(*t, writes);
                if (t->sampled)
                    cb_.use(*t->sampled, writes);
            }
            uint32_t tic = t ? t->tic : 0;
            if (const FormatInfo *f = t ? formatInfo(t->format) : nullptr) {
                const SlotView view =
                    slotView(e.window, f->tic0, swizzledWord0(f->tic0, t->swizzle), f->bpp, blockCompressed(f));
                if (view == SlotView::None) {
                    *why = "a kernel m2v binds a view of another texel size at a texture slot";
                    return false;
                }
                if (view == SlotView::View && !(tic = dev_.viewTic(*t, e.window, ViewAccess::Texture))) {
                    *why = "no texture header left for a kernel's view of another format";
                    return false;
                }
            }
            at[0] |= tic;
        } else if (e.kind == KernelSpillTable) {
            continue;
        } else if (e.kind == KernelTextureTable || e.kind == KernelSamplerTable) {
            const HeapTables &tables = dev_.heapTables_;
            const bool textures = e.kind == KernelTextureTable;
            const uint64_t va = textures ? tables.textureTableVa() : tables.samplerTableVa();
            at[0] = uint32_t(va);
            at[1] = uint32_t(va >> 32);
            at[2] = textures ? tables.textureTableBytes() : tables.samplerTableBytes();
            at[3] = 0;
        } else if (e.slot & kConstexprSampler) {
            const uint32_t index = e.slot & ~kConstexprSampler;
            if (index >= dev_.constexprCount_) {
                *why = "a kernel's constexpr sampler the pack holds no header of";
                return false;
            }
            at[0] |= dev_.constexprTsc_[index] << 20;
        } else {
            const Sampler *s = e.slot < 16 ? samplers_[e.slot] : nullptr;
            at[0] |= (s ? s->tsc : dev_.neutralTsc_) << 20;
        }
    }
    if (r.spillEntry && r.spillEntry <= r.entries) {
        uint32_t words[kSpillWordsMax];
        composeSpill(state_->spill, r.spillWords, spilled_, words);
        uint64_t va = 0;
        memcpy(dev_.alloc(dev_.upload_, spillRange(r.spillWords), 256, va), words, r.spillWords * sizeof(uint32_t));
        spillDescriptor(va, r.spillWords, set + r.entry[r.spillEntry - 1].offset / 4);
    }
    // All descriptor read/modify/write operations above touch cached CPU scratch, never the WC upload.
    uint8_t *upload = dev_.alloc(dev_.upload_, setBytes, 256, setVa_);
    memcpy(upload, set, setBytes);
    describedState_ = state_;
    describedRevision_ = bindingRevision_;
    rootVa_ = 0;
    RAW_DIAGNOSTIC(dev_.stats.uploadBytes += setBytes);
    return true;
}

bool ComputeEncoder::dispatch(const uint32_t groups[3], const uint32_t threads[3], const char **why)
{
    const auto plan = computeDispatch({groups[0], groups[1], groups[2]}, {threads[0], threads[1], threads[2]}, false);
    if (plan.reason != ComputeDispatch::Ready) {
        *why = "a dispatch outside the raw grid or local-size limits";
        return false;
    }
    return !plan.count || dispatchRegion(plan.regions[0], plan.grid, plan.nominal, true, true, why);
}

// NVK's root table of a dispatch (nvk_cmd_upload_qmd): the cs base group and group count, set 0's address and size, the push words with the
// SlotBindOffset residuals and m2v's DispatchRegion field
void ComputeEncoder::rootWords(const PackKernel &r, const uint32_t base[3], const uint32_t groups[3], uint64_t setVa,
                               uint32_t residualMask, const std::array<uint32_t, 32> &residuals,
                               const uint32_t region[8], uint32_t *root)
{
    for (uint32_t a = 0; a < 3; ++a) {
        root[a] = base[a];
        root[3 + a] = groups[a];
    }
    root[kRootSets / 4 + 0] = uint32_t(setVa);
    root[kRootSets / 4 + 1] = uint32_t(setVa >> 32);
    root[kRootSets / 4 + 2] = r.setBytes;
    for (uint32_t m = r.pushMask; m; m &= m - 1)
        root[kRootPush / 4 + __builtin_ctz(m)] = r.pushWords[__builtin_ctz(m)];
    for (uint32_t m = residualMask; m; m &= m - 1)
        root[kRootPush / 4 + __builtin_ctz(m)] = residuals[__builtin_ctz(m)];
    if (r.regionWord >= 0)
        memcpy(root + kRootPush / 4 + r.regionWord, region, std::min<size_t>(8 * 4, 4 * (32 - r.regionWord)));
}

// A command's root table and set 0 hold the record's 32 push words and the bound buffers and samplers: nothing a kernel
// reads through argument buffers (argumentRoot's words, set 1, the handle tables, an argument table), and no residual of a
// bind offset spilled past the push words
const char *ComputeEncoder::icbRefusal(const ComputeState &k)
{
    const PackKernel &r = *k.rec;
    if (k.argumentMask || k.heapSet || k.heapMissing || r.rootSlots || r.spillEntry)
        return "a kernel that reads argument buffers";
    for (uint32_t i = 0; i < r.entries; i++) {
        const PackKernelEntry &e = r.entry[i];
        if (e.kind == KernelTextureTable || e.kind == KernelSamplerTable || e.kind == KernelSpillTable)
            return "a kernel that reads argument buffers";
        if (e.kind == KernelBuffer && spilledWord(kernelResidualWord(e)))
            return "a kernel whose bind offset spills past its push window";
    }
    return nullptr;
}

bool ComputeEncoder::icbBinds(const ComputeState &k, uint64_t va, std::vector<icb::ComputeBind> &out,
                              std::vector<icb::ComputeStride> &strides, const char **why)
{
    if (const char *refusal = icbRefusal(k)) {
        *why = refusal;
        return false;
    }
    const PackKernel &r = *k.rec;
    const uint64_t rootVa = va + icb::kComputeQmdBytes, setVa = rootVa + icb::kComputeRootBytes;
    for (uint32_t i = 0; i < r.entries; i++) {
        const PackKernelEntry &e = r.entry[i];
        if (e.kind != KernelBuffer || e.slot >= 31)
            continue;
        const int word = kernelResidualWord(e);
        // the macro writes the residual itself, 0 for a base on 16 bytes: the word the record leaves for it must be 0
        if (word >= 0 && (r.pushMask >> word & 1) && r.pushWords[word]) {
            *why = "a kernel whose residual push word the record fills";
            return false;
        }
        out.push_back(
            {setVa + e.offset, word >= 0 ? rootVa + kRootPush + 4 * uint32_t(word) : 0, e.slot, kernelBufferWindow(e)});
    }
    for (const auto &stride : r.stageInStride)
        if (stride[1] && stride[1] <= 32 && stride[0] < 31)
            strides.push_back({rootVa + kRootPush + 4 * (stride[1] - 1u), stride[0]});
    return true;
}

bool ComputeEncoder::qmdWords(const ComputeState &k, const uint32_t groups[3], uint64_t rootVa, uint64_t setVa,
                              uint32_t *qmd, const char **why)
{
    memcpy(qmd, k.rec->qmd, sizeof k.rec->qmd);
    qmd[kQmdProgramOffset] = k.program;
    qmd[kQmdRasterWidth] = groups[0];
    qmd[kQmdRasterHeightDepth] = groups[1] | groups[2] << 16;
    for (uint32_t i = 0; i < 8; i++) {
        if (!(qmd[kQmdCbufValid] >> i & 1))
            continue;
        uint64_t va = 0;
        switch (cbufKind(k.cbufType[i])) {
        case CbufRoot:
            va = rootVa;
            break;
        case CbufSet:
            va = k.cbufType[i] == kCbufHeapSet ? (k.heapSet ? k.heapSet->memory.va : 0) : setVa;
            break;
        case CbufShaderData:
            va = k.data;
            break;
        default:
            *why = "a kernel reading a constant buffer the raw path does not bind";
            return false;
        }
        qmd[qmdCbufLo(i)] = uint32_t(va);
        qmd[qmdCbufLo(i) + 1] = (qmd[qmdCbufLo(i) + 1] & ~0xffu) | uint32_t(va >> 32);
    }
    return true;
}

bool ComputeEncoder::dispatchRegion(const ComputeRegion &part, const std::array<uint32_t, 3> &total,
                                    const std::array<uint32_t, 3> &nominal, bool first, bool last, const char **why)
{
    const auto *groups = part.groups.data();
    const auto *threads = part.local.data();
    cb_.buffersWritten_ = true;
    if (!state_) {
        *why = "a dispatch without a kernel";
        return false;
    }
    if (!groups[0] || !groups[1] || !groups[2])
        return true;
    if (groups[1] > 0xffff || groups[2] > 0xffff) {
        *why = "a grid taller or deeper than 65535 threadgroups";
        return false;
    }
    const PackKernel &r = *state_->rec;
    std::array<uint32_t, 8> region{};
    if (r.regionWord >= 0 && !computeRegionWords(part.base, total, nominal, region.data())) {
        *why = "a threadgroup m2v's dispatch region cannot name";
        return false;
    }
    if (state_->heapMissing) {
        *why = "no memory for the tables a kernel reads argument buffers through";
        return false;
    }
    if (!prepareDescriptors(why))
        return false;
    const std::array<uint32_t, 3> grid{groups[0], groups[1], groups[2]};
    uint64_t units = 1;
    for (uint32_t axis = 0; axis < 3; ++axis)
        units = laneWork(laneWork(units, groups[axis]), threads[axis]);
    const uint8_t axis = pieceAxis(grid);
    // A loop's body is laid down once and listed per copy (raw_loop_copies.h): no band cuts a batch inside it, and its
    // copies end the batches between them
    const bool looping = cb_.loops_.open();
    const uint32_t pieces =
        dev_.compositorClass() || looping
            ? 1
            : pieceCount(estimatedNs(state_->rate, units, workNs(laneWork(units, state_->instructions))),
                         dev_.bandBudgetNs(), grid[axis], kBandsPerCommandBuffer - cb_.bands);
    if (serial_ && dispatched_ && first)
        barrier();
    dispatched_ = true;
    if (pieces > 1 && cb_.batchWork_.operations)
        cutBatch();
    if (first)
        noteDispatch(units, looping);
    slotTableVa_ = 0;
    for (uint32_t i = 0; i < pieces; i++) {
        if (!launch(grid, dispatchPiece(grid, axis, i, pieces), region, why))
            return false;
        if (pieces > 1)
            cutBatch();
    }
    if (pieces > 1) {
        cb_.bands += pieces;
        RAW_DIAGNOSTIC(dev_.stats.bandedDispatches++; dev_.stats.bands += pieces);
    }
    uint64_t work = state_->instructions;
    for (uint32_t a = 0; a < 3; ++a)
        work = BatchWork::product(BatchWork::product(work, groups[a]), threads[a]);
    apiWork_ = std::min(BatchWork::kSaturated, (first ? 0 : apiWork_) + work);
    if (pieces < 2 && last && looping)
        cb_.loops_.addWork(apiWork_);
    else if (pieces < 2 && last && cb_.batchWork_.add(apiWork_, cb_.batchLimit()))
        cutBatch();
    RAW_DIAGNOSTIC(dev_.stats.dispatches++);
    return true;
}

// One launch of the bound kernel over `piece` of `grid`: its root table (the whole grid's cs.group_count, the piece's
// cs.base_group, raw_dispatch_bands.h), the QMD with the piece's raster
bool ComputeEncoder::launch(const std::array<uint32_t, 3> &grid, const DispatchPiece &piece,
                            const std::array<uint32_t, 8> &region, const char **why)
{
    const PackKernel &r = *state_->rec;
    if (!rootVa_ || grid_ != grid || base_ != piece.base || region_ != region) {
        uint32_t root[kRootBytes / 4] = {};
        rootWords(r, piece.base.data(), grid.data(), setVa_, residualMask_, residuals_, region.data(), root);
        argumentRoot(r, root);
        stageInWords(r, stageIn_, root + kRootPush / 4);
        uint8_t *upload = dev_.alloc(dev_.upload_, kRootBytes, 256, rootVa_);
        memcpy(upload, root, sizeof root);
        grid_ = grid;
        base_ = piece.base;
        region_ = region;
        RAW_DIAGNOSTIC(dev_.stats.uploadBytes += kRootBytes);
    }
    uint64_t qmdVa = 0;
    uint32_t qmd[kQmdWords];
    if (!qmdWords(*state_, piece.groups.data(), rootVa_, setVa_, qmd, why))
        return false;
#if defined(SWITCH_RAW_HARNESS_MARKS)
    if (benchQmd && !benchQmd(qmd))
        return true;
#endif
    uint8_t *qmdUpload = dev_.alloc(dev_.upload_, sizeof qmd, 256, qmdVa);
    memcpy(qmdUpload, qmd, sizeof qmd);
    RAW_DIAGNOSTIC(dev_.stats.uploadBytes += sizeof qmd);
    const bool looping = cb_.loops_.open();
    room(8 + Predication::kGuardWords + Predication::kReleaseWords +
         (looping ? SWITCH_GM20B_INLINE_WORDS_WORDS(kQmdWords) : 0));
    // Every copy of a loop launches this QMD: QMDV01_07 has words only the hardware writes (HW_ONLY_*, clb1c0qmd.h:429),
    // so each launch takes the CPU's bytes again, written in the stream before SEND_PCAS invalidates the scheduler's copy
    // as NVK writes an indirect dispatch's grid (nvk_CmdDispatchIndirect2KHR)
    if (looping)
        kit(p_, switch_gm20b_compute_inline_words, qmdVa, qmd, kQmdWords);
    cb_.predication_.guard(p_);
    p_.m(SEND_PCAS_A, uint32_t(qmdVa >> 8), SUBC_COMPUTE);
    p_.m(SEND_SIGNALING_PCAS_B, kPcasInvalidateSchedule, SUBC_COMPUTE);
    cb_.predication_.release(p_);
    return true;
}

void ComputeEncoder::cutBatch()
{
    cb_.segment(start_, p_.at);
    cb_.segments_.finishBatch();
    cb_.batchWork_ = {};
    start_ = p_.at;
}

// A dispatch alone in its encoder is measured as a draw alone in its pass is (raw_band_rates.h), with the host's semaphore
// after a wait for idle (the compute class's release has no pipeline location to wait at): the start stamp right after
// the encoder's opening wait for idle, which only the first dispatch follows, the end stamp ahead of the closing one. The
// start stamp goes out before the encoder knows the dispatch stays alone, so it is taken in a slot of its own; a second
// dispatch leaves it unread. The pick is counted at the end whether or not a start stamp went out.
void ComputeEncoder::noteDispatch(uint64_t units, bool looping)
{
    if (++dispatches_ != 1 || looping || dev_.compositorClass() || cb_.timings.size() >= kMeasuredPerCommandBuffer)
        return;
    candidate_ = {&state_->rate, units, 0, 0, state_->rec->keyHash, state_->instructions};
    if (!wouldMeasure(state_->rate, units))
        return;
    const uint32_t n = dev_.drawStamps_.fetch_add(1, std::memory_order_relaxed);
    candidate_.slot = n % kDrawStampSlots;
    candidate_.payload = n + 1 ? n + 1 : 1;
    room(kStampWords);
    emitStamp(p_, dev_.stamps_.va + drawStampOffset(candidate_.slot, false), candidate_.payload);
}

// The root table words of the kernel's reads through argument buffers: the device's root table and ExecutionStatus
// addresses, NVK's sets[1] for its heap arrays, and for a kernel whose root table holds buffer slots, that table of this
// dispatch's bound buffers in the upload ring (raw_heap_tables.h writeDrawRoots), each slot's buffer used as written
void ComputeEncoder::argumentRoot(const PackKernel &r, uint32_t *root)
{
    for (uint32_t m = state_->argumentMask; m; m &= m - 1)
        root[kRootPush / 4 + __builtin_ctz(m)] = state_->argumentPush[__builtin_ctz(m)];
    if (const HeapSet *heap = state_->heapSet) {
        root[kRootSets / 4 + 4] = uint32_t(heap->memory.va);
        root[kRootSets / 4 + 5] = uint32_t(heap->memory.va >> 32);
        root[kRootSets / 4 + 6] = r.heapBytes;
    }
    if (!r.rootSlots || !r.rootTableWord || r.rootTableWord > 31)
        return;
    if (slotTableVa_) {
        root[kRootPush / 4 + r.rootTableWord - 1] = uint32_t(slotTableVa_);
        root[kRootPush / 4 + r.rootTableWord] = uint32_t(slotTableVa_ >> 32);
        return;
    }
    const uint32_t entries = drawRootEntries(r.rootSlots);
    RootSlot slot[32];
    for (uint32_t n = 0; kFirstSlotRoot + n < entries; n++) {
        const Bound &b = bufs_[n];
        slot[n] = {};
        if (!(r.rootSlots >> n & 1) || !b.va)
            continue;
        if (b.buffer) {
            cb_.use(*b.buffer, true);
            slot[n] = {b.buffer->va, b.buffer->length, b.offset};
        } else {
            cb_.useAddress(b.va, b.size, true);
            slot[n] = {b.va, b.size, 0};
        }
    }
    uint64_t va = 0;
    auto *table = reinterpret_cast<RootEntry *>(dev_.alloc(dev_.upload_, entries * sizeof(RootEntry), 256, va));
    writeDrawRoots(table, va, r.rootSlots, slot, dev_.heapTables_.sinkVa(), dev_.heapTables_.identityVa());
    slotTableVa_ = va;
    root[kRootPush / 4 + r.rootTableWord - 1] = uint32_t(va);
    root[kRootPush / 4 + r.rootTableWord] = uint32_t(va >> 32);
    RAW_DIAGNOSTIC(dev_.stats.uploadBytes += entries * sizeof(RootEntry));
}

void ComputeEncoder::timestamp(uint64_t va)
{
    room(counters::kTimestampWords);
    cb_.useAddress(va, counters::kRecordBytes, true);
    counters::emitCompute(p_, va);
}

void ComputeEncoder::end()
{
    room(8 + kStampWords);
    if (dispatches_ == 1 && candidate_.rate && measure(*candidate_.rate, candidate_.units) && candidate_.payload) {
        emitStamp(p_, dev_.stamps_.va + drawStampOffset(candidate_.slot, true), candidate_.payload);
        cb_.timings.push_back(candidate_);
    }
    computeToGraphics(p_, true);
    cb_.loops_.closeSegment(stream());
    dev_.push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - dev_.push_.bo.cpu);
    RAW_DIAGNOSTIC(dev_.stats.pushBytes += uint64_t(p_.at - start_) * 4);
}

}  // namespace raw

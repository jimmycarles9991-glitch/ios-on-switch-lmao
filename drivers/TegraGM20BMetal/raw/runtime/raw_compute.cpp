// SPDX-License-Identifier: MIT
// See raw_compute.h. Method numbers are NVIDIA's (Mesa src/nouveau/headers/nvidia/classes/clb1c0.h, cla1c0qmd.h, MIT).
#include "raw_compute.h"
#include "raw_residual.h"
#include "raw_slm.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

namespace raw {
namespace {

enum : uint32_t {
    WAIT_FOR_IDLE = 0x0110,
    INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI = 0x0244,
    INVALIDATE_SKED_CACHES = 0x0298,
    SEND_PCAS_A = 0x02b4,
    SEND_SIGNALING_PCAS_B = 0x02bc,
    INVALIDATE_SHADER_CACHES = 0x021c,
    INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI = 0x1288,
    INVALIDATE_SHADER_CACHES_NO_WFI_3D = 0x0da4,
    INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI_3D = 0x1338,
    INVALIDATE_SAMPLER_CACHE_NO_WFI = 0x1424,
};

constexpr uint32_t kRootBytes = 0x800, kRootSets = 0x38, kRootPush = 0x700, kUploadChunk = 0x100;
constexpr uint32_t kPcasInvalidateSchedule = 3;
constexpr uint32_t kInvalidateData = 1u << 4, kFlushData = 1u << 2, kInvalidateConstant = 1u << 12;
constexpr uint32_t kEncoderWords = 1 << 12;
// PackKernelEntry.slot of a constexpr sampler: its index in the pack's (pack/kernels.py CONSTEXPR)
constexpr uint8_t kConstexprSampler = 0x80;

// QMDV01_07 (cla1c0qmd.h) words
constexpr uint32_t kQmdProgramOffset = 8, kQmdRasterWidth = 12, kQmdRasterHeightDepth = 13, kQmdCbufValid = 20;

constexpr uint32_t qmdCbufLo(uint32_t i)
{
    return 29 + 2 * i;
}

// The states made, by device and key (a pack kernel's keyHash, a record's address): a device lives as long as its process
std::mutex gStatesLock;
std::map<std::pair<const Device *, uint64_t>, ComputeState> gStates;

// m2v's DispatchRegion push field (metal2vulkan lower/dispatch.rs region_bytes): the region's first threadgroup, Metal's
// threadgroup size packed 10 bits an axis (each size - 1), the grid in threads
bool regionWords(const uint32_t groups[3], const uint32_t threads[3], uint32_t out[8])
{
    uint32_t nominal = 0;
    for (int i = 0; i < 3; i++) {
        if (!threads[i] || threads[i] > 1024)
            return false;
        nominal |= (threads[i] - 1) << (10 * i);
    }
    const uint32_t w[8] = {0, 0, 0, nominal, groups[0] * threads[0], groups[1] * threads[1], groups[2] * threads[2], 0};
    memcpy(out, w, sizeof w);
    return true;
}

}  // namespace

const ComputeState *computeState(Device &dev, uint64_t keyHash)
{
    std::lock_guard<std::mutex> hold(gStatesLock);
    if (auto it = gStates.find({&dev, keyHash}); it != gStates.end())
        return &it->second;
    const PackKernel *k = dev.pack().findKernel(keyHash);
    if (!k)
        return nullptr;
    ComputeState c;
    c.rec = k;
    {
        std::lock_guard<std::mutex> l(dev.lock_);
        if (!dev.place(k->program, c.program, c.data) || !dev.slmNeed(slmPerWarpOfQmd(k->qmd))) {
            dev.stats.heapFull++;
            return nullptr;
        }
    }
    const PackProgram &p = dev.pack().section<PackProgram>(PackPrograms)[k->program];
    memcpy(c.cbufType, p.cbufType, std::min<size_t>(sizeof c.cbufType, p.cbufs));
    return &gStates.emplace(std::make_pair(&dev, keyHash), c).first->second;
}

const ComputeState *placeCompute(Device &dev, const ComputeRecord &r)
{
    std::lock_guard<std::mutex> hold(gStatesLock);
    const uint64_t key = uint64_t(uintptr_t(&r));
    if (auto it = gStates.find({&dev, key}); it != gStates.end())
        return &it->second;
    const size_t dataAt = r.data.empty() ? r.code.size() : (r.code.size() + 0xff) & ~size_t(0xff);
    std::vector<uint8_t> slot(dataAt + r.data.size(), 0);
    memcpy(slot.data(), r.code.data(), r.code.size());
    if (!r.data.empty())
        memcpy(slot.data() + dataAt, r.data.data(), r.data.size());
    uint64_t at = 0;
    {
        std::lock_guard<std::mutex> l(dev.lock_);
        if (!dev.heapPlace(slot.data(), uint32_t(slot.size()), at) || !dev.slmNeed(slmPerWarpOfQmd(r.kernel.qmd))) {
            dev.stats.heapFull++;
            return nullptr;
        }
    }
    dev.stats.programsPlaced++;
    ComputeState c;
    c.rec = &r.kernel;
    memcpy(c.cbufType, r.cbufType, sizeof c.cbufType);
    c.program = uint32_t(at);
    c.data = r.data.empty() ? 0 : dev.heap_.va + at + dataAt;
    return &gStates.emplace(std::make_pair(&dev, key), c).first->second;
}

ComputeEncoder::ComputeEncoder(CommandBuffer &cb, Device &dev) : cb_(cb), dev_(dev)
{
    uint64_t va;
    start_ = reinterpret_cast<uint32_t *>(dev_.alloc(dev_.push_, kEncoderWords * 4, 256, va));
    p_ = Push{start_, start_ + kEncoderWords};
    // what the render and copy work before wrote, out of the SMs' data, constant and texture caches; the scheduler's cached
    // QMDs (the upload ring reuses their addresses) and the compute class's header caches, as NVK opens a command buffer's
    // compute work (nvk_cmd_buffer_begin_compute)
    p_.m(WAIT_FOR_IDLE, 0);
    p_.m(INVALIDATE_SHADER_CACHES, kInvalidateData | kFlushData | kInvalidateConstant, SUBC_COMPUTE);
    p_.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0, SUBC_COMPUTE);
    p_.m(INVALIDATE_SKED_CACHES, 0, SUBC_COMPUTE);
    p_.m(INVALIDATE_SAMPLER_CACHE_NO_WFI, 0, SUBC_COMPUTE);
    p_.m(INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, 0, SUBC_COMPUTE);
}

void ComputeEncoder::room(uint32_t words)
{
    if (p_.room() >= words + 16)
        return;
    cb_.segment(start_, p_.at);
    dev_.push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - dev_.push_.bo.cpu);
    uint64_t va;
    start_ = reinterpret_cast<uint32_t *>(dev_.alloc(dev_.push_, kEncoderWords * 4, 256, va));
    p_ = Push{start_, start_ + kEncoderWords};
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
    bufs_[slot] = {va, length, nullptr, 0};
    dev_.stats.uploadBytes += chunk;
}

void ComputeEncoder::setBytesAt(uint32_t slot, uint64_t va, uint32_t length)
{
    if (slot < 31)
        bufs_[slot] = {va, length, nullptr, 0};
}

void ComputeEncoder::setBuffer(uint32_t slot, const Buffer *b, uint64_t offset)
{
    if (slot >= 31)
        return;
    if (!b) {
        bufs_[slot] = {};
        return;
    }
    bufs_[slot] = {b->va + offset, uint32_t(std::min<uint64_t>(b->length - std::min(offset, b->length), 0xffffffffu)),
                   b, offset};
}

void ComputeEncoder::setBufferOffset(uint32_t slot, uint64_t offset)
{
    if (slot < 31 && bufs_[slot].buffer)
        setBuffer(slot, bufs_[slot].buffer, offset);
}

void ComputeEncoder::barrier()
{
    room(8);
    p_.m(INVALIDATE_SHADER_CACHES, kInvalidateData | kFlushData | kInvalidateConstant, SUBC_COMPUTE);
    p_.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0, SUBC_COMPUTE);
}

void ComputeEncoder::setTexture(uint32_t slot, const Texture *t)
{
    if (slot < 31)
        textures_[slot] = t;
}

void ComputeEncoder::setSampler(uint32_t slot, const Sampler *s)
{
    if (slot < 16)
        samplers_[slot] = s;
}

bool ComputeEncoder::dispatch(const uint32_t groups[3], const uint32_t threads[3], const char **why)
{
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
    uint32_t region[8] = {};
    if (r.regionWord >= 0 && !regionWords(groups, threads, region)) {
        *why = "a threadgroup m2v's dispatch region cannot name";
        return false;
    }
    const uint32_t setBytes = std::max<uint32_t>((r.setBytes + 255) & ~255u, 256);
    uint64_t setVa = 0;
    uint32_t *set = reinterpret_cast<uint32_t *>(dev_.alloc(dev_.upload_, setBytes, 256, setVa));
    memset(set, 0, setBytes);
    uint32_t residuals[32] = {}, residualMask = 0;
    for (uint32_t i = 0; i < r.entries; i++) {
        const PackKernelEntry &e = r.entry[i];
        uint32_t *at = set + e.offset / 4;
        if (e.kind == KernelBuffer) {
            const Bound &b = bufs_[e.slot];
            BufferDescriptor desc;
            if (!bufferDescriptor(e, b.va, b.size, desc)) {
                *why = "a buffer bound at an offset that is not a multiple of 16";
                return false;
            }
            if (desc.word >= 0) {
                residuals[desc.word] = desc.residual;
                residualMask |= 1u << desc.word;
            }
            if (b.buffer && !b.buffer->wc && b.buffer->cpu)
                cb_.clean(b.buffer->cpu + b.offset, std::min<uint64_t>(desc.range - desc.residual, b.size));
            at[0] = uint32_t(desc.base);
            at[1] = uint32_t(desc.base >> 32);
            at[2] = b.va ? desc.range : 0;
            at[3] = 0;
        } else if (e.kind == KernelTexture) {
            const Texture *t = textures_[e.slot];
            if (t && !t->tic) {
                *why = "a texture the raw path cannot sample";
                return false;
            }
            const FormatInfo *f = t ? formatInfo(t->format) : nullptr;
            if (f && e.window && f->tic0 != e.window) {
                *why = "a kernel m2v binds a view of another format at a texture slot";
                return false;
            }
            at[0] |= t ? t->tic : 0;
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
    uint64_t rootVa = 0;
    uint32_t *root = reinterpret_cast<uint32_t *>(dev_.alloc(dev_.upload_, kRootBytes, 256, rootVa));
    memset(root, 0, kRootBytes);
    root[3] = groups[0];
    root[4] = groups[1];
    root[5] = groups[2];
    root[kRootSets / 4 + 0] = uint32_t(setVa);
    root[kRootSets / 4 + 1] = uint32_t(setVa >> 32);
    root[kRootSets / 4 + 2] = r.setBytes;
    for (uint32_t m = r.pushMask; m; m &= m - 1)
        root[kRootPush / 4 + __builtin_ctz(m)] = r.pushWords[__builtin_ctz(m)];
    for (uint32_t m = residualMask; m; m &= m - 1)
        root[kRootPush / 4 + __builtin_ctz(m)] = residuals[__builtin_ctz(m)];
    if (r.regionWord >= 0)
        memcpy(root + kRootPush / 4 + r.regionWord, region, std::min<size_t>(sizeof region, 4 * (32 - r.regionWord)));
    uint64_t qmdVa = 0;
    uint32_t *qmd = reinterpret_cast<uint32_t *>(dev_.alloc(dev_.upload_, kQmdWords * 4, 256, qmdVa));
    memcpy(qmd, r.qmd, sizeof r.qmd);
    qmd[kQmdProgramOffset] = state_->program;
    qmd[kQmdRasterWidth] = groups[0];
    qmd[kQmdRasterHeightDepth] = groups[1] | groups[2] << 16;
    for (uint32_t i = 0; i < 8; i++) {
        if (!(qmd[kQmdCbufValid] >> i & 1))
            continue;
        uint64_t va = 0;
        switch (cbufKind(state_->cbufType[i])) {
        case CbufRoot:
            va = rootVa;
            break;
        case CbufSet:
            va = setVa;
            break;
        case CbufShaderData:
            va = state_->data;
            break;
        default:
            *why = "a kernel reading a constant buffer the raw path does not bind";
            return false;
        }
        qmd[qmdCbufLo(i)] = uint32_t(va);
        qmd[qmdCbufLo(i) + 1] = (qmd[qmdCbufLo(i) + 1] & ~0xffu) | uint32_t(va >> 32);
    }
    dev_.stats.uploadBytes += setBytes + kRootBytes + kQmdWords * 4;
    if (serial_ && dispatched_)
        barrier();
    dispatched_ = true;
    room(8);
    p_.m(SEND_PCAS_A, uint32_t(qmdVa >> 8), SUBC_COMPUTE);
    p_.m(SEND_SIGNALING_PCAS_B, kPcasInvalidateSchedule, SUBC_COMPUTE);
    dev_.stats.dispatches++;
    return true;
}

void ComputeEncoder::end()
{
    room(8);
    p_.m(WAIT_FOR_IDLE, 0, SUBC_COMPUTE);
    p_.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI_3D, 0);
    p_.m(INVALIDATE_SHADER_CACHES_NO_WFI_3D, kInvalidateData | kInvalidateConstant);
    cb_.segment(start_, p_.at);
    dev_.push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - dev_.push_.bo.cpu);
    dev_.stats.pushBytes += uint64_t(p_.at - start_) * 4;
}

}  // namespace raw

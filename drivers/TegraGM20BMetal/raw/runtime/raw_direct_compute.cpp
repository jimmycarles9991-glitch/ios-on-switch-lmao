// SPDX-License-Identifier: MIT
// See raw_direct_compute.h. A kernel's record from m2v's plans of three dispatches (inline bytes, buffers elsewhere, buffers off 16)
// and NVK's own rules for the QMD, the layout and the descriptors.
#include "raw_direct_compute.h"

#include <algorithm>
#include <cstring>
#include <map>
#include "../pack/probe_bind.h"
#include "bridge/NativeMetalDirect.h"
#include "raw_compute_key.h"
#include "raw_direct_nvk.h"
#include "raw_residual.h"

namespace raw {
namespace {

constexpr uint32_t kTagBytes = 4096;  // the probe's payload: a descriptor's range below it is the function's window
constexpr uint32_t kRootPush = 0x700;
constexpr uint8_t kConstexpr = 0x80;
constexpr size_t kEntries = sizeof(PackKernel::entry) / sizeof(PackKernelEntry);
constexpr size_t kCbufs = sizeof(ComputeRecord::cbufType);

class Compiled {
public:
    Compiled(const DirectNvk &nvk, void *device) : nvk_(nvk), device_(device)
    {}

    ~Compiled()
    {
        if (stage.owner)
            nvk::stageFinish(nvk_, device_, stage);
    }

    NvkStage stage = {};

private:
    const DirectNvk &nvk_;
    void *device_;
};

// The root push words a dispatch writes but m2v's DispatchRegion field, which raw_compute.cpp writes per dispatch, by root offset
std::map<uint32_t, uint32_t> pushWords(uint32_t mask, const uint32_t (&words)[32], int32_t region)
{
    std::map<uint32_t, uint32_t> out;
    for (uint32_t i = 0; i < 32; i++)
        if (mask >> i & 1 && words[i] && !(region >= 0 && int32_t(i) >= region && int32_t(i) < region + 8))
            out[kRootPush + 4 * i] = words[i];
    return out;
}

// What each dispatch sets cleared: its program offset, grid and constant buffer addresses (raw_compute.cpp)
void clearDispatchWords(uint32_t (&q)[64])
{
    q[8] = q[12] = q[13] = 0;
    for (int i = 0; i < 8; i++) {
        q[29 + 2 * i] = 0;
        q[30 + 2 * i] &= ~0xFFu;
    }
}

uint8_t samplerSlot(ComputeRecord &out, const std::array<uint32_t, 8> &header)
{
    auto at = std::find(out.samplers.begin(), out.samplers.end(), header);
    if (at == out.samplers.end())
        at = out.samplers.insert(at, header);
    return uint8_t(kConstexpr | (at - out.samplers.begin()));
}

class Entries {
public:
    Entries(const NmbDirectKernel &k, const DirectNvk &nvk, ComputeRecord &out)
        : k_(k), nvk_(nvk), device_(handle(k.device)), out_(out)
    {}

    bool add(const NmbDirectKernelWrite &kw, std::string &why)
    {
        const NmbDirectWrite &w = kw.write;
        NvkSetBinding b = {};
        if (w.set != 0) {
            why = "a set-" + std::to_string(w.set) + " descriptor (binding " + std::to_string(w.binding) + ")";
            return false;
        }
        if (!k_.set0 || !nvk::setLayoutBinding(nvk_, k_.set0, w.binding, b)) {
            why = "set 0 binding " + std::to_string(w.binding) + " not in its layout";
            return false;
        }
        const uint16_t offset = uint16_t(b.offset + w.element * b.stride);
        if (bufferType(w.type))
            return buffer(w, offset, why);
        if (w.type == SampledImage || w.type == StorageImage || w.type == CombinedImageSampler) {
            if (!texture(kw, offset, why))
                return false;
            return w.type != CombinedImageSampler || sampler(kw, offset, why);
        }
        if (w.type == Sampler)
            return sampler(kw, offset, why);
        why = "a set-0 descriptor of type " + std::to_string(w.type) + " (source " + std::to_string(w.source) + ")";
        return false;
    }

    std::vector<PackKernelEntry> entries;

private:
    const NmbDirectKernel &k_;
    const DirectNvk &nvk_;
    void *device_;
    ComputeRecord &out_;

    bool buffer(const NmbDirectWrite &w, uint16_t offset, std::string &why)
    {
        if (w.source != NmbDirectBuffer && w.source != NmbDirectBytes) {
            why = "a set-0 buffer m2v binds itself at " + std::to_string(offset) + " (source " +
                  std::to_string(w.source) + ")";
            return false;
        }
        const uint32_t size = nvk::bufferDescriptorSize(nvk_, device_, w.type, w.range);
        entries.push_back({offset, KernelBuffer, uint8_t(w.slot), size < kTagBytes ? size : 0});
        return true;
    }

    bool texture(const NmbDirectKernelWrite &kw, uint16_t offset, std::string &why)
    {
        const NmbDirectWrite &w = kw.write;
        const bool slotted =
            w.source == NmbDirectTexture || (w.source == NmbDirectCombined && w.slot != NMB_DIRECT_NEUTRAL);
        uint32_t word0 = 0;
        if (!slotted || !kw.viewFormat) {
            why = "a texture slot the dispatch leaves unbound at " + std::to_string(offset);
            return false;
        }
        const NvkComponents c = {kw.components[0], kw.components[1], kw.components[2], kw.components[3]};
        if (!nvk::viewHeaderWord0(nvk_, device_, kw.viewFormat, c, w.type == StorageImage, word0)) {
            why = "a texture view of VkFormat " + std::to_string(kw.viewFormat) + " NVK has no header for";
            return false;
        }
        entries.push_back({offset, KernelTexture, uint8_t(w.slot), word0});
        return true;
    }

    bool sampler(const NmbDirectKernelWrite &kw, uint16_t offset, std::string &why)
    {
        const NmbDirectWrite &w = kw.write;
        std::array<uint32_t, 8> header = {};
        const bool known =
            w.sampler == NMB_DIRECT_IMMUTABLE
                ? nvk::immutableSamplerHeader(nvk_, device_, k_.set0, w.binding, w.element, header.data())
                : kw.sampler && nvk::samplerHeader(nvk_, device_, kw.sampler, header.data());
        if (!known) {
            why = "a sampler slot the dispatch leaves unbound at " + std::to_string(offset);
            return false;
        }
        entries.push_back({offset, KernelSampler, samplerSlot(out_, header), 0});
        return true;
    }
};

// Each buffer entry's root push word the bind-offset dispatch set to its slot's residual (probeComputeCode), where no other
// buffer slot of the kernel shares the code and no other word holds it
void attachResiduals(const NmbDirectKernel &k, std::vector<PackKernelEntry> &entries)
{
    if (!k.boundSame)
        return;
    const std::map<uint32_t, uint32_t> words = residualWordsByValue(pushWords(k.pushMask, k.push, k.regionWord),
                                                                    pushWords(k.boundMask, k.bound, k.regionWord));
    std::map<uint32_t, int> codes;
    for (const PackKernelEntry &e : entries)
        if (e.kind == KernelBuffer)
            codes[probeComputeCode(e.slot)]++;
    for (PackKernelEntry &e : entries) {
        const uint32_t code = probeComputeCode(e.slot);
        const auto word = words.find(code);
        if (e.kind == KernelBuffer && codes[code] == 1 && word != words.end())
            e.window |= ((word->second - kRootPush) / 4 + 1) << 24;
    }
}

Compiler::Outcome compile(const NmbDirectKernel &k, const DirectNvk &nvk, Compiled &made, std::string &why)
{
    SubgroupSize subgroup;
    subgroup.size = k.stage.subgroupSize;
    const NvkStageInfo info = {k.stage.spirv,
                               k.stage.spirvBytes,
                               k.stage.entry,
                               k.stage.stage,
                               k.stage.specialization,
                               handle(k.layout),
                               0,
                               nullptr,
                               k.stage.subgroupSize ? &subgroup : nullptr,
                               false,
                               false,
                               0,
                               0,
                               {}};
    if (k.stage.stage != kComputeStage) {
        why = "a stage of VkShaderStageFlags " + std::to_string(k.stage.stage);
        return Compiler::Outcome::Refused;
    }
    if (const int32_t result = nvk::compileStage(nvk, handle(k.device), info, made.stage); result != 0) {
        why = "NVK refused the kernel (VkResult " + std::to_string(result) + ")";
        return Compiler::Outcome::Failed;
    }
    if (made.stage.cbufCount > kCbufs) {
        why = std::to_string(made.stage.cbufCount) + " constant buffers";
        return Compiler::Outcome::Refused;
    }
    return Compiler::Outcome::Made;
}

// What sends a kernel's dispatch to no record
bool recordable(const NmbDirectKernel &k, std::string &why)
{
    if (k.pushHoles)
        why = std::to_string(k.pushHoles) + " push words m2v fills with addresses";
    else if (k.regions != 1)
        why = std::to_string(k.regions) + " Vulkan dispatches for one";
    else if (!k.movedSame ||
             pushWords(k.pushMask, k.push, k.regionWord) != pushWords(k.movedMask, k.moved, k.regionWord))
        why = "a program or push word the bound addresses or samplers decide";
    else
        return true;
    return false;
}

}  // namespace

Compiler::Outcome directKernel(const NmbDirectKernel &k, const DirectNvk &nvk, const std::string &key,
                               ComputeRecord &out, std::string &why)
{
    out = {};
    if (!recordable(k, why))
        return Compiler::Outcome::Refused;
    Compiled made(nvk, handle(k.device));
    if (const Compiler::Outcome compiled = compile(k, nvk, made, why); compiled != Compiler::Outcome::Made)
        return compiled;
    Entries entries(k, nvk, out);
    for (uint32_t i = 0; i < k.writeCount; i++)
        if (!entries.add(k.writes[i], why))
            return Compiler::Outcome::Refused;
    std::stable_sort(entries.entries.begin(), entries.entries.end(),
                     [](const PackKernelEntry &a, const PackKernelEntry &b) {
                         return a.offset < b.offset;
                     });
    if (entries.entries.size() > kEntries) {
        why = std::to_string(entries.entries.size()) + " set-0 entries";
        return Compiler::Outcome::Refused;
    }
    attachResiduals(k, entries.entries);
    PackKernel &r = out.kernel;
    r.setBytes = k.set0 ? uint16_t(nvk::setLayoutSize(nvk, handle(k.device), k.set0)) : 0;
    if (!nvk::computeQmd(nvk, handle(k.device), made.stage, r.setBytes, r.qmd)) {
        why = "a kernel reading a constant buffer the raw path does not bind";
        return Compiler::Outcome::Refused;
    }
    clearDispatchWords(r.qmd);
    r.keyHash = computeKeyHash(key);
    r.regionWord = int8_t(k.regionWord);
    r.entries = uint8_t(entries.entries.size());
    std::copy(entries.entries.begin(), entries.entries.end(), r.entry);
    for (const auto &[offset, word] : pushWords(k.pushMask, k.push, k.regionWord)) {
        r.pushMask |= 1u << (offset - kRootPush) / 4;
        r.pushWords[(offset - kRootPush) / 4] = word;
    }
    const auto *code = static_cast<const uint8_t *>(made.stage.code),
               *data = static_cast<const uint8_t *>(made.stage.data);
    out.code.assign(code, code + made.stage.codeBytes);
    out.data.assign(data, data + made.stage.dataBytes);
    std::copy(made.stage.cbufTypes, made.stage.cbufTypes + made.stage.cbufCount, out.cbufType);
    out.cbufs = uint8_t(made.stage.cbufCount);
    out.keyString = key;
    return Compiler::Outcome::Made;
}

}  // namespace raw

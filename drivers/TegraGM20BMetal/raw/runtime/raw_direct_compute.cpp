// SPDX-License-Identifier: MIT
// See raw_direct_compute.h. A kernel's record from m2v's plans of three dispatches (inline bytes, buffers elsewhere, buffers off 16)
// and NVK's rules for the QMD, the layout and the descriptors (raw_direct_encode.h).
#include "raw_direct_compute.h"

#include <algorithm>
#include <cstring>
#include <map>
#include "bridge/NativeMetalDirect.h"
#include "raw_compute_key.h"
#include "raw_direct_encode.h"
#include "raw_direct_nvk.h"
#include "raw_spill_table.h"

namespace raw {
namespace {

constexpr uint32_t kTagBytes = 4096;  // the probe's payload: a descriptor's range below it is the function's window
constexpr uint32_t kRootPush = 0x700;
constexpr uint8_t kConstexpr = 0x80;
constexpr size_t kEntries = sizeof(PackKernel::entry) / sizeof(PackKernelEntry);
constexpr size_t kCbufs = sizeof(ComputeRecord::cbufType);

class Compiled {
public:
    explicit Compiled(const DirectNvk &nvk) : nvk_(nvk)
    {}

    ~Compiled()
    {
        if (stage.owner)
            nvk::nakStageFinish(nvk_, stage);
    }

    NvkStage stage = {};

private:
    const DirectNvk &nvk_;
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

std::vector<uint32_t> spillOf(const uint32_t *words, uint32_t count)
{
    return count ? std::vector<uint32_t>(words, words + count) : std::vector<uint32_t>();
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
    Entries(const NmbDirectKernel &k, ComputeRecord &out) : k_(k), set0_(k.set0Bindings, k.set0BindingCount), out_(out)
    {}

    bool add(const NmbDirectKernelWrite &kw, std::string &why)
    {
        const NmbDirectWrite &w = kw.write;
        SetSlot b;
        if (w.set == 1 && k_.heapArrays)
            return true;  // a heap array's element: the device's own (raw_heap_tables.h)
        if (w.set != 0) {
            why = "a set-" + std::to_string(w.set) + " descriptor (binding " + std::to_string(w.binding) + ")";
            return false;
        }
        if (!set0_.slot(w.binding, b)) {
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
    const Set0 set0_;
    ComputeRecord &out_;

    bool buffer(const NmbDirectWrite &w, uint16_t offset, std::string &why)
    {
        if (w.source == NmbDirectTextureTable || w.source == NmbDirectSamplerTable) {
            entries.push_back(
                {offset, w.source == NmbDirectTextureTable ? KernelTextureTable : KernelSamplerTable, 0, 0});
            return true;
        }
        if (w.source == NmbDirectArgumentTable) {
            entries.push_back({offset, KernelSpillTable, 0, 0});
            return true;
        }
        if (w.source != NmbDirectBuffer && w.source != NmbDirectBytes) {
            why = "a set-0 buffer m2v binds itself at " + std::to_string(offset) + " (source " +
                  std::to_string(w.source) + ")";
            return false;
        }
        const uint32_t size = bufferDescriptorBytes(w.type, w.range);
        entries.push_back({offset, KernelBuffer, uint8_t(w.slot), size < kTagBytes ? size : 0});
        return true;
    }

    bool texture(const NmbDirectKernelWrite &kw, uint16_t offset, std::string &why)
    {
        const NmbDirectWrite &w = kw.write;
        const bool slotted =
            w.source == NmbDirectTexture || (w.source == NmbDirectCombined && w.slot != NMB_DIRECT_NEUTRAL);
        if (w.viewType >= 0 &&
            (w.source == NmbDirectNeutral || (w.source == NmbDirectCombined && w.slot == NMB_DIRECT_NEUTRAL)))
            return true;  // Set words stay zero: Device reserves TIC 0 for NIL's null image descriptor.
        uint32_t word0 = 0;
        if (!slotted || !kw.viewFormat) {
            why = "a texture slot the dispatch leaves unbound at " + std::to_string(offset);
            return false;
        }
        if (!viewWord0(kw.viewFormat, kw.components, w.type == StorageImage, word0)) {
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
        const bool known = w.sampler == NMB_DIRECT_IMMUTABLE
                               ? immutableSamplerHeader(k_.set0Bindings, k_.set0BindingCount, w.binding, header.data())
                               : kw.hasSampler && samplerHeader(kw.samplerState, header.data());
        if (!known) {
            why = "a sampler slot the dispatch leaves unbound at " + std::to_string(offset);
            return false;
        }
        entries.push_back({offset, KernelSampler, samplerSlot(out_, header), 0});
        return true;
    }
};

// Each buffer entry's residual word as m2v's plan places it (NmbDirectKernel residualWord), where binding at an offset
// keeps the plain dispatch's variant, region and layout
void attachResiduals(const NmbDirectKernel &k, std::vector<PackKernelEntry> &entries)
{
    if (!k.boundSame)
        return;
    for (PackKernelEntry &e : entries)
        if (e.kind == KernelBuffer && e.slot < 32 && k.residualWord[e.slot] <= kRootPushWords + kSpillResidualWords)
            e.window |= uint32_t(k.residualWord[e.slot]) << 24;
}

CompileOutcome compile(const NmbDirectKernel &k, const DirectNvk &nvk, Compiled &made, std::string &why)
{
    SubgroupSize subgroup;
    subgroup.size = k.stage.subgroupSize;
    const NvkStageInfo info = {k.stage.spirv,
                               k.stage.spirvBytes,
                               k.stage.entry,
                               k.stage.stage,
                               k.stage.specialization,
                               nullptr,
                               0,
                               nullptr,
                               k.stage.subgroupSize ? &subgroup : nullptr,
                               false,
                               false,
                               0,
                               0,
                               {},
                               0,
                               k.stage.semantic,
                               k.stage.portableContext,
                               k.stage.portable};
    if (k.stage.stage != kComputeStage) {
        why = "a stage of VkShaderStageFlags " + std::to_string(k.stage.stage);
        return CompileOutcome::Refused;
    }
    const NakLayoutData layout(k.layoutData);
    if (!layout.valid) {
        why = "a descriptor type NVK lays out by another rule";
        return CompileOutcome::Refused;
    }
    if (const int32_t result = nvk::nakCompileStage(nvk, nvk.nakTarget, info, layout.layout, made.stage); result != 0) {
        why = "NVK refused the kernel (VkResult " + std::to_string(result) + ")";
        return CompileOutcome::Failed;
    }
    if (made.stage.cbufCount > kCbufs) {
        why = std::to_string(made.stage.cbufCount) + " constant buffers";
        return CompileOutcome::Refused;
    }
    return CompileOutcome::Made;
}

// The kernel's reads through argument buffers a dispatch takes of the device (raw_heap_tables.h): its address words, the
// buffer slots its root table holds, its set-1 heap arrays in NVK's layout (heapSetArrays, as a draw's)
bool kernelArguments(const NmbDirectKernel &k, PackKernel &r, std::string &why)
{
    r.rootTableWord = k.rootTableWord;
    r.statusWord = k.statusWord;
    r.rootSlots = k.rootSlots;
    if (r.rootSlots && !r.rootTableWord) {
        why = "a root table's buffer slots in a kernel that reads no root table";
        return false;
    }
    if (!k.heapArrays)
        return true;
    if (const char *refused = heapSetArrays(k.layoutData, k.heapArrays, k.heapClass, r.heap, r.heapBytes)) {
        why = refused;
        return false;
    }
    r.heapArrays = uint8_t(k.heapArrays);
    return true;
}

// What sends a kernel's dispatch to no record
bool recordable(const NmbDirectKernel &k, std::string &why)
{
    if (k.otherHoles)
        why =
            std::to_string(k.otherHoles) + " push words m2v fills with addresses other than its root and status tables";
    else if (k.regions != 1)
        why = std::to_string(k.regions) + " Vulkan dispatches for one";
    else if (!k.movedSame ||
             pushWords(k.pushMask, k.push, k.regionWord) != pushWords(k.movedMask, k.moved, k.regionWord) ||
             spillOf(k.spill, k.spillWords) != spillOf(k.movedSpill, k.movedSpillWords))
        why = "a program or push word the bound addresses or samplers decide";
    else if (k.spillWords > kSpillWordsMax)
        why = "an argument table past 255 words";
    else
        return true;
    return false;
}

}  // namespace

CompileOutcome directKernel(const NmbDirectKernel &k, const DirectNvk &nvk, const std::string &key, ComputeRecord &out,
                            std::string &why)
{
    out = {};
    if (!recordable(k, why))
        return CompileOutcome::Refused;
    Compiled made(nvk);
    if (const CompileOutcome compiled = compile(k, nvk, made, why); compiled != CompileOutcome::Made)
        return compiled;
    Entries entries(k, out);
    for (uint32_t i = 0; i < k.writeCount; i++)
        if (!entries.add(k.writes[i], why))
            return CompileOutcome::Refused;
    std::stable_sort(entries.entries.begin(), entries.entries.end(),
                     [](const PackKernelEntry &a, const PackKernelEntry &b) {
                         return a.offset < b.offset;
                     });
    if (entries.entries.size() > kEntries) {
        why = std::to_string(entries.entries.size()) + " set-0 entries";
        return CompileOutcome::Refused;
    }
    attachResiduals(k, entries.entries);
    PackKernel &r = out.kernel;
    r.setBytes = uint16_t(Set0(k.set0Bindings, k.set0BindingCount).bytes());
    if (!kernelArguments(k, r, why))
        return CompileOutcome::Refused;
    if (!computeQmd(made.stage, r.setBytes, r.heapBytes, r.qmd)) {
        why = "a kernel reading a constant buffer the raw path does not bind";
        return CompileOutcome::Refused;
    }
    for (uint32_t c = 0; c < made.stage.cbufCount && c < 16; c++)
        if (cbufKind(made.stage.cbufTypes[c]) == CbufSet && made.stage.cbufTypes[c] != CbufSet &&
            !(made.stage.cbufTypes[c] == kCbufHeapSet && r.heapArrays)) {
            why = "a kernel reading a descriptor set past set 0";
            return CompileOutcome::Refused;
        }
    clearDispatchWords(r.qmd);
    r.keyHash = computeKeyHash(key);
    r.regionWord = int8_t(k.regionWord);
    if (k.stageInRegionWord + 8 > 32) {
        why = "a stage-in region past the root push window";
        return CompileOutcome::Refused;
    }
    r.stageInRegionWord = uint8_t(k.stageInRegionWord + 1);
    for (size_t i = 0; i < 4; i++) {
        r.stageInStride[i][0] = uint8_t(k.stageInStrides[i][0]);
        r.stageInStride[i][1] = uint8_t(k.stageInStrides[i][1]);
    }
    r.entries = uint8_t(entries.entries.size());
    std::copy(entries.entries.begin(), entries.entries.end(), r.entry);
    for (uint32_t i = 0; i < r.entries; i++)
        if (r.entry[i].kind == KernelSpillTable)
            r.spillEntry = uint8_t(i + 1);
    if (bool(r.spillEntry) != bool(k.spillWords)) {
        why = "an argument table its layout and its words disagree on";
        return CompileOutcome::Refused;
    }
    r.spillWords = uint8_t(k.spillWords);
    out.spill = spillOf(k.spill, k.spillWords);
    for (size_t i = 0; i < entries.entries.size(); ++i) {
        const auto &entry = entries.entries[i];
        if (entry.slot >= 32)
            continue;
        const uint32_t bit = 1u << entry.slot;
        if ((entry.kind == KernelBuffer && (k.bufferReads & bit) && !(k.bufferWrites & bit)) ||
            (entry.kind == KernelTexture && (k.textureReads & bit) && !(k.textureWrites & bit)))
            r.readOnlyEntries |= 1ull << i;
    }
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
    return CompileOutcome::Made;
}

}  // namespace raw

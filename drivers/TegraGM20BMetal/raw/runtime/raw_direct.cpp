// SPDX-License-Identifier: MIT
// See raw_direct.h. What the probe's capture reads of NVK's commands (raw_record.cpp) is read here from what made them: m2v's plan
// of each probe draw and NVK's own rules for the layout and the descriptors.
#include "raw_direct.h"
#include "raw_direct_nvk.h"
#include "raw_direct_push.h"
#include "raw_residual.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include "bridge/NativeMetalDirect.h"
#include "raw_emit.h"
#include "raw_record_parts.h"

namespace raw {
namespace {

// nvk_switch_split.h switch_nvk_split_layout, nvk_switch_compile.h switch_nvk_split_parts
struct NvkSplitLayout {
    uint32_t frontierBytes;
    uint8_t frontierCbuf, prologueCbufs, shaderDataCbuf, uboCount;
    uint8_t uboCbuf[6];
    uint32_t frontierValues, prologueInstrs;
};

struct NvkSplitParts {
    NvkStage fs, prologue;
    NvkSplitLayout layout;
    uint32_t qmd[64];
    uint32_t thresholdPixels;
};

using SplitParts = void (*)(const void *split, NvkSplitParts *);
using CompileAbi = void (*)(uint32_t sizes[3]);

// VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR: m2v's states are made of pipeline libraries
constexpr uint64_t kLibraryBit = 0x800;
constexpr uint32_t kFragmentBit = 0x10;  // VK_SHADER_STAGE_FRAGMENT_BIT
// pack/splits.py lay_out's limits: the constant buffers a split per-pixel program binds, and a QMD's
constexpr uint32_t kSplitProgramCbufs = 11, kQmdCbufs = 8;
constexpr uint32_t kRootPush = 0x700;
constexpr int32_t kViewType3D = 2;  // VK_IMAGE_VIEW_TYPE_3D
constexpr uint32_t kTagged = 16;    // probe.mm's fragment textures and samplers

std::string hex(uint64_t v)
{
    char b[24];
    snprintf(b, sizeof b, "%llx", static_cast<unsigned long long>(v));
    return b;
}

uint64_t fnv(const void *p, size_t n, uint64_t h = 0xcbf29ce484222325ull)
{
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

// A compiled stage as the capture names a program (nvdec's: the FNV-1a of its image, then its data)
CapturedProgram programOf(const NvkStage &s)
{
    CapturedProgram p;
    p.image.assign(s.header, s.header + sizeof s.header);
    const auto *code = static_cast<const uint8_t *>(s.code), *data = static_cast<const uint8_t *>(s.data);
    p.image.insert(p.image.end(), code, code + s.codeBytes);
    p.data.assign(data, data + s.dataBytes);
    p.cbufTypes.assign(s.cbufTypes, s.cbufTypes + std::min<uint32_t>(s.cbufCount, 16));
    p.id = hex(fnv(p.data.data(), p.data.size(), fnv(p.image.data(), p.image.size())));
    return p;
}

// The written colour attachments of a 16-bit float format the key names ("c<j>=<MTLPixelFormat>:<writeMask>:"): R16Float,
// RG16Float, RGBA16Float
uint32_t halfFloatTargets(const std::string &key)
{
    const std::string fields = key.substr(0, key.find(" | "));
    uint32_t out = 0;
    for (size_t at = fields.find(" c"); at != std::string::npos; at = fields.find(" c", at + 2)) {
        unsigned j, format, mask;
        if (sscanf(fields.c_str() + at, " c%u=%u:%u:", &j, &format, &mask) == 3 && j < 8 && mask &&
            (format == 25 || format == 65 || format == 115))
            out |= 1u << j;
    }
    return out;
}

class Stages {
public:
    Stages(const DirectNvk &nvk, void *device) : nvk_(nvk), device_(device)
    {}

    ~Stages()
    {
        for (NvkStage &s : made_)
            if (s.owner)
                nvk::stageFinish(nvk_, device_, s);
    }

    // Refused: a state the raw path does not draw from a record; Failed: NVK could not compile a stage. `split`: the fragment
    // stage's split too (split())
    Compiler::Outcome compile(const NmbDirectState &state, uint32_t halfTargets, bool split, std::string &why)
    {
        if (state.stageCount != 2) {
            why = std::to_string(state.stageCount) + " stages";
            return Compiler::Outcome::Refused;
        }
        for (uint32_t k = 0; k < 2; k++) {
            const NmbDirectStage &s = state.stages[k];
            if (s.flags) {
                why = "a stage made with flags " + hex(s.flags);
                return Compiler::Outcome::Refused;
            }
            SubgroupSize subgroup;
            subgroup.size = s.subgroupSize;
            NvkStageInfo info = {s.spirv,
                                 s.spirvBytes,
                                 s.entry,
                                 s.stage,
                                 s.specialization,
                                 handle(state.layout),
                                 kLibraryBit,
                                 nullptr,
                                 s.subgroupSize ? &subgroup : nullptr,
                                 state.sampleShading,
                                 split && s.stage == kFragmentBit,
                                 halfTargets,
                                 constantPushWords(state),
                                 {}};
            memcpy(info.constPushWords, state.draws[0].push, sizeof info.constPushWords);
            const int32_t result = nvk::compileStage(nvk_, device_, info, made_[k]);
            if (result != 0) {
                why = "NVK refused stage " + std::to_string(k) + " (VkResult " + std::to_string(result) + ")";
                return Compiler::Outcome::Failed;
            }
            programs_[k] = programOf(made_[k]);
        }
        return Compiler::Outcome::Made;
    }

    const CapturedProgram &program(int k) const
    {
        return programs_[k];
    }

    EmitProgram emit(int k) const
    {
        return {made_[k].header, made_[k].numGprs, made_[k].earlyFragmentTests};
    }

    const void *split(int k) const
    {
        return made_[k].split;
    }

private:
    const DirectNvk &nvk_;
    void *device_;
    NvkStage made_[2] = {};
    CapturedProgram programs_[2];
};

// The set-0 byte of `w` in the draw's layout
bool offsetOf(const DirectNvk &nvk, const NmbDirectDraw &d, const NmbDirectWrite &w, uint32_t &offset)
{
    NvkSetBinding b = {};
    if (!d.set0 || !nvk::setLayoutBinding(nvk, d.set0, w.binding, b))
        return false;
    offset = b.offset + w.element * b.stride;
    return true;
}

// raw_record.cpp layout(): the entries of draw 0's set 0, read from m2v's writes where the probe reads NVK's words
bool layout(const DirectNvk &nvk, void *device, const NmbDirectDraw &d, std::vector<LayoutEntry> &out, std::string &why)
{
    for (uint32_t i = 0; i < d.writeCount; i++) {
        const NmbDirectWrite &w = d.writes[i];
        if (w.set != 0)
            continue;
        LayoutEntry e;
        if (!offsetOf(nvk, d, w, e.offset)) {
            why = "set 0 binding " + std::to_string(w.binding) + " not in its layout";
            return false;
        }
        if (w.type == StorageBuffer && w.source == NmbDirectColorTable) {
            e.colorTable = true;
            out.push_back(e);
            continue;
        }
        if (bufferType(w.type)) {
            if ((w.source != NmbDirectBuffer && w.source != NmbDirectBytes) || (w.stage != 1 && w.stage != 2)) {
                why = "a set-0 buffer m2v binds itself at " + std::to_string(e.offset) + " (source " +
                      std::to_string(w.source) + ")";
                return false;
            }
            e.buffer = true;
            e.stage = uint8_t(w.stage);
            e.slot = uint8_t(w.slot);
            out.push_back(e);
            continue;
        }
        // without the interlock m2v reads such an attachment as a storage image, which the probe saw as a fetch of the target;
        // a storage image's word names its texture's header as a sampled image's does
        const bool attachment = w.source == NmbDirectAttachment || w.source == NmbDirectStorageAttachment;
        if (w.type != Sampler && w.type != CombinedImageSampler && w.type != SampledImage &&
            w.type != InputAttachment && !(w.type == StorageImage && (attachment || w.source == NmbDirectTexture))) {
            why = "a set-0 descriptor of type " + std::to_string(w.type) + " (source " + std::to_string(w.source) +
                  ", slot " + std::to_string(w.slot) + ")";
            return false;
        }
        if (w.type != Sampler) {
            if (attachment) {
                e.tex = Tex::Fetch;
                e.texSlot = uint8_t(w.slot);
            } else if ((w.source == NmbDirectTexture || w.source == NmbDirectCombined) && w.stage == 2 &&
                       w.slot < kTagged) {
                // the direct compile binds a 3D texture at each texture3d argument (NativeMetalDirect.rs volume_slots)
                e.tex = w.viewType == kViewType3D ? Tex::Texture3D : Tex::Slot;
                e.texSlot = uint8_t(w.slot);
            } else {
                e.tex = Tex::Unknown;
            }
        }
        if (w.type == Sampler || w.type == CombinedImageSampler) {
            if (w.sampler == NMB_DIRECT_IMMUTABLE) {
                e.smp = Smp::Constexpr;
                e.headerKnown = nvk::immutableSamplerHeader(nvk, device, d.set0, w.binding, w.element, e.header.data());
            } else if (w.sampler < kTagged) {
                e.smp = Smp::Slot;
                e.smpSlot = uint8_t(w.sampler);
            } else {
                why = "a neutral sampler at " + std::to_string(e.offset);
                return false;
            }
        }
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const LayoutEntry &a, const LayoutEntry &b) {
        return a.offset < b.offset;
    });
    return true;
}

// raw_record.cpp boundSizes: by set-0 byte, the size word NVK writes in a buffer descriptor of the draw
std::map<uint32_t, uint32_t> boundSizes(const DirectNvk &nvk, void *device, const NmbDirectDraw &d)
{
    std::map<uint32_t, uint32_t> out;
    for (uint32_t i = 0; i < d.writeCount; i++) {
        const NmbDirectWrite &w = d.writes[i];
        uint32_t offset = 0;
        if (w.set == 0 && bufferType(w.type) && offsetOf(nvk, d, w, offset))
            out[offset] = nvk::bufferDescriptorSize(nvk, device, w.type, w.range);
    }
    return out;
}

// raw_record.cpp residualWords: the root push words the bind-offset draw writes where draw 0 left 0, by their value's low byte
std::map<uint32_t, uint32_t> residualWords(const NmbDirectDraw &plain, const NmbDirectDraw &bound)
{
    std::map<uint32_t, uint32_t> was, now;
    for (uint32_t i = 0; i < 32; i++) {
        if (plain.pushMask >> i & 1)
            was[kRootPush + 4 * i] = plain.push[i];
        if (bound.pushMask >> i & 1)
            now[kRootPush + 4 * i] = bound.push[i];
    }
    return residualWordsByValue(was, now);
}

// raw_emit's registers over the pack's baseline and context. One the hardware does not read keeps what pack states leave there
// unless the pack varies it, so it never costs an extra.
bool emitted(const std::string &key, const Stages &stages, const std::vector<uint32_t> &attributes, uint32_t blended,
             const PackRules &rules, std::map<uint32_t, uint32_t> &regs, std::string &why)
{
    std::string fields = key.substr(0, key.find(" | "));
    while (!fields.empty() && fields.back() == ' ')
        fields.pop_back();
    std::vector<EmitRegister> made;
    if (!emitState(fields, stages.emit(0), stages.emit(1), made, why, &blended) ||
        !emitVertexInput(fields, attributes, made, why))
        return false;
    regs = rules.context;
    for (const auto &[m, v] : rules.baseline)
        regs[m] = v;
    for (const EmitRegister &r : made)
        if (r.read || rules.varying.count(r.method))
            regs[r.method] = r.value;
    return true;
}

// The fragment program's split as a record holds it (NVK made none where it saves nothing)
void splitInto(const DirectNvk &nvk, const void *split, RuntimeRecord &out)
{
    if (!split)
        return;
    NvkSplitParts parts;
    reinterpret_cast<SplitParts>(nvk.splitParts)(split, &parts);
    const NvkSplitLayout &l = parts.layout;
    if (parts.fs.cbufCount > kSplitProgramCbufs || l.prologueCbufs > kQmdCbufs || l.uboCount > sizeof l.uboCbuf)
        return;
    PackSplit &r = out.splitRecord;
    r.thresholdPixels = parts.thresholdPixels;
    r.frontierBytes = uint16_t(l.frontierBytes);
    r.frontierCbuf = l.frontierCbuf;
    r.uboCount = l.uboCount;
    r.shaderDataCbuf = l.shaderDataCbuf;
    r.prologueCbufs = l.prologueCbufs;
    memcpy(r.uboCbuf, l.uboCbuf, l.uboCount);
    memcpy(r.qmd, parts.qmd, sizeof r.qmd);
    out.splitPrograms[0] = programOf(parts.fs);
    out.splitPrograms[1] = programOf(parts.prologue);
    out.split = true;
}

}  // namespace

DirectNvk DirectNvk::find(void *(*symbol)(const char *name))
{
    DirectNvk nvk;
    nvk.compileStage = symbol("switch_nvk_compile_stage");
    nvk.stageFinish = symbol("switch_nvk_stage_finish");
    nvk.setLayoutBinding = symbol("switch_nvk_set_layout_binding");
    nvk.setLayoutSize = symbol("switch_nvk_set_layout_size");
    nvk.immutableSamplerHeader = symbol("switch_nvk_immutable_sampler_header");
    nvk.bufferDescriptorSize = symbol("switch_nvk_buffer_descriptor_size");
    nvk.samplerHeader = symbol("switch_nvk_sampler_header");
    nvk.computeQmd = symbol("switch_nvk_compute_qmd");
    nvk.viewHeaderWord0 = symbol("switch_nvk_view_header_word0");

    nvk.splitParts = symbol("switch_nvk_split_parts");
    if (const auto abi = reinterpret_cast<CompileAbi>(symbol("switch_nvk_compile_abi"))) {
        uint32_t sizes[3] = {};
        abi(sizes);
        nvk.abiMatches =
            sizes[0] == sizeof(NvkStageInfo) && sizes[1] == sizeof(NvkStage) && sizes[2] == sizeof(NvkSplitParts);
    }
    return nvk;
}

bool DirectNvk::complete() const
{
    return compileStage && stageFinish && setLayoutBinding && setLayoutSize && immutableSamplerHeader &&
           bufferDescriptorSize && splitParts && abiMatches;
}

bool DirectNvk::kernelComplete() const
{
    return complete() && samplerHeader && computeQmd && viewHeaderWord0;
}

Compiler::Outcome directRecord(const NmbDirectState &state, const DirectNvk &nvk, const PackRules &rules,
                               const std::string &key, const std::vector<uint32_t> &attributes, RuntimeRecord &out,
                               std::string &why, std::map<uint32_t, uint32_t> *registers, bool split)
{
    out = {};
    void *device = handle(state.device);
    const NmbDirectDraw &draw = state.draws[0];
    if (state.storageForm) {
        why = "storage form (its barrier form needs NVK's interlock hidden)";
        return Compiler::Outcome::Refused;
    }
    if (draw.pushHoles) {
        why = std::to_string(draw.pushHoles) + " push words m2v fills with addresses";
        return Compiler::Outcome::Refused;
    }
    Stages stages(nvk, device);
    if (const Compiler::Outcome compiled = stages.compile(state, halfFloatTargets(key), split, why);
        compiled != Compiler::Outcome::Made)
        return compiled;
    RecordParts parts;
    parts.programs[0] = &stages.program(0);
    parts.programs[1] = &stages.program(1);
    if (!programsBindable(parts, why) || !layout(nvk, device, draw, parts.layout, why))
        return Compiler::Outcome::Refused;
    parts.sizes16 = boundSizes(nvk, device, state.draws[1]);
    parts.sizes4096 = boundSizes(nvk, device, state.draws[2]);
    parts.residuals = residualWords(draw, state.draws[3]);
    if (draw.vertexSlot != UINT32_MAX) {
        parts.vertexSlot = uint8_t(draw.vertexSlot);
        parts.vertexStride = uint16_t(draw.vertexStride & 0xFFF);
    }
    if (!emitted(key, stages, attributes, state.blended, rules, parts.regs, why))
        return Compiler::Outcome::Refused;
    for (uint32_t i = 0; i < 32; i++)
        if (draw.pushMask >> i & 1)
            parts.push[kRootPush + 4 * i] = draw.push[i];
    parts.setBytes = draw.set0 ? uint16_t(nvk::setLayoutSize(nvk, device, draw.set0)) : 0;
    if (!assembleRecord(parts, key, rules, out, why))
        return Compiler::Outcome::Refused;
    if (registers)
        *registers = parts.regs;
    out.splitTried = split;
    if (split)
        splitInto(nvk, stages.split(1), out);
    return Compiler::Outcome::Made;
}

}  // namespace raw

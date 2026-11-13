// SPDX-License-Identifier: MIT
// See raw_direct.h. What the probe's capture reads of NVK's commands (raw_record.cpp) is read here from what made them: m2v's plan
// of each probe draw and NVK's rules for the layout and the descriptors (raw_direct_encode.h).
#include "raw_direct.h"
#include "raw_direct_heap.h"
#include "raw_direct_nvk.h"
#include "raw_direct_push.h"
#include "raw_chip_facts.h"
#include "raw_direct_storage.h"
#include "raw_direct_encode.h"
#include "raw_nak_target.h"
#include "raw_rotation.h"
#include "raw_spill_table.h"
#include "raw_split_metadata.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
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
    uint16_t batchVersion, batchThreads;
    uint32_t frontierValues, prologueInstrs;
};

struct NvkSplitParts {
    NvkStage fs, prologue;
    NvkSplitLayout layout;
    uint32_t qmd[64];
    uint32_t thresholdPixels;
    SplitCost cost;
};

using SplitParts = void (*)(const void *split, NvkSplitParts *);
using CompileAbi = void (*)(uint32_t sizes[3]);

// VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR: m2v's states are made of pipeline libraries
constexpr uint64_t kLibraryBit = 0x800;
constexpr uint32_t kFragmentBit = 0x10;  // VK_SHADER_STAGE_FRAGMENT_BIT
// pack/splits.py lay_out's limits: the constant buffers a split per-pixel program binds, and a QMD's
constexpr uint32_t kSplitProgramCbufs = sizeof(PackProgram::cbufType), kQmdCbufs = 8;
constexpr uint32_t kRootPush = 0x700;
constexpr int32_t kViewType3D = 2;  // VK_IMAGE_VIEW_TYPE_3D

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
    p.gprs = uint8_t(s.numGprs);
    p.boundTextures = s.boundTextures;
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
    Stages(const DirectNvk &nvk, const NakLayoutData &layout, uint64_t xrPlaneBindings = 0)
        : nvk_(nvk), layout_(layout), xrPlaneBindings_(xrPlaneBindings)
    {}

    ~Stages()
    {
        for (NvkStage &s : made_)
            if (s.owner)
                nvk::nakStageFinish(nvk_, s);
    }

    // Refused: a state the raw path does not draw from a record; Failed: NVK could not compile a stage. `split`: the fragment
    // stage's split too (split()). `zeroPush`: root push words compiled as the constant 0 besides the state's constant ones.
    // `turned`: the fragment stage alone, as a turned plane's (NVK's nvk_switch_turn.h)
    CompileOutcome compile(const NmbDirectState &state, uint32_t halfTargets, bool split, std::string &why,
                           uint32_t zeroPush = 0, bool texsF16 = false, bool turned = false)
    {
        if (state.stageCount != 2 && !(state.stageCount == 4 && state.tess.parameters && state.tess.controlWord)) {
            why = std::to_string(state.stageCount) + " stages";
            return CompileOutcome::Refused;
        }
        const uint32_t first = turned ? 1 : 0, end = turned ? 2 : state.stageCount;
        for (uint32_t k = first; k < end; k++) {
            const NmbDirectStage &s = state.stages[k];
            if (s.flags) {
                why = "a stage made with flags " + hex(s.flags);
                return CompileOutcome::Refused;
            }
            SubgroupSize subgroup;
            subgroup.size = s.subgroupSize;
            NvkStageInfo info = {
                s.spirv,
                s.spirvBytes,
                s.entry,
                s.stage,
                s.specialization,
                nullptr,
                kLibraryBit,
                nullptr,
                s.subgroupSize ? &subgroup : nullptr,
                state.sampleShading,
                split && s.stage == kFragmentBit,
                halfTargets,
                constantPushWords(state),
                {},
                uint64_t(s.stage == kFragmentBit && state.storage.depthFormat && state.storage.samples > 1),
                nullptr,
                nullptr,
                nullptr};
            memcpy(info.constPushWords, state.draws[0].push, sizeof info.constPushWords);
            info.texsF16 = texsF16 && s.stage == kFragmentBit;
            info.turned = turned;
            info.xrPlaneBindings = s.stage == kFragmentBit ? xrPlaneBindings_ : 0;
            info.constPushMask |= zeroPush;
            for (uint32_t i = 0; i < 32; i++)
                if (zeroPush >> i & 1)
                    info.constPushWords[i] = 0;
            const int32_t result = nvk::nakCompileStage(nvk_, nvk_.nakTarget, info, layout_.layout, made_[k]);
            if (result != 0) {
                why = "NVK refused stage " + std::to_string(k) + " (VkResult " + std::to_string(result) + ")";
                return CompileOutcome::Failed;
            }
            programs_[k] = programOf(made_[k]);
        }
        return CompileOutcome::Made;
    }

    const CapturedProgram &program(int k) const
    {
        return programs_[k];
    }

    bool tessellates() const
    {
        return made_[3].owner != nullptr;
    }

    EmitProgram emit(int k) const
    {
        return {made_[k].header, made_[k].numGprs, made_[k].earlyFragmentTests, made_[k].interlockControl,
                bool(made_[k].usesSampleShading)};
    }

    const void *split(int k) const
    {
        return made_[k].split;
    }

private:
    const DirectNvk &nvk_;
    const NakLayoutData &layout_;
    uint64_t xrPlaneBindings_;
    NvkStage made_[4] = {};
    CapturedProgram programs_[4];
};

// The set-0 bindings of draw 0's writes of fragment texture slots `slots` (nvk_switch_xr_planes.h), 0 where one has none
uint64_t xrPlaneBindings(const NmbDirectDraw &d, uint16_t slots)
{
    uint64_t bindings = 0;
    uint16_t found = 0;
    for (uint32_t i = 0; i < d.writeCount; i++) {
        const NmbDirectWrite &w = d.writes[i];
        if (w.set == 0 && w.stage == 2 && w.slot < 16 && (slots >> w.slot & 1) && w.binding < 64 &&
            (w.source == NmbDirectTexture || w.source == NmbDirectCombined)) {
            bindings |= uint64_t(1) << w.binding;
            found |= uint16_t(1u << w.slot);
        }
    }
    return found == slots ? bindings : 0;
}

// The set-0 byte of `w` in the draw's layout
bool offsetOf(const Set0 &set0, const NmbDirectWrite &w, uint32_t &offset)
{
    SetSlot b;
    if (!set0.slot(w.binding, b))
        return false;
    offset = b.offset + w.element * b.stride;
    return true;
}

// raw_record.cpp layout(): the entries of draw 0's set 0, read from m2v's writes where the probe reads NVK's words
bool layout(const NmbDirectDraw &d, std::vector<LayoutEntry> &out, std::string &why)
{
    const Set0 set0(d.set0Bindings, d.set0BindingCount);
    for (uint32_t i = 0; i < d.writeCount; i++) {
        const NmbDirectWrite &w = d.writes[i];
        if (w.set != 0)
            continue;
        LayoutEntry e;
        if (!offsetOf(set0, w, e.offset)) {
            why = "set 0 binding " + std::to_string(w.binding) + " not in its layout";
            return false;
        }
        if (w.source == NmbDirectTessFactors || w.source == NmbDirectPatchIndices ||
            w.source == NmbDirectControlPointIndices) {
            e.tessBuffer = int8_t(w.source == NmbDirectTessFactors    ? TessFactors
                                  : w.source == NmbDirectPatchIndices ? TessPatchIndices
                                                                      : TessControlPointIndices);
            out.push_back(e);
            continue;
        }
        if (w.type == StorageBuffer && w.source == NmbDirectColorTable) {
            e.colorTable = true;
            out.push_back(e);
            continue;
        }
        if (w.type == StorageBuffer && (w.source == NmbDirectTextureTable || w.source == NmbDirectSamplerTable)) {
            e.table = w.source == NmbDirectTextureTable ? EntryTextureTable : EntrySamplerTable;
            e.stage = uint8_t(w.stage);
            out.push_back(e);
            continue;
        }
        if (w.source == NmbDirectArgumentTable) {
            if (w.stage != 1 && w.stage != 2) {
                why = "an argument table of stage " + std::to_string(w.stage);
                return false;
            }
            e.table = EntrySpillTable;
            e.stage = uint8_t(w.stage);
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
        const bool testImage = w.source == NmbDirectDepthStandIn || w.source == NmbDirectStencilStandIn;
        if (w.type != Sampler && w.type != CombinedImageSampler && w.type != SampledImage &&
            w.type != InputAttachment &&
            !(w.type == StorageImage && (attachment || testImage || w.source == NmbDirectTexture))) {
            why = "a set-0 descriptor of type " + std::to_string(w.type) + " (source " + std::to_string(w.source) +
                  ", slot " + std::to_string(w.slot) + ")";
            return false;
        }
        e.stage = uint8_t(w.stage == 1 ? 1 : 2);
        if (w.type != Sampler) {
            if (testImage) {
                e.tex = w.source == NmbDirectDepthStandIn ? Tex::DepthTest : Tex::StencilTest;
            } else if (attachment) {
                e.tex = Tex::Fetch;
                e.texSlot = uint8_t(w.slot);
            } else if ((w.source == NmbDirectTexture || w.source == NmbDirectCombined) &&
                       (w.stage == 1 || w.stage == 2)) {
                if (w.slot >= kStageTextures) {
                    why = std::string(w.stage == 1 ? "a vertex" : "a fragment") + " texture at slot " +
                          std::to_string(w.slot) + ", past the raw encoder's " + std::to_string(kStageTextures);
                    return false;
                }
                // the direct compile binds a texture of each argument's declared type (NativeMetalDirect.rs declared_slots)
                e.tex = w.viewType == kViewType3D ? Tex::Texture3D : Tex::Slot;
                e.texSlot = uint8_t(w.slot);
                if (w.type == StorageImage && w.stage == 2) {
                    if (!w.format) {
                        why = "a storage texture at fragment slot " + std::to_string(w.slot) + " of no format";
                        return false;
                    }
                    e.writtenFormat = uint16_t(w.format);
                }
            } else {
                e.tex = Tex::Unknown;
            }
        }
        if (w.type == Sampler || w.type == CombinedImageSampler) {
            if (w.sampler == NMB_DIRECT_IMMUTABLE) {
                e.smp = Smp::Constexpr;
                e.headerKnown = immutableSamplerHeader(d.set0Bindings, d.set0BindingCount, w.binding, e.header.data());
            } else if (w.sampler < kStageSamplers) {
                e.smp = Smp::Slot;
                e.smpSlot = uint8_t(w.sampler);
                e.smpCompare = w.compare != 0;
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
std::map<uint32_t, uint32_t> boundSizes(const NmbDirectDraw &d)
{
    const Set0 set0(d.set0Bindings, d.set0BindingCount);
    std::map<uint32_t, uint32_t> out;
    for (uint32_t i = 0; i < d.writeCount; i++) {
        const NmbDirectWrite &w = d.writes[i];
        uint32_t offset = 0;
        if (w.set == 0 && bufferType(w.type) && offsetOf(set0, w, offset))
            out[offset] = bufferDescriptorBytes(w.type, w.range);
    }
    return out;
}

// What the record takes of the draws' argument tables (m2v's push fields past each stage's window): draw 0's words, and
// each bind offset residual's word as the plan places it, in the push window or the table; null: why not
const char *spillTables(const NmbDirectState &state, RecordParts &parts)
{
    const NmbDirectDraw &draw = state.draws[0];
    for (uint32_t k = 0; k < 2; k++) {
        for (const NmbDirectDraw &d : state.draws)
            if (d.spillWords[k] != draw.spillWords[k])
                return "probe draws on variants of other argument tables";
        if (draw.spillWords[k] > kSpillWordsMax)
            return "an argument table past 255 words";
        parts.spill[k].assign(draw.spill[k], draw.spill[k] + draw.spillWords[k]);
        if (memcmp(draw.residualWord[k], state.draws[3].residualWord[k], sizeof draw.residualWord[k]))
            return "probe draws on variants of other push placements";
        for (uint32_t slot = 0; slot < 32; slot++)
            if (draw.residualWord[k][slot] > kRootPushWords + kSpillResidualWords)
                return "a spilled bind offset past word 30 of its argument table";
        memcpy(parts.residualWord[k], draw.residualWord[k], sizeof parts.residualWord[k]);
    }
    for (const uint32_t word : {draw.blendColorWord, draw.halfBlendColorWord, draw.depthTestWord, draw.stencilTestWord})
        if (word > 0xFF)
            return "a dynamic field past word 222 of its argument table";
    return nullptr;
}

// The streams past stream 0 into the record (PackPipeline::vertexStreams). Stream 0 is the bridge's first vertex buffer
// (vertexSlot), which binds at the lowest Metal buffer index as raw_emit numbers the streams
bool vertexStreams(const std::vector<EmitStream> &streams, RecordParts &parts, std::string &why)
{
    if (streams.size() > kPackVertexStreams) {
        why = "more vertex streams than a record's " + std::to_string(kPackVertexStreams);
        return false;
    }
    if (!streams.empty() && streams[0].buffer != parts.vertexSlot) {
        why = "the first vertex stream is not the translation's first vertex buffer";
        return false;
    }
    for (size_t k = 1; k < streams.size(); k++)
        parts.vertexStreams[k - 1] = uint8_t(streams[k].buffer + 1) | (streams[k].bindStride ? kStreamBindStride : 0);
    return true;
}

// raw_emit's registers over the pack's baseline and context. One the hardware does not read keeps what pack states leave there
// unless the pack varies it, so it never costs an extra. Fixed-function input fetches the attributes the translation does not
// pull, at the locations m2v gives them (dense in attribute order). `alphaTargets`: a two-plane state's (emitState's).
bool emitted(const std::string &key, const Stages &stages, const std::vector<uint32_t> &attributes, uint32_t pulled,
             uint32_t blended, bool twoPlane, const PackRules &rules, std::map<uint32_t, uint32_t> &regs,
             uint32_t &alphaTargets, std::vector<EmitStream> &streams, std::string &why)
{
    std::string fields = key.substr(0, key.find(" | "));
    while (!fields.empty() && fields.back() == ' ')
        fields.pop_back();
    std::vector<uint32_t> fetched;
    if (!stages.tessellates())
        std::copy_if(attributes.begin(), attributes.end(), std::back_inserter(fetched), [&](uint32_t index) {
            return index >= 32 || !(pulled >> index & 1);
        });
    std::vector<EmitRegister> made;
    if (!emitState(fields, stages.emit(0), stages.emit(1), made, why, &blended, twoPlane, &alphaTargets) ||
        !emitVertexInput(fields, fetched, made, streams, why))
        return false;
    regs = rules.context;
    for (const auto &[m, v] : rules.baseline)
        regs[m] = v;
    for (const EmitRegister &r : made)
        if (r.read || rules.varying.count(r.method))
            regs[r.method] = r.value;
    return true;
}

// Each stage's program variants, most specialised first (raw_pack.h PackVariant): residual 0, compiled with the root push
// words that hold uniform bind residuals (raw_residual.h) as the constant 0, so a uniform read is a constant-buffer operand
// at an immediate offset; and where the chip is proven to round them so (raw_chip_facts.h), fetches rounded to half in
// TEXS's F16 forms. A stage's variant is kept only where its code differs, and nothing else a record says of it does.
void programVariants(const NmbDirectState &state, const DirectNvk &nvk, const NakLayoutData &layout,
                     uint32_t halfTargets, uint64_t xrPlanes, const Stages &generic, RuntimeRecord &out)
{
    uint32_t residual[2] = {}, zeroPush = 0;
    const PackPipeline &p = out.pipeline;
    for (uint32_t k = 0; k < kUniformRanges; k++) {
        if (p.uniform[k].entry >= p.entries)
            continue;
        const PackEntry &e = p.entry[p.uniform[k].entry];
        const int word = bufferResidualWord(e);
        if (word < 0 || word >= 32)
            continue;
        residual[e.stage == 1 ? 0 : 1] |= AssumeUniformAligned << k;
        zeroPush |= 1u << word;
    }

    struct Kind {
        uint32_t zeroPush;
        bool texsF16;
    };

    const bool f16 = kProvenChipFacts & AssumeTexsF16;
    std::vector<Kind> kinds;
    if (zeroPush && f16)
        kinds.push_back({zeroPush, true});
    if (zeroPush)
        kinds.push_back({zeroPush, false});
    if (f16)
        kinds.push_back({0, true});
    std::vector<RuntimeRecord::Variant> made[2];
    for (const Kind &kind : kinds) {
        Stages stages(nvk, layout, xrPlanes);
        std::string why;
        if (stages.compile(state, halfTargets, false, why, kind.zeroPush, kind.texsF16) != CompileOutcome::Made)
            continue;
        for (uint32_t k = 0; k < 2; k++) {
            const EmitProgram a = generic.emit(int(k)), b = stages.emit(int(k));
            const CapturedProgram &program = stages.program(int(k));
            const auto same = [&](const RuntimeRecord::Variant &v) {
                return v.program.image == program.image;
            };
            if (program.image == out.programs[k].image || program.cbufTypes != out.programs[k].cbufTypes ||
                a.earlyFragmentTests != b.earlyFragmentTests || a.interlockControl != b.interlockControl ||
                a.usesSampleShading != b.usesSampleShading || std::any_of(made[k].begin(), made[k].end(), same))
                continue;
            made[k].push_back({(kind.zeroPush ? residual[k] : 0) | (kind.texsF16 ? AssumeTexsF16 : 0), program});
        }
    }
    for (uint32_t k = 0; k < 2; k++) {
        out.pipeline.variantCount[k] = uint8_t(made[k].size());
        out.variants.insert(out.variants.end(), made[k].begin(), made[k].end());
    }
}

// The push words the probe draws agree a raw draw writes itself (raw_direct_push.h drawnPushMask), into the record
bool drawnWords(const NmbDirectState &state, RecordParts &parts, std::string &why)
{
    const NmbDirectDraw &d = state.draws[0];
    for (const NmbDirectDraw &other : state.draws)
        if (other.amplificationIdWord != d.amplificationIdWord ||
            other.amplificationCountWord != d.amplificationCountWord || other.arrayLengthWord != d.arrayLengthWord ||
            other.viewportCountWord != d.viewportCountWord ||
            other.amplificationLayerWord != d.amplificationLayerWord ||
            other.amplificationViewportWord != d.amplificationViewportWord ||
            memcmp(other.pulledStrides, d.pulledStrides, sizeof d.pulledStrides)) {
            why = "probe draws that place a drawn push word apart";
            return false;
        }
    if (d.amplificationMax > 0xFF) {
        why = "a vertex amplification count past 255";
        return false;
    }
    parts.amplificationMax = uint8_t(d.amplificationMax > 1 ? d.amplificationMax : 0);
    parts.amplificationIdWord = uint8_t(d.amplificationIdWord);
    parts.amplificationCountWord = uint8_t(d.amplificationCountWord);
    parts.arrayLengthWord = uint8_t(d.arrayLengthWord);
    parts.viewportCountWord = uint8_t(d.viewportCountWord);
    for (size_t k = 0; k < 4; k++) {
        parts.pulledStride[k][0] = uint8_t(d.pulledStrides[k][0]);
        parts.pulledStride[k][1] = uint8_t(d.pulledStrides[k][1]);
    }
    return true;
}

// A tessellation state's registers its draws set and the draws after it set back: its domain, spacing and output primitives,
// patches of one control point, and the control and evaluation stages on at bind groups 1 and 2 (nvk_pipeline_bind_group)
void tessellationRegisters(const NmbDirectTess &tess, RuntimeRecord &out)
{
    constexpr uint32_t kTessellationParameters = 0x0320, kPatch = 0x0dcc;
    constexpr uint32_t kShader = 0x2000, kBinding = 0x2010, kStride = 0x40, kControl = 2, kEvaluation = 3;
    out.extras.insert(out.extras.end(), {{kTessellationParameters, tess.parameters},
                                         {kPatch, 1},
                                         {kShader + kStride * kControl, 1 | kControl << 4},
                                         {kBinding + kStride * kControl, 1},
                                         {kShader + kStride * kEvaluation, 1 | kEvaluation << 4},
                                         {kBinding + kStride * kEvaluation, 2}});
    out.restore.insert(out.restore.end(), {{kShader + kStride * kControl, kControl << 4},
                                           {kShader + kStride * kEvaluation, kEvaluation << 4}});
}

// The fragment program's turned variant (AssumeTurned), first of its stage's: compiled where the generic program's code may read
// a turned plane's axes (turnSensitive) and kept where its code differs, the turn then being the program's to undo. Where it
// would be needed and cannot be made, the record says so (PipelineTurnRefused).
void turnedVariant(const NmbDirectState &state, const DirectNvk &nvk, const NakLayoutData &layout, uint32_t halfTargets,
                   uint64_t xrPlanes, const Stages &generic, RuntimeRecord &out)
{
    const CapturedProgram &fs = generic.program(1);
    if (fs.image.size() <= 80)
        return;
    uint32_t sph[20];
    memcpy(sph, fs.image.data(), sizeof sph);
    std::vector<uint32_t> code((fs.image.size() - 80) / 4);
    memcpy(code.data(), fs.image.data() + 80, code.size() * 4);
    if (!turnSensitive(sph, code.data(), uint32_t(code.size()), false))
        return;
    Stages stages(nvk, layout, xrPlanes);
    std::string why;
    const EmitProgram a = generic.emit(1);
    if (stages.compile(state, halfTargets, false, why, 0, false, true) != CompileOutcome::Made) {
        out.pipeline.flags |= PipelineTurnRefused;
        return;
    }
    const CapturedProgram &program = stages.program(1);
    const EmitProgram b = stages.emit(1);
    if (program.image == fs.image)
        return;
    if (program.cbufTypes != fs.cbufTypes || a.earlyFragmentTests != b.earlyFragmentTests ||
        a.interlockControl != b.interlockControl || a.usesSampleShading != b.usesSampleShading) {
        out.pipeline.flags |= PipelineTurnRefused;
        return;
    }
    out.variants.insert(out.variants.begin() + out.pipeline.variantCount[0], {AssumeTurned, program});
    out.pipeline.variantCount[1]++;
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
    r.batchVersion = l.batchVersion;
    r.batchThreads = l.batchThreads;
    if (!splitQmd(parts.prologue, r.qmd))
        return;
    r.cost = parts.cost;
    if (!usableSplitMetadata(r))
        return;
    out.splitPrograms[0] = programOf(parts.fs);
    out.splitPrograms[1] = programOf(parts.prologue);
    out.split = true;
}

}  // namespace

DirectNvk DirectNvk::find(void *(*symbol)(const char *name))
{
    DirectNvk nvk;
    nvk.splitParts = symbol("switch_nvk_split_parts");
    nvk.nakCompileStage = symbol("switch_nak_compile_stage");
    nvk.nakStageFinish = symbol("switch_nak_stage_finish");
    nvk.nakFreeze = symbol("switch_nak_freeze");
    nvk.nakTarget = kNakTarget;
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
    return nakCompileStage && nakStageFinish && splitParts && abiMatches;
}

CompileOutcome directRecord(const NmbDirectState &state, const DirectNvk &nvk, const PackRules &rules,
                            const std::string &key, const std::vector<uint32_t> &attributes, RuntimeRecord &out,
                            std::string &why, std::map<uint32_t, uint32_t> *registers, bool split, bool variants,
                            uint16_t xrSlots)
{
    out = {};
    const NmbDirectDraw &draw = state.draws[0];
    PackPipeline storage = {};
    const auto storageHeader = [](uint32_t format, uint32_t &word) {
        const uint32_t identity[4] = {};
        return viewWord0(format, identity, true, word);
    };
    if (const char *refused = qualifyStorage(state, storageHeader, storage)) {
        why = refused;
        return CompileOutcome::Refused;
    }
    RecordHeap heap;
    if (const char *refused = directHeap(state, heap)) {
        why = refused;
        return CompileOutcome::Refused;
    }
    if (!validDynamicPush(state)) {
        why = "inconsistent dynamic state push placement";
        return CompileOutcome::Refused;
    }
    const NakLayoutData nakLayout(state.layoutData);
    if (!nakLayout.valid) {
        why = "a descriptor type NVK lays out by another rule";
        return CompileOutcome::Refused;
    }
    const uint64_t xrPlanes = xrSlots ? xrPlaneBindings(draw, xrSlots) : 0;
    if (xrSlots && !xrPlanes) {
        why = "a two-plane texture slot the state's fragment function does not sample";
        return CompileOutcome::Refused;
    }
    // a split prologue binds its own constant buffers, set 1 not among them
    const bool splits = split && !storage.storageMask && !heap.arrays && !draw.spillWords[0] && !draw.spillWords[1] &&
                        state.stageCount == 2;
    Stages stages(nvk, nakLayout, xrPlanes);
    if (const CompileOutcome compiled = stages.compile(state, halfFloatTargets(key), splits, why);
        compiled != CompileOutcome::Made)
        return compiled;
    const uint32_t interlock = stages.emit(1).interlockControl;
    if (state.storageForm && !storage.shaderTests.flags &&
        !storageDepthQualified(key.c_str(), stages.emit(1).earlyFragmentTests, storage.storageTests)) {
        why = "storage form: depth/stencil requires a draw-qualified shader variant";
        return CompileOutcome::Refused;
    }
    if (storage.storageSamples > 1 && !stages.emit(1).usesSampleShading) {
        why = "storage form: multisampled fetch requires per-sample shader invocations";
        return CompileOutcome::Refused;
    }
    if (storage.storageMask ? !orderedStorageControl(interlock) : interlock != 0) {
        why = "storage form: compiled shader does not match the ordered attachment contract";
        return CompileOutcome::Refused;
    }
    RecordParts parts;
    parts.storageMask = storage.storageMask;
    parts.storageWriteMask = storage.storageWriteMask;
    parts.storageSamples = storage.storageSamples;
    parts.storageTests = storage.storageTests;
    parts.storageBlendMask = storage.storageBlendMask;
    parts.storageBlendModel = storage.storageBlendModel;
    parts.shaderTests = storage.shaderTests;
    parts.shaderTests.depthWord = uint8_t(draw.depthTestWord);
    parts.shaderTests.stencilWord = uint8_t(draw.stencilTestWord);
    memcpy(parts.storageTic, storage.storageTic, sizeof parts.storageTic);
    parts.pushMetadata = true;
    parts.blendColorWord = uint8_t(draw.blendColorWord);
    parts.halfBlendColorWord = uint8_t(draw.halfBlendColorWord);
    parts.programs[0] = &stages.program(0);
    parts.programs[1] = &stages.program(1);
    parts.heap = heap;
    if (state.stageCount == 4) {
        parts.tessPrograms[0] = &stages.program(2);
        parts.tessPrograms[1] = &stages.program(3);
        parts.controlWord = uint8_t(state.tess.controlWord);
        parts.controlPoints = uint8_t(state.tess.controlPoints);
        parts.controlPointIndexBytes = uint8_t(state.tess.indexBytes);
    }
    if (!programsBindable(parts, why) || !layout(draw, parts.layout, why))
        return CompileOutcome::Refused;
    parts.sizes16 = boundSizes(state.draws[1]);
    parts.sizes4096 = boundSizes(state.draws[2]);
    parts.residualsPlanned = true;
    if (const char *refused = spillTables(state, parts)) {
        why = refused;
        return CompileOutcome::Refused;
    }
    if (draw.vertexSlot != UINT32_MAX) {
        if (!draw.vertexStrideDynamic && draw.vertexStride > kStreamStrideField) {
            why = "a fixed-function vertex stream of a stride past SET_VERTEX_STREAM_A_FORMAT's 12 bits";
            return CompileOutcome::Refused;
        }
        parts.vertexSlot = uint8_t(draw.vertexSlot);
        parts.vertexStride = uint16_t(draw.vertexStride);
        parts.vertexStrideDynamic = draw.vertexStrideDynamic != 0;
    }
    if (!drawnWords(state, parts, why))
        return CompileOutcome::Refused;
    std::vector<EmitStream> streams;
    if (!emitted(key, stages, attributes, draw.pulledAttributes, state.blended, state.twoPlane, rules, parts.regs,
                 parts.alphaTargets, streams, why) ||
        !vertexStreams(streams, parts, why))
        return CompileOutcome::Refused;
    const uint32_t addresses = addressPushWords(draw);
    for (uint32_t i = 0; i < 32; i++)
        if ((draw.pushMask | addresses) >> i & 1)
            parts.push[kRootPush + 4 * i] = addresses >> i & 1 ? 0 : draw.push[i];
    parts.setBytes = uint16_t(Set0(draw.set0Bindings, draw.set0BindingCount).bytes());
    if (!assembleRecord(parts, key, rules, out, why))
        return CompileOutcome::Refused;
    out.amplificationLayerWord = uint8_t(draw.amplificationLayerWord);
    out.amplificationViewportWord = uint8_t(draw.amplificationViewportWord);
    if (!usableStorageMetadata(out.pipeline, out.blob.data(), uint32_t(out.blob.size()))) {
        why = "storage form: assembled record lost its attachment contract";
        return CompileOutcome::Refused;
    }
    if (state.stageCount == 4)
        tessellationRegisters(state.tess, out);
    if (registers)
        *registers = parts.regs;
    if (state.twoPlane)
        out.pipeline.flags |= PipelineTwoPlane;
    out.pipeline.xrSlots = xrSlots;
    out.splitTried = split && state.stageCount == 2;
    if (splits)
        splitInto(nvk, stages.split(1), out);
    if (variants && state.stageCount == 2)
        programVariants(state, nvk, nakLayout, halfFloatTargets(key), xrPlanes, stages, out);
    turnedVariant(state, nvk, nakLayout, halfFloatTargets(key), xrPlanes, stages, out);
    return CompileOutcome::Made;
}

}  // namespace raw

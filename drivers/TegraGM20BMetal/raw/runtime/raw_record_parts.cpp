// SPDX-License-Identifier: MIT
// See raw_record_parts.h. Each step follows the pack/mkpack.py or pack/packfile.py function it is named after: they are one rule,
// and a change to one is a change to the other (test/recordcheck fails otherwise).
#include "raw_record_parts.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>
#include "raw_owned.h"
#include "raw_set_reads.h"
#include "raw_spill_table.h"

namespace raw {
namespace {

constexpr uint32_t kProgramVertex = 0x2044, kProgramPixel = 0x2144;

// mkpack.py disabled_stream: SET_VERTEX_STREAM_A_FORMAT(j) with ENABLE clear, a stream the hardware does not fetch
bool disabledStream(uint32_t m, uint32_t v)
{
    return m >= 0x1C00 && m < 0x1E00 && (m - 0x1C00) % 0x10 == 0 && !(v & 0x1000);
}

uint32_t valueOf(const std::map<uint32_t, uint32_t> &regs, uint32_t m)
{
    const auto it = regs.find(m);
    return it == regs.end() ? 0 : it->second;
}

// mkpack.py raw_bindable: set 1 (the heap's arrays) only for a state whose record describes them
bool rawBindable(const CapturedProgram &p, bool heap)
{
    const std::vector<uint8_t> &t = p.cbufTypes;
    if (t.empty() || t[0] != CbufRoot)
        return false;
    bool data = false, set = false, heapSet = false;
    uint32_t ubos = 0;
    for (size_t i = 1; i < t.size(); i++) {
        if (cbufKind(t[i]) == CbufUbo) {
            const uint32_t bit = 1u << (cbufUboOffset(t[i]) / 16);
            if (ubos & bit)
                return false;
            ubos |= bit;
            continue;
        }
        if (heap && t[i] == kCbufHeapSet && !heapSet) {
            heapSet = true;
            continue;
        }
        if ((t[i] != CbufShaderData && t[i] != CbufSet) || (t[i] == CbufShaderData ? data : set))
            return false;
        (t[i] == CbufShaderData ? data : set) = true;
    }
    return data == !p.data.empty();
}

// mkpack.py pipeline: every uniform buffer a program reads as a constant buffer is a uniform entry of set 0
bool ubosNamed(const CapturedProgram &p, const PackPipeline &pipe)
{
    for (size_t i = 1; i < p.cbufTypes.size(); i++) {
        if (cbufKind(p.cbufTypes[i]) != CbufUbo)
            continue;
        bool named = false;
        for (uint32_t k = 0; k < pipe.entries && !named; k++)
            named = uniformEntry(pipe.entry[k]) && pipe.entry[k].offset == cbufUboOffset(p.cbufTypes[i]);
        if (!named)
            return false;
    }
    return true;
}

std::string cbufList(const std::vector<uint8_t> &types)
{
    std::string out = "[";
    for (size_t i = 0; i < types.size(); i++)
        out += (i ? ", " : "") + std::to_string(types[i]);
    return out + "]";
}

// packfile.py entry_bytes, from mkpack.py buffer_entry: a 16-byte payload bound at 256 is a uniform buffer (NVK rounds its range
// up to the cbuf alignment), else storage; the 4096-byte one's range is the function's window on the slot (0: the whole payload);
// its residual push word (packfile.py buffer_kind), a storage one's where no other storage slot shares its code
PackEntry bufferEntry(const LayoutEntry &e, const RecordParts &parts, bool codeShared)
{
    uint32_t s16 = 256, s4k = 256;
    if (parts.sizes16.count(e.offset) || parts.sizes4096.count(e.offset)) {
        s16 = valueOf(parts.sizes16, e.offset);
        s4k = valueOf(parts.sizes4096, e.offset);
    }
    const uint32_t window = s4k >= 4096 ? 0 : s4k;
    PackEntry p = {};
    p.offset = uint16_t(e.offset);
    p.kind = EntryUbo;
    p.stage = e.stage;
    p.slot = e.slot;
    p.samplerKind = s16 < 256 ? 1 : 0;
    const uint32_t value = p.samplerKind ? probeStorageCode(e.stage, e.slot) : probeBindOffset(e.stage, e.slot);
    if (parts.residualsPlanned) {
        if (e.stage >= 1 && e.stage <= 2 && e.slot < 32)
            p.samplerKind |= uint8_t(parts.residualWord[e.stage - 1][e.slot] << 2);
    } else if (const auto at = parts.residuals.find(value);
               at != parts.residuals.end() && !(p.samplerKind && codeShared)) {
        p.samplerKind |= uint8_t(((at->second - 0x700) / 4 + 1) << 2);
    }
    p.sampler = uint8_t(window / 16);
    p.pad = uint8_t(window / 16 >> 8);
    return p;
}

// packfile.py entry_bytes, from mkpack.py handle_entry; false: a problem (`why`)
bool handleEntry(const LayoutEntry &e, RuntimeRecord &rec, PackEntry &p, std::string &why)
{
    p = {};
    p.offset = uint16_t(e.offset);
    p.stage = e.stage == 1 ? 1 : 2;
    switch (e.tex) {
    case Tex::None:
        p.kind = EntrySamplerOnly;
        break;
    case Tex::Slot:
    case Tex::Fetch:
        p.kind = e.tex == Tex::Slot ? EntryTexture : EntryFetch;
        p.slot = e.texSlot;
        break;
    case Tex::Texture3D:
        p.kind = EntryTexture3d;
        p.slot = e.texSlot;
        break;
    case Tex::DepthTest:
    case Tex::StencilTest:
        p.kind = e.tex == Tex::DepthTest ? EntryDepthTest : EntryStencilTest;
        p.slot = 0;
        break;
    case Tex::Unknown:
        why = "texture unidentified";
        return false;
    }
    switch (e.smp) {
    case Smp::None:
        break;
    case Smp::Slot:
        p.samplerKind = SamplerSlot;
        p.sampler = e.smpSlot;
        p.pad = e.smpCompare ? kEntryCompares : 0;
        break;
    case Smp::Derived:
        why = "a sampler m2v derives from a bound one (YCbCr conversion)";
        return false;
    case Smp::Constexpr: {
        // a sampler the probe did not bind: the function's constexpr sampler (made by m2v as an immutable one)
        if (!e.headerKnown) {
            why = "a constexpr sampler's header unread";
            return false;
        }
        const size_t at = std::find(rec.samplers.begin(), rec.samplers.end(), e.header) - rec.samplers.begin();
        if (at == rec.samplers.size())
            rec.samplers.push_back(e.header);
        p.samplerKind = SamplerConstexpr;
        p.sampler = uint8_t(at);
        break;
    }
    }
    return true;
}

// mkpack.py split_registers and pipeline's blob: the varying registers' values; and those outside them the state leaves other
// than every pack state does
void registers(const std::map<uint32_t, uint32_t> &regs, uint32_t colors, const PackRules &rules, RuntimeRecord &rec)
{
    for (uint32_t m : rules.varying)
        if (m != kProgramVertex && m != kProgramPixel && !foreign(m, colors)) {
            rec.blob.push_back(m);
            rec.blob.push_back(valueOf(regs, m));
        }
    std::set<uint32_t> others;
    for (const auto *from : {&regs, &rules.baseline, &rules.context})
        for (const auto &kv : *from)
            others.insert(kv.first);
    for (uint32_t m : others) {
        if (rules.varying.count(m) || ownedMethod(m) || foreign(m, colors) || m == kProgramVertex || m == kProgramPixel)
            continue;
        const auto base = rules.baseline.find(m);
        const uint32_t expected = base != rules.baseline.end() ? base->second : valueOf(rules.context, m);
        if (valueOf(regs, m) != expected) {
            rec.extras.push_back({m, valueOf(regs, m)});
            rec.restore.push_back({m, expected});
        }
    }
}

}  // namespace

uint32_t targets(const std::string &key)
{
    uint32_t mask = 0;
    for (size_t at = 0; at < key.size();) {
        size_t end = key.find(' ', at);
        if (end == std::string::npos)
            end = key.size();
        if (end - at >= 2 && key[at] == 'c' && isdigit(static_cast<unsigned char>(key[at + 1])) &&
            key.find('=', at) < end)
            mask |= 1u << (key[at + 1] - '0');
        at = end + 1;
    }
    return mask;
}

bool foreign(uint32_t m, uint32_t targets)
{
    const auto lacks = [&](uint32_t j) {
        return !(targets >> j & 1);
    };
    return (m >= 0x1E00 && m < 0x1F00 && lacks((m - 0x1E00) / 0x20)) ||
           (m >= 0x1A00 && m < 0x1A20 && lacks((m - 0x1A00) / 4)) ||
           (m >= 0x1360 && m < 0x1380 && lacks((m - 0x1360) / 4));
}

bool programsBindable(const RecordParts &parts, std::string &why)
{
    const CapturedProgram *all[] = {parts.programs[0], parts.programs[1], parts.tessPrograms[0], parts.tessPrograms[1]};
    for (uint32_t k = 0; k < (parts.controlWord ? 4u : 2u); k++) {
        const CapturedProgram *p = all[k];
        if (!p) {
            why = "unknown program";
            return false;
        }
        if (!rawBindable(*p, parts.heap.arrays != 0)) {
            why = "a program reads constant buffers " + cbufList(p->cbufTypes) +
                  ", the raw path binds root, shader data, set 0, its uniform buffers and a heap's set 1";
            return false;
        }
        constexpr size_t kPackCbufs = sizeof(PackProgram::cbufType);
        if (p->cbufTypes.size() > kPackCbufs) {
            why = "a program reads " + std::to_string(p->cbufTypes.size()) + " constant buffers, past the " +
                  std::to_string(kPackCbufs) + " a pack program records";
            return false;
        }
    }
    return true;
}

// The constant buffer slot `program` reads uniform entry `e` at; kNoUniform: none
static uint8_t uniformBank(const CapturedProgram &program, const PackEntry &e)
{
    for (size_t c = 1; c < program.cbufTypes.size(); c++)
        if (cbufKind(program.cbufTypes[c]) == CbufUbo && cbufUboOffset(program.cbufTypes[c]) == e.offset)
            return uint8_t(c);
    return kNoUniform;
}

// The colors the key's descriptor has a write mask for ("c<n>=<format>:<mask>:...")
static uint8_t describedWrites(const std::string &key)
{
    uint8_t writes = 0;
    const std::string head = key.substr(0, key.find(" | "));
    for (size_t at = head.find(" c"); at != std::string::npos; at = head.find(" c", at + 2)) {
        unsigned color = 0, format = 0, mask = 0;
        if (sscanf(head.c_str() + at, " c%u=%u:%u:", &color, &format, &mask) == 3 && color < 8 && mask)
            writes |= uint8_t(1u << color);
    }
    return writes;
}

// The uniform entries the programs read (the assumption bits they are named by: PackPipeline::uniform), each color's rung, and
// the set-0 handle words the fragment program's bound textures read. A fetched color the state does not write needs no more
// than a barrier; one it writes, the interlock (where the function writes it, for a storage form's).
static void describeReads(RuntimeRecord &out, const std::string &key)
{
    PackPipeline &p = out.pipeline;
    memset(p.uniform, kNoUniform, sizeof p.uniform);
    const uint8_t writes = (describedWrites(key) & ~p.storageMask) | (p.storageWriteMask & p.storageMask);
    uint32_t n = 0;
    for (uint32_t k = 0; k < p.entries; k++) {
        const PackEntry &e = p.entry[k];
        if (e.kind == EntryFetch && e.slot < 8)
            p.fetchRung[e.slot] = writes >> e.slot & 1 ? FetchInterlock : FetchBarrier;
        if (e.kind != EntryUbo || bufferStorage(e) || n == kUniformRanges)
            continue;
        const uint8_t bank = uniformBank(out.programs[e.stage == 1 ? 0 : 1], e);
        if (bank != kNoUniform)
            p.uniform[n++] = {uint8_t(k), bank, 0, 0};
    }
    const CapturedProgram &fs = out.programs[1];
    if (!fs.boundTextures || fs.boundTextures > fs.cbufTypes.size() || fs.cbufTypes[fs.boundTextures - 1] != CbufSet)
        return;
    for (uint32_t k = 0; k < p.entries; k++)
        if (p.entry[k].kind != EntryUbo && p.entry[k].kind != EntryColorTable && p.entry[k].kind != EntryTextureTable &&
            p.entry[k].kind != EntrySamplerTable && p.entry[k].kind != EntrySpillTable)
            p.boundEntries |= 1u << k;
}

bool assembleRecord(const RecordParts &parts, const std::string &key, const PackRules &rules, RuntimeRecord &out,
                    std::string &why)
{
    if (!programsBindable(parts, why))
        return false;
    out.programs[0] = *parts.programs[0];
    out.programs[1] = *parts.programs[1];
    if (parts.controlWord) {
        out.tessPrograms[0] = *parts.tessPrograms[0];
        out.tessPrograms[1] = *parts.tessPrograms[1];
    }
    if (parts.layout.size() > kPackEntries) {
        why = "more than " + std::to_string(kPackEntries) + " set-0 entries";
        return false;
    }
    if (renderSetBytes(parts.setBytes) > kRenderSetBytes) {
        why = "a set 0 of " + std::to_string(parts.setBytes) + " bytes, past the render encoder's " +
              std::to_string(kRenderSetBytes);
        return false;
    }
    PackPipeline &p = out.pipeline;
    std::map<uint32_t, int> storageCodes;
    for (const LayoutEntry &e : parts.layout)
        if (e.buffer && bufferEntry(e, parts, false).samplerKind & 1)
            storageCodes[probeStorageCode(e.stage, e.slot)]++;
    for (const LayoutEntry &e : parts.layout) {
        PackEntry &entry = p.entry[p.entries++];
        if (e.colorTable)
            entry = {uint16_t(e.offset), EntryColorTable, 2, 0, 0, 0, 0};
        else if (e.table)
            entry = {uint16_t(e.offset), e.table, e.stage, 0, 0, 0, 0};
        else if (e.tessBuffer >= 0 && e.tessBuffer < TessBufferCount && parts.controlWord)
            entry = {uint16_t(e.offset),
                     EntryTessBuffer,
                     1,
                     uint8_t(e.tessBuffer),
                     uint8_t(1 | (parts.controlWord + kTessResidualWord[e.tessBuffer]) << 2),
                     0,
                     0};
        else if (e.buffer)
            entry = bufferEntry(e, parts, storageCodes[probeStorageCode(e.stage, e.slot)] > 1);
        else if (!handleEntry(e, out, entry, why))
            return false;
        if (entry.kind == EntryFetch)
            p.flags |= PipelineFetches;
        if ((entry.kind == EntryTexture || entry.kind == EntryTexture3d) && e.writtenFormat && e.stage != 1 &&
            entry.slot < kWrittenSlots)
            p.writtenFormat[entry.slot] = e.writtenFormat;
    }
    if (!ubosNamed(out.programs[0], p) || !ubosNamed(out.programs[1], p) ||
        (parts.controlWord && (!ubosNamed(out.tessPrograms[0], p) || !ubosNamed(out.tessPrograms[1], p)))) {
        why = "a program reads a set-0 uniform buffer the layout does not name";
        return false;
    }
    p.vertexSlot = parts.vertexSlot;
    p.vertexStride = parts.vertexStride;
    p.vertexStrideDynamic = parts.vertexStrideDynamic;
    memcpy(p.pulledStride, parts.pulledStride, sizeof p.pulledStride);
    memcpy(p.vertexStreams, parts.vertexStreams, sizeof p.vertexStreams);
    p.amplificationMax = parts.amplificationMax;
    p.amplificationIdWord = parts.amplificationIdWord;
    p.amplificationCountWord = parts.amplificationCountWord;
    p.arrayLengthWord = parts.arrayLengthWord;
    p.viewportCountWord = parts.viewportCountWord;
    const uint32_t colors = targets(key) | parts.alphaTargets;
    std::map<uint32_t, uint32_t> regs;
    for (const auto &[m, v] : parts.regs)
        if (!foreign(m, colors))
            regs[m] = disabledStream(m, v) ? 0 : v;
    registers(regs, colors, rules, out);
    if (parts.storageMask) {
        for (uint32_t m :
             {0x0210u, 0x0754u, 0x1224u, 0x1a00u, 0x1a04u, 0x1a08u, 0x1a0cu, 0x1a10u, 0x1a14u, 0x1a18u, 0x1a1cu}) {
            if (m >= 0x1a00 && !(parts.storageMask & (1u << ((m - 0x1a00) / 4))))
                continue;
            bool found = false;
            for (size_t i = 0; i < out.blob.size(); i += 2)
                found |= out.blob[i] == m;
            if (!found) {
                out.blob.push_back(m);
                out.blob.push_back(valueOf(regs, m));
            }
        }
    }
    p.keyHash = packKeyHash(key);
    p.vs = 0;
    p.fs = 1;
    p.blobDwords = uint32_t(out.blob.size());
    for (const auto &[offset, word] : parts.push) {
        if (offset < 0x700 || offset >= 0x780)
            continue;
        p.pushMask |= 1u << (offset - 0x700) / 4;
        p.pushWords[(offset - 0x700) / 4] = word;
        p.pushes++;
    }
    p.setBytes = parts.setBytes;
    p.controlWord = parts.controlWord;
    p.controlPoints = parts.controlPoints;
    p.controlPointIndexBytes = parts.controlPointIndexBytes;
    p.flags |= parts.pushMetadata ? PipelinePushMetadata : 0;
    p.blendColorWord = parts.blendColorWord;
    p.halfBlendColorWord = parts.halfBlendColorWord;
    p.flags |= parts.storageMask ? PipelineStorageMetadata : 0;
    p.storageMask = parts.storageMask;
    p.storageWriteMask = parts.storageWriteMask;
    p.storageSamples = parts.storageSamples;
    p.storageTests = parts.storageTests;
    p.storageBlendMask = parts.storageBlendMask;
    p.storageBlendModel = parts.storageBlendModel;
    p.shaderTests = parts.shaderTests;
    memcpy(p.storageTic, parts.storageTic, sizeof p.storageTic);
    memcpy(p.rootTableWord, parts.heap.rootTableWord, sizeof p.rootTableWord);
    memcpy(p.statusWord, parts.heap.statusWord, sizeof p.statusWord);
    memcpy(p.rootSlots, parts.heap.rootSlots, sizeof p.rootSlots);
    p.heapArrays = parts.heap.arrays;
    p.heapBytes = parts.heap.bytes;
    memcpy(p.heap, parts.heap.array, sizeof p.heap);
    out.spill.clear();
    for (uint32_t k = 0; k < 2; k++) {
        for (uint32_t i = 0; i < p.entries; i++)
            if (p.entry[i].kind == EntrySpillTable && p.entry[i].stage == k + 1)
                p.spillEntry[k] = uint8_t(i + 1);
        if (bool(p.spillEntry[k]) != !parts.spill[k].empty() || parts.spill[k].size() > kSpillWordsMax) {
            why = "an argument table its layout and its words disagree on";
            return false;
        }
        p.spillWords[k] = uint8_t(parts.spill[k].size());
        out.spill.insert(out.spill.end(), parts.spill[k].begin(), parts.spill[k].end());
    }
    describeReads(out, key);
    constexpr size_t kHeaderBytes = 0x50;
    for (const CapturedProgram *program :
         {&out.programs[0], &out.programs[1], &out.tessPrograms[0], &out.tessPrograms[1]})
        if (program->image.size() > kHeaderBytes)
            p.memoryEntries |= programSetReads(program->image.data() + kHeaderBytes,
                                               program->image.size() - kHeaderBytes, program->cbufTypes.data(),
                                               uint32_t(program->cbufTypes.size()), program->boundTextures, p);
    const size_t bar = key.find(" | ");
    out.keyString = key.substr(0, bar);
    while (!out.keyString.empty() && isspace(static_cast<unsigned char>(out.keyString.back())))
        out.keyString.pop_back();
    return true;
}

uint32_t recordSetReads(const RuntimeRecord &rec)
{
    constexpr size_t kHeaderBytes = 0x50;
    const PackPipeline &p = rec.pipeline;
    uint32_t reads = p.memoryEntries;
    const auto add = [&](const CapturedProgram &program) {
        if (program.image.size() > kHeaderBytes)
            reads |=
                programSetReads(program.image.data() + kHeaderBytes, program.image.size() - kHeaderBytes,
                                program.cbufTypes.data(), uint32_t(program.cbufTypes.size()), program.boundTextures, p);
    };
    for (const RuntimeRecord::Variant &v : rec.variants)
        add(v.program);
    if (rec.split) {
        add(rec.splitPrograms[0]);
        add(rec.splitPrograms[1]);
    }
    return reads;
}

uint8_t dropUnreadFetches(RuntimeRecord &rec)
{
    PackPipeline &p = rec.pipeline;
    const uint32_t reads = recordSetReads(rec);
    uint8_t unread = 0, read = 0;
    for (uint32_t k = 0; k < p.entries && k < kPackEntries; k++) {
        const PackEntry &e = p.entry[k];
        if (e.kind == EntryFetch && e.slot < 8)
            (reads >> k & 1 ? read : unread) |= uint8_t(1u << e.slot);
    }
    const uint8_t dropped = unread & ~read;
    if (!dropped)
        return 0;
    for (uint32_t k = 0; k < p.entries && k < kPackEntries; k++) {
        PackEntry &e = p.entry[k];
        if (e.kind == EntryFetch && e.slot < 8 && (dropped >> e.slot & 1))
            e = {e.offset, EntrySamplerOnly, e.stage, 0, SamplerNone, 0, 0};
    }
    p.storageMask &= uint8_t(~dropped);
    p.storageWriteMask &= uint8_t(~dropped);
    p.storageBlendMask &= uint8_t(~dropped);
    if (!p.storageBlendMask)
        p.storageBlendModel = 0;
    for (uint32_t c = 0; c < 8; c++)
        if (dropped >> c & 1) {
            p.storageTic[c] = 0;
            p.fetchRung[c] = FetchNone;
        }
    if (!read)
        p.flags &= ~uint32_t(PipelineFetches);
    return dropped;
}

bool loadsSurface(const RuntimeRecord &rec)
{
    constexpr size_t kHeaderBytes = 0x50;
    // tools/gpu/maxwell/opcodes.txt SULD: bits 63-53 1110 1011 000
    constexpr uint16_t kSuldMask = 0xffe0, kSuld = 0xeb00;
    const auto loads = [&](const CapturedProgram &program) {
        for (size_t at = kHeaderBytes; at + 8 <= program.image.size(); at += 8) {
            if ((at - kHeaderBytes) % 32 == 0)
                continue;
            uint64_t word;
            memcpy(&word, program.image.data() + at, sizeof word);
            if ((uint16_t(word >> 48) & kSuldMask) == kSuld)
                return true;
        }
        return false;
    };
    if (loads(rec.programs[0]) || loads(rec.programs[1]))
        return true;
    for (const RuntimeRecord::Variant &v : rec.variants)
        if (loads(v.program))
            return true;
    return rec.split && (loads(rec.splitPrograms[0]) || loads(rec.splitPrograms[1]));
}

std::string strideTwinKey(const std::string &key)
{
    return key + " | strides pulled";
}

uint64_t packKeyHash(const std::string &key)
{
    const size_t vertexAt = key.find(" | v "), fragmentAt = key.find(" | f ");
    std::string pair;
    if (vertexAt != std::string::npos && fragmentAt != std::string::npos && fragmentAt > vertexAt) {
        std::string vertex = key.substr(vertexAt + 5, fragmentAt - vertexAt - 5);
        while (!vertex.empty() && vertex.back() == ' ')
            vertex.pop_back();
        pair = vertex + " | f " + key.substr(fragmentAt + 5);
    }
    for (const char *state : {"ycbcr_vert | f ycbcr_converted_frag",
                              "ycbcr_vert | f ycbcr_srgb_frag",
                              "ycbcr_vert | f ycbcr_codes_frag",
                              "ycbcr_vert | f ycbcr_codes10_frag",
                              "ycbcr_vert | f ycbcr_packed_converted_frag",
                              "ycbcr_vert | f ycbcr_packed_srgb_frag",
                              "ycbcr_vert | f ycbcr_packed_codes10_frag",
                              "ycbcr_vert | f ycbcr_packed_plane_frag",
                              "ycbcr_store_vert | f ycbcr_store_luma8_frag",
                              "ycbcr_store_vert | f ycbcr_store_luma16_frag",
                              "ycbcr_store_vert | f ycbcr_store_chroma8_frag",
                              "ycbcr_store_vert | f ycbcr_store_chroma16_frag",
                              "ycbcr_store_vert | f ycbcr_store_luma8_srgb_frag",
                              "ycbcr_store_vert | f ycbcr_store_luma16_srgb_frag",
                              "ycbcr_store_vert | f ycbcr_store_chroma8_srgb_frag",
                              "ycbcr_store_vert | f ycbcr_store_chroma16_srgb_frag",
                              "ycbcr_store_vert | f ycbcr_store_luma_packed_frag",
                              "ycbcr_store_vert | f ycbcr_store_chroma_packed_frag",
                              "ycbcr_store_vert | f ycbcr_store_luma_packed_srgb_frag",
                              "ycbcr_store_vert | f ycbcr_store_chroma_packed_srgb_frag",
                              "ycbcr_store_vert | f ycbcr_store_plane_packed_frag",
                              "turn_vert | f turn_copy8_frag",
                              "turn_vert | f turn_copy16_frag",
                              "turn_vert | f turn_copy32_frag",
                              "turn_vert | f turn_copy64_frag",
                              "turn_vert | f turn_copy128_frag",
                              "turn_vert | f turn_masked8_frag",
                              "turn_vert | f turn_masked16_frag",
                              "turn_vert | f turn_masked32_frag",
                              "turn_vert | f turn_masked64_frag",
                              "turn_vert | f turn_masked128_frag",
                              "turn_vert | f turn_reach_frag",
                              "turn_vert | f turn_reach_reduce_frag",
                              "turn_vert | f turn_depth16_frag",
                              "turn_vert | f turn_depth32f_frag",
                              "turn_vert | f turn_depth32fs8_frag",
                              "stencil_bits_vert | f stencil_bits_s8_frag",
                              "stencil_bits_vert | f stencil_bits_d32fs8_frag",
                              "stencil_bits_vert | f stencil_bits_ms_s8_frag",
                              "stencil_bits_vert | f stencil_bits_ms_d32fs8_frag",
                              "stencil_bits_vert | f stencil_bits_ms_d32fs8_min_frag",
                              "stencil_bits_vert | f stencil_bits_ms_d32fs8_max_frag",
                              "stencil_bits_vert | f stencil_bits_s8_colour_frag",
                              "stencil_bits_vert | f stencil_bits_d32fs8_colour_frag",
                              "stencil_bits_vert | f stencil_bits_ms_s8_colour_frag",
                              "stencil_bits_vert | f stencil_bits_ms_d32fs8_colour_frag",
                              "stencil_bits_vert | f stencil_bits_ms_d32fs8_min_colour_frag",
                              "stencil_bits_vert | f stencil_bits_ms_d32fs8_max_colour_frag",
                              "xr_vert | f xr_bgr10_frag",
                              "xr_vert | f xr_bgra10_frag",
                              "xr_vert | f xr_planes_frag",
                              "xr_vert | f xr_two_planes_frag",
                              "two_plane_cover | f two_plane_load",
                              "two_plane_cover | f two_plane_store",
                              "resolve_vert | f resolve_rgba8_frag",
                              "resolve_vert | f resolve_rgba8_srgb_frag",
                              "resolve_vert | f resolve_bgra8_frag",
                              "resolve_vert | f resolve_bgra8_srgb_frag",
                              "resolve_vert | f resolve_rgba8_2x_frag",
                              "resolve_vert | f resolve_rgba8_4x_frag",
                              "resolve_vert | f resolve_bgra8_2x_frag",
                              "resolve_vert | f resolve_bgra8_4x_frag",
                              "resolve_vert | f resolve_rg11b10f_frag",
                              "resolve_vert | f resolve_rgba16f_frag",
                              "resolve_vert | f resolve_depth16_sample0_frag",
                              "resolve_vert | f resolve_depth16_min_frag",
                              "resolve_vert | f resolve_depth16_max_frag",
                              "resolve_vert | f resolve_depth32f_sample0_frag",
                              "resolve_vert | f resolve_depth32f_min_frag",
                              "resolve_vert | f resolve_depth32f_max_frag",
                              "resolve_vert | f resolve_depth32fs8_sample0_frag",
                              "resolve_vert | f resolve_depth32fs8_min_frag",
                              "resolve_vert | f resolve_depth32fs8_max_frag",
                              "mip_vert | f mip_a8_frag",
                              "mip_vert | f mip_r16_frag",
                              "mip_vert | f mip_r16f_frag",
                              "mip_vert | f mip_b5g6r5_frag",
                              "mip_vert | f mip_bgr5a1_frag",
                              "mip_vert | f mip_r32f_frag",
                              "mip_vert | f mip_rg16_frag",
                              "mip_vert | f mip_rg16f_frag",
                              "mip_vert | f mip_rgba8_srgb_frag",
                              "mip_vert | f mip_bgra8_srgb_frag",
                              "mip_vert | f mip_rgb10a2_frag",
                              "mip_vert | f mip_rg11b10f_frag",
                              "mip_vert | f mip_bgr10a2_frag",
                              "mip_vert | f mip_rg32f_frag",
                              "mip_vert | f mip_rgba16_frag",
                              "mip_vert | f mip_rgba32f_frag"}) {
        if (pair != state)
            continue;
        const size_t vertexEnd = pair.find(" | f ");
        const size_t familyEnd = pair.rfind('_', vertexEnd);
        const std::string name = "raw." + pair.substr(0, familyEnd) + "." + pair.substr(vertexEnd + 5);
        return Pack::fnv(name.data(), name.size());
    }
    return Pack::fnv(key.data(), key.size());
}

PackRules PackRules::of(const Pack &pack)
{
    std::map<uint32_t, uint32_t> context;
    const PackInitMethod *init = pack.section<PackInitMethod>(PackInit);
    for (uint64_t i = 0; i < pack.bytes(PackInit) / sizeof(PackInitMethod); i++)
        if (init[i].subc == 0 && init[i].reloc == RelocNone && init[i].data)
            context[init[i].mthd] = init[i].data;
        else if (init[i].subc == 0)
            context.erase(init[i].mthd);
    return of(pack, std::move(context));
}

PackRules PackRules::of(const Pack &pack, std::map<uint32_t, uint32_t> context)
{
    PackRules r;
    r.context = std::move(context);
    const PackPipeline *pipes = pack.section<PackPipeline>(PackPipelines);
    const uint32_t *blob = pack.section<uint32_t>(PackBlob);
    for (uint32_t i = 0; i < pack.header()->pipelines; i++)
        for (uint32_t k = 0; k < pipes[i].blobDwords; k += 2)
            r.varying.insert(blob[pipes[i].blob + k]);
    const uint32_t *base = pack.section<uint32_t>(PackBaseline);
    for (uint64_t i = 0; i < pack.bytes(PackBaseline) / 8; i++)
        r.baseline[base[2 * i]] = base[2 * i + 1];
    return r;
}

std::vector<uint8_t> programSlot(const CapturedProgram &p, PackProgram &info)
{
    std::vector<uint8_t> slot(0x30);
    slot.insert(slot.end(), p.image.begin(), p.image.end());
    slot.resize((slot.size() + 0xFF) & ~size_t(0xFF));
    info = {};
    info.program = 0x30;
    info.dataOffset = p.data.empty() ? 0 : uint32_t(slot.size());
    info.dataBytes = uint32_t(p.data.size());
    slot.insert(slot.end(), p.data.begin(), p.data.end());
    info.bytes = uint32_t(slot.size());
    info.cbufs = uint8_t(std::min<size_t>(p.cbufTypes.size(), sizeof info.cbufType));
    memcpy(info.cbufType, p.cbufTypes.data(), info.cbufs);
    info.gprs = p.gprs;
    info.boundTextures = p.boundTextures;
    return slot;
}

}  // namespace raw

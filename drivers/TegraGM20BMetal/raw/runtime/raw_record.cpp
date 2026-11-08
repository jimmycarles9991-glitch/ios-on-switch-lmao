// SPDX-License-Identifier: MIT
// See raw_record.h. Each step follows the pack/mkpack.py or pack/layouts.py function it is named after: they are one rule, and a
// change to one is a change to the other (test/recordcheck fails otherwise).
#include "raw_record.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include "raw_owned.h"

namespace raw {
namespace {

constexpr uint32_t kProgramVertex = 0x2044, kProgramPixel = 0x2144;
constexpr uint32_t kInterlockControl = 0x1224;  // SET_PIXEL_SHADER_INTERLOCK_CONTROL
constexpr uint32_t kTextureType3D = 2;          // TEXHEAD's TEXTURE_TYPE (word 4 bits 26:23)
constexpr uint32_t kTagBuffer = 0xC0DE;         // pack/probe.mm's payload tag

std::string hex(uint64_t v)
{
    char b[24];
    snprintf(b, sizeof b, "%llx", static_cast<unsigned long long>(v));
    return b;
}

// mkpack.py targets: the color attachments the key names
uint32_t targets(const std::string &key)
{
    uint32_t n = 0;
    for (size_t at = 0; at < key.size();) {
        size_t end = key.find(' ', at);
        if (end == std::string::npos)
            end = key.size();
        if (end - at >= 2 && key[at] == 'c' && isdigit(static_cast<unsigned char>(key[at + 1])) &&
            key.find('=', at) < end)
            n++;
        at = end + 1;
    }
    return n;
}

// mkpack.py foreign: a color target's own register for a target the state does not have (its blend, SET_CT_WRITE, SET_BLEND)
bool foreign(uint32_t m, uint32_t targets)
{
    return (m >= 0x1E00 && m < 0x1F00 && (m - 0x1E00) / 0x20 >= targets) ||
           (m >= 0x1A00 && m < 0x1A20 && (m - 0x1A00) / 4 >= targets) ||
           (m >= 0x1360 && m < 0x1380 && (m - 0x1360) / 4 >= targets);
}

uint32_t valueOf(const std::map<uint32_t, uint32_t> &regs, uint32_t m)
{
    const auto it = regs.find(m);
    return it == regs.end() ? 0 : it->second;
}

// mkpack.py interlocked: the storage form, in one draw or in a scratch between m2v's copy in and copy out
bool interlocked(const nvdec::ProbeSubmission &s)
{
    for (const nvdec::ProbeRecord &r : s.records)
        if (valueOf(r.regs, kInterlockControl) & 3)
            return true;
    return false;
}

// mkpack.py skip_reason
bool skipReason(const nvdec::ProbeSubmission &s, std::string &why)
{
    if (s.draws != 1 && !s.dispatches && interlocked(s))
        why = "storage form through a scratch, m2v copying the color in and out around it";
    else if (s.draws != 1)
        why = std::to_string(s.draws) + " draws";
    else if (s.dispatches)
        why = std::to_string(s.dispatches) + " dispatches";
    else if (s.records.empty())
        why = "no record";
    if (!why.empty())
        return true;
    std::string stages;
    for (const auto &[stage, program] : s.records[0].programs) {
        if (!program.known) {
            why = "unknown program";
            return true;
        }
        stages += (stages.empty() ? "" : ", ") + std::to_string(stage);
    }
    if (s.records[0].programs.size() != 2 || !s.records[0].programs.count(1) || !s.records[0].programs.count(5)) {
        why = "stages [" + stages + "]";
        return true;
    }
    return false;
}

// mkpack.py raw_bindable
bool rawBindable(const CapturedProgram &p)
{
    const std::vector<uint8_t> &t = p.cbufTypes;
    if (t.empty() || t[0] != CbufRoot)
        return false;
    bool data = false, set = false;
    uint32_t ubos = 0;
    for (size_t i = 1; i < t.size(); i++) {
        if (cbufKind(t[i]) == CbufUbo) {
            const uint32_t bit = 1u << (cbufUboOffset(t[i]) / 16);
            if (ubos & bit)
                return false;
            ubos |= bit;
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
            named = pipe.entry[k].kind == EntryUbo && !bufferStorage(pipe.entry[k]) &&
                    pipe.entry[k].offset == cbufUboOffset(p.cbufTypes[i]);
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

enum class Tex : uint8_t {
    None,
    Slot,
    Fetch,
    Texture3D,
    Unknown
};

enum class Smp : uint8_t {
    None,
    Slot,
    Derived,
    Constexpr
};

struct LayoutEntry {
    uint32_t offset = 0;
    bool buffer = false;
    uint8_t stage = 0, slot = 0;  // a buffer's
    Tex tex = Tex::None;
    Smp smp = Smp::None;
    uint8_t texSlot = 0, smpSlot = 0;
    uint32_t tsc = 0;  // a handle's sampler index
};

// layouts.py tic_kind
Tex textureKind(const nvdec::ProbeRecord::Header &t, const std::map<std::string, uint32_t> &targets, uint8_t &slot)
{
    const uint32_t width = (t.w[4] & 0xFFFF) + 1, height = (t.w[5] & 0xFFFF) + 1;
    if (width >= 16 && width < 32 && height == width - 8) {
        slot = uint8_t(width - 16);
        return Tex::Slot;
    }
    if (const auto it = targets.find(t.name); it != targets.end()) {
        slot = uint8_t(it->second);
        return Tex::Fetch;
    }
    return (t.w[4] >> 23 & 0xF) == kTextureType3D ? Tex::Texture3D : Tex::Unknown;
}

// layouts.py tsc_kind: probe.mm tags fragment sampler slot s with max LOD 1 + s/2 (nearest mip filter), linear filters when s&1
// and a repeating U when s&2; a header without such a tag is a constexpr sampler's
Smp samplerKind(const std::array<uint32_t, 8> &t, uint8_t &slot)
{
    const uint32_t lod = t[2] >> 12 & 0xFFF, mag = t[1] & 7, u = t[0] & 7;
    if (lod % 256 || lod < 256 || lod > 256 * 8 || (mag != 1 && mag != 2)) {
        return Smp::Constexpr;
    }
    const uint32_t s = 2 * (lod / 256 - 1) + (mag == 2 ? 1 : 0);
    if (s > 15 || u != (s & 2 ? 0u : 2u))
        return Smp::Derived;
    slot = uint8_t(s);
    return Smp::Slot;
}

// layouts.py layout: set 0's entries; false: a problem (`why`)
bool layout(const nvdec::ProbeSubmission &s, uint32_t index, std::vector<LayoutEntry> &out, std::string &why)
{
    const nvdec::ProbeRecord &r = s.records[0];
    const std::vector<uint32_t> &words = r.sets[0].words;
    for (size_t o = 0; o < words.size();) {
        if (o % 4 == 0) {
            if (const auto p = r.setPointers[0].find(uint32_t(o * 4)); p != r.setPointers[0].end()) {
                const std::vector<uint32_t> &pw = p->second.words;
                const uint32_t stage = pw.size() > 1 ? pw[0] >> 8 & 0xFF : 0;
                if (pw.size() < 2 || pw[0] >> 16 != kTagBuffer || pw[1] != index || (stage != 1 && stage != 2)) {
                    why = "untagged pointer at " + std::to_string(o * 4) + " (" + hex(pw.empty() ? 0 : pw[0]) + ")";
                    return false;
                }
                LayoutEntry e;
                e.offset = uint32_t(o * 4);
                e.buffer = true;
                e.stage = uint8_t(stage);
                e.slot = uint8_t(pw[0] & 0xFF);
                out.push_back(e);
                o += 4;
                continue;
            }
        }
        const uint32_t w = words[o];
        if (w) {
            LayoutEntry e;
            e.offset = uint32_t(o * 4);
            e.tsc = w >> 20;
            if (const uint32_t tic = w & 0xFFFFF) {
                const auto t = r.tic.find(tic);
                e.tex = t == r.tic.end() ? Tex::Unknown : textureKind(t->second, s.targets, e.texSlot);
            }
            const auto smp = r.tsc.find(e.tsc);
            if (e.tsc == 0)
                e.smp = Smp::None;  // NVK's reserved sampler (nvk_device.c): the handle names no sampler
            else
                e.smp = smp == r.tsc.end() ? Smp::Constexpr : samplerKind(smp->second, e.smpSlot);
            if (e.tex == Tex::Unknown) {
                why = "unidentified handle at " + std::to_string(o * 4) + ": " + hex(w);
                return false;
            }
            out.push_back(e);
        }
        o++;
    }
    return true;
}

// mkpack.py load_sizes: by set-0 byte, the range bound for the payload of a run whose draw is one record (missing: 0)
std::map<uint32_t, uint32_t> boundSizes(const nvdec::ProbeSubmission *s)
{
    std::map<uint32_t, uint32_t> out;
    if (!s || s->draws != 1 || s->dispatches || s->records.empty() || s->records[0].sets[0].words.empty())
        return out;
    const std::vector<uint32_t> &w = s->records[0].sets[0].words;
    for (size_t o = 0; o + 3 < w.size(); o += 4)
        if (s->records[0].setPointers[0].count(uint32_t(o * 4)))
            out[uint32_t(o * 4)] = w[o + 2];
    return out;
}

// mkpack.py attach_residuals: the root push words the bind-offset run set where the 256-byte run left 0, by their value's low byte
std::map<uint32_t, uint32_t> residualWords(const nvdec::ProbeSubmission *plain, const nvdec::ProbeSubmission *bound)
{
    std::map<uint32_t, uint32_t> out;
    if (!plain || !bound || bound->draws != 1 || bound->dispatches || bound->records.empty() || plain->records.empty())
        return out;
    std::map<uint32_t, uint32_t> was;
    for (const auto &[offset, word] : plain->records[0].pushWords)
        was[offset] = word;
    for (const auto &[offset, word] : bound->records[0].pushWords)
        if (word && !was[offset])
            out[word & 0xff] = offset;
    return out;
}

// packfile.py entry_bytes, from mkpack.py buffer_entry: a 16-byte payload bound at 256 is a uniform buffer (NVK rounds its range
// up to the cbuf alignment), else storage; the 4096-byte one's range is the function's window on the slot (0: the whole payload);
// a uniform one's residual push word (packfile.py buffer_kind)
PackEntry bufferEntry(const LayoutEntry &e, const std::map<uint32_t, uint32_t> &sizes16,
                      const std::map<uint32_t, uint32_t> &sizes4096, const std::map<uint32_t, uint32_t> &residuals)
{
    uint32_t s16 = 256, s4k = 256;
    if (sizes16.count(e.offset) || sizes4096.count(e.offset)) {
        s16 = valueOf(sizes16, e.offset);
        s4k = valueOf(sizes4096, e.offset);
    }
    const uint32_t window = s4k >= 4096 ? 0 : s4k;
    PackEntry p = {};
    p.offset = uint16_t(e.offset);
    p.kind = EntryUbo;
    p.stage = e.stage;
    p.slot = e.slot;
    p.samplerKind = s16 < 256 ? 1 : 0;
    if (const auto at = residuals.find(probeBindOffset(e.stage, e.slot)); !p.samplerKind && at != residuals.end())
        p.samplerKind = uint8_t(((at->second - 0x700) / 4 + 1) << 2);
    p.sampler = uint8_t(window / 16);
    p.pad = uint8_t(window / 16 >> 8);
    return p;
}

// packfile.py entry_bytes, from mkpack.py handle_entry; false: a problem (`why`)
bool handleEntry(const LayoutEntry &e, const nvdec::ProbeRecord &r, const PackRules &rules, RuntimeRecord &rec,
                 PackEntry &p, std::string &why)
{
    p = {};
    p.offset = uint16_t(e.offset);
    p.stage = 2;
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
        why = "a texture3d argument, and the raw path has no 3D textures";
        return false;
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
        break;
    case Smp::Derived:
        why = "a sampler m2v derives from a bound one (YCbCr conversion)";
        return false;
    case Smp::Constexpr: {
        // a sampler the probe did not bind: the function's constexpr sampler (made by m2v as an immutable one)
        const auto t = r.tsc.find(e.tsc);
        if (t == r.tsc.end()) {
            why = "a constexpr sampler's header unread";
            return false;
        }
        size_t at = std::find(rules.samplers.begin(), rules.samplers.end(), t->second) - rules.samplers.begin();
        if (at == rules.samplers.size()) {
            const size_t own = std::find(rec.samplers.begin(), rec.samplers.end(), t->second) - rec.samplers.begin();
            if (own == rec.samplers.size())
                rec.samplers.push_back(t->second);
            at = rules.samplers.size() + own;
        }
        p.samplerKind = SamplerConstexpr;
        p.sampler = uint8_t(at);
        break;
    }
    }
    return true;
}

// mkpack.py vertex_streams: the Metal vertex buffer slot and stride of the first stream; false: a stream fed by no vertex slot
bool vertexStream(const nvdec::ProbeRecord &r, uint8_t &slot, uint16_t &stride, std::string &why)
{
    slot = 0xFF;
    stride = 0;
    for (const auto &[j, stream] : r.streams) {
        const uint32_t w0 = stream.words.empty() ? 0 : stream.words[0];
        if (w0 >> 16 != kTagBuffer || (w0 >> 8 & 0xFF) != 1) {
            why = "vertex stream " + std::to_string(j) + " not a vertex slot";
            return false;
        }
        if (slot == 0xFF) {
            slot = uint8_t(w0 & 0xFF);
            stride = uint16_t(stream.format & 0xFFF);
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

uint64_t packKeyHash(const std::string &key)
{
    for (const char *lib : {"ycbcr", "xr", "two_plane"}) {
        const size_t own = key.find(std::string("| f ") + lib + "_");
        if (own == std::string::npos)
            continue;
        size_t end = own + 4;
        while (end < key.size() && (isalnum(static_cast<unsigned char>(key[end])) || key[end] == '_'))
            end++;
        const std::string name = std::string("raw.") + lib + "." + key.substr(own + 4, end - own - 4);
        return Pack::fnv(name.data(), name.size());
    }
    return Pack::fnv(key.data(), key.size());
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
    const uint32_t *samplers = pack.section<uint32_t>(PackSamplers);
    for (uint32_t i = 0; i < pack.header()->samplers; i++) {
        std::array<uint32_t, 8> w;
        memcpy(w.data(), samplers + 8 * i, sizeof w);
        r.samplers.push_back(w);
    }
    return r;
}

bool makeRecord(const ProbeRuns &runs, const PackRules &rules, const ProgramLookup &program, RuntimeRecord &out,
                std::string &why)
{
    out = {};
    const nvdec::ProbeSubmission &s = *runs.bytes256;
    if (skipReason(s, why))
        return false;
    const nvdec::ProbeRecord &r = s.records[0];
    if (valueOf(r.regs, kInterlockControl) & 3) {
        why = "storage form (its barrier form needs NVK's interlock hidden)";
        return false;
    }
    for (int k = 0; k < 2; k++) {
        const CapturedProgram *p = program(r.programs.at(k ? 5 : 1).id);
        if (!p) {
            why = "unknown program";
            return false;
        }
        if (!rawBindable(*p)) {
            why = "a program reads constant buffers " + cbufList(p->cbufTypes) +
                  ", the raw path binds root, shader data, set 0 and its uniform buffers";
            return false;
        }
        out.programs[k] = *p;
    }
    std::vector<LayoutEntry> layoutEntries;
    if (!layout(s, runs.index, layoutEntries, why))
        return false;
    if (layoutEntries.size() > 16) {
        why = "more than 16 set-0 entries";
        return false;
    }
    PackPipeline &p = out.pipeline;
    const std::map<uint32_t, uint32_t> sizes16 = boundSizes(runs.bytes16), sizes4096 = boundSizes(runs.bytes4096),
                                       residuals = residualWords(runs.bytes256, runs.bound);
    for (const LayoutEntry &e : layoutEntries) {
        PackEntry &entry = p.entry[p.entries++];
        if (e.buffer)
            entry = bufferEntry(e, sizes16, sizes4096, residuals);
        else if (!handleEntry(e, r, rules, out, entry, why))
            return false;
        if (entry.kind == EntryFetch)
            p.flags |= PipelineFetches;
    }
    if (!ubosNamed(out.programs[0], p) || !ubosNamed(out.programs[1], p)) {
        why = "a program reads a set-0 uniform buffer the layout does not name";
        return false;
    }
    if (!vertexStream(r, p.vertexSlot, p.vertexStride, why))
        return false;
    const uint32_t colors = targets(runs.key);
    std::map<uint32_t, uint32_t> regs;
    for (const auto &[m, v] : r.regs)
        if (!foreign(m, colors))
            regs[m] = v;
    registers(regs, colors, rules, out);
    p.keyHash = packKeyHash(runs.key);
    p.vs = 0;
    p.fs = 1;
    p.blobDwords = uint32_t(out.blob.size());
    for (const auto &[offset, word] : r.pushWords) {
        if (offset < 0x700 || offset >= 0x780)
            continue;
        p.pushMask |= 1u << (offset - 0x700) / 4;
        p.pushWords[(offset - 0x700) / 4] = word;
        p.pushes++;
    }
    const std::vector<uint32_t> &root = r.root.words;
    p.setBytes = r.sets[0].words.empty() || root.size() <= 0x38 / 4 + 2 ? 0 : uint16_t(root[0x38 / 4 + 2]);
    const size_t bar = runs.key.find(" | ");
    out.keyString = runs.key.substr(0, bar);
    while (!out.keyString.empty() && isspace(static_cast<unsigned char>(out.keyString.back())))
        out.keyString.pop_back();
    return true;
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
    return slot;
}

}  // namespace raw

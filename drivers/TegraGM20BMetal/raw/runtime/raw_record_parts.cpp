// SPDX-License-Identifier: MIT
// See raw_record_parts.h. Each step follows the pack/mkpack.py or pack/packfile.py function it is named after: they are one rule,
// and a change to one is a change to the other (test/recordcheck fails otherwise).
#include "raw_record_parts.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>
#include "raw_owned.h"

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
    if (const auto at = parts.residuals.find(value); at != parts.residuals.end() && !(p.samplerKind && codeShared))
        p.samplerKind |= uint8_t(((at->second - 0x700) / 4 + 1) << 2);
    p.sampler = uint8_t(window / 16);
    p.pad = uint8_t(window / 16 >> 8);
    return p;
}

// packfile.py entry_bytes, from mkpack.py handle_entry; false: a problem (`why`)
bool handleEntry(const LayoutEntry &e, const PackRules &rules, RuntimeRecord &rec, PackEntry &p, std::string &why)
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
        p.kind = EntryTexture3d;
        p.slot = e.texSlot;
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
        size_t at = std::find(rules.samplers.begin(), rules.samplers.end(), e.header) - rules.samplers.begin();
        if (at == rules.samplers.size()) {
            const size_t own = std::find(rec.samplers.begin(), rec.samplers.end(), e.header) - rec.samplers.begin();
            if (own == rec.samplers.size())
                rec.samplers.push_back(e.header);
            at = rules.samplers.size() + own;
        }
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

bool foreign(uint32_t m, uint32_t targets)
{
    return (m >= 0x1E00 && m < 0x1F00 && (m - 0x1E00) / 0x20 >= targets) ||
           (m >= 0x1A00 && m < 0x1A20 && (m - 0x1A00) / 4 >= targets) ||
           (m >= 0x1360 && m < 0x1380 && (m - 0x1360) / 4 >= targets);
}

bool programsBindable(const RecordParts &parts, std::string &why)
{
    for (const CapturedProgram *p : parts.programs) {
        if (!p) {
            why = "unknown program";
            return false;
        }
        if (!rawBindable(*p)) {
            why = "a program reads constant buffers " + cbufList(p->cbufTypes) +
                  ", the raw path binds root, shader data, set 0 and its uniform buffers";
            return false;
        }
    }
    return true;
}

bool assembleRecord(const RecordParts &parts, const std::string &key, const PackRules &rules, RuntimeRecord &out,
                    std::string &why)
{
    if (!programsBindable(parts, why))
        return false;
    out.programs[0] = *parts.programs[0];
    out.programs[1] = *parts.programs[1];
    if (parts.layout.size() > kPackEntries) {
        why = "more than " + std::to_string(kPackEntries) + " set-0 entries";
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
        else if (e.buffer)
            entry = bufferEntry(e, parts, storageCodes[probeStorageCode(e.stage, e.slot)] > 1);
        else if (!handleEntry(e, rules, out, entry, why))
            return false;
        if (entry.kind == EntryFetch)
            p.flags |= PipelineFetches;
    }
    if (!ubosNamed(out.programs[0], p) || !ubosNamed(out.programs[1], p)) {
        why = "a program reads a set-0 uniform buffer the layout does not name";
        return false;
    }
    p.vertexSlot = parts.vertexSlot;
    p.vertexStride = parts.vertexStride;
    const uint32_t colors = targets(key);
    std::map<uint32_t, uint32_t> regs;
    for (const auto &[m, v] : parts.regs)
        if (!foreign(m, colors))
            regs[m] = disabledStream(m, v) ? 0 : v;
    registers(regs, colors, rules, out);
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
    const size_t bar = key.find(" | ");
    out.keyString = key.substr(0, bar);
    while (!out.keyString.empty() && isspace(static_cast<unsigned char>(out.keyString.back())))
        out.keyString.pop_back();
    return true;
}

uint64_t packKeyHash(const std::string &key)
{
    for (const char *lib : {"ycbcr", "xr", "two_plane", "resolve"}) {
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
    const uint32_t *samplers = pack.section<uint32_t>(PackSamplers);
    for (uint32_t i = 0; i < pack.header()->samplers; i++) {
        std::array<uint32_t, 8> w;
        memcpy(w.data(), samplers + 8 * i, sizeof w);
        r.samplers.push_back(w);
    }
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
    return slot;
}

}  // namespace raw

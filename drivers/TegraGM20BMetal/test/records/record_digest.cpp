// SPDX-License-Identifier: MIT
// See record_digest.h.
#include "drivers/TegraGM20BMetal/test/records/record_digest.h"

#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <sstream>

namespace records {
namespace {

using namespace raw;

template <class T> void put(std::vector<uint8_t> &out, const T &value)
{
    const auto *p = reinterpret_cast<const uint8_t *>(&value);
    out.insert(out.end(), p, p + sizeof value);
}

void put(std::vector<uint8_t> &out, const void *p, size_t n)
{
    const auto *b = static_cast<const uint8_t *>(p);
    out.insert(out.end(), b, b + n);
}

void put(std::vector<uint8_t> &out, const std::string &s)
{
    put(out, s.data(), s.size() + 1);
}

// The program's record with its heap placement left out, then its slot's bytes (header, code, shader data)
void program(std::vector<uint8_t> &out, const Pack &pack, uint32_t index)
{
    if (uint64_t(index + 1) * sizeof(PackProgram) > pack.bytes(PackPrograms))
        return put(out, uint32_t(0xFFFFFFFF));
    PackProgram p = pack.section<PackProgram>(PackPrograms)[index];
    const uint32_t at = p.heapOffset;
    p.program -= p.heapOffset;
    p.heapOffset = 0;
    put(out, p);
    if (uint64_t(at) + p.bytes <= pack.bytes(PackHeap))
        put(out, pack.section<uint8_t>(PackHeap) + at, p.bytes);
}

void clearDispatchWords(uint32_t (&q)[64])
{
    q[8] = q[12] = q[13] = 0;
    for (int i = 0; i < 8; i++) {
        q[29 + 2 * i] = 0;
        q[30 + 2 * i] &= ~0xFFu;
    }
}

// A constexpr sampler's words, which records name by their index in the pack's samplers section
void sampler(std::vector<uint8_t> &out, const Pack &pack, uint32_t index)
{
    if (uint64_t(index + 1) * 32 > pack.bytes(PackSamplers))
        return put(out, uint32_t(0xFFFFFFFF));
    put(out, pack.section<uint32_t>(PackSamplers) + 8 * index, 32);
}

bool handle(const PackEntry &e)
{
    return e.kind == EntrySamplerOnly || e.kind == EntryTexture || e.kind == EntryFetch || e.kind == EntryTexture3d;
}

using Registers = std::map<uint32_t, uint32_t>;

struct Context {
    Registers init;      // each 3D method's last value in the context init, address words left out
    Registers baseline;  // the registers every state of the pack leaves the same
};

Context context(const Pack &pack)
{
    Context c;
    const PackInitMethod *m = pack.section<PackInitMethod>(PackInit);
    for (uint64_t i = 0; i < pack.bytes(PackInit) / sizeof(PackInitMethod); i++)
        if (m[i].subc == 0 && m[i].reloc == RelocNone)
            c.init[m[i].mthd] = m[i].data;
    const uint32_t *base = pack.section<uint32_t>(PackBaseline);
    for (uint64_t i = 0; i < pack.bytes(PackBaseline) / 8; i++)
        c.baseline[base[2 * i]] = base[2 * i + 1];
    return c;
}

uint32_t colorTargets(const std::string &key)
{
    std::istringstream fields(key);
    std::string field;
    uint32_t n = 0;
    while (fields >> field)
        if (field.size() > 1 && field[0] == 'c' && isdigit(uint8_t(field[1])) && field.find('=') != std::string::npos)
            n++;
    return n;
}

// A colour target's register past the state's targets (mkpack.py foreign): the state leaves it as the last draw did
bool foreign(uint32_t m, uint32_t targets)
{
    return (m >= 0x1E00 && m < 0x1F00 && (m - 0x1E00) / 0x20 >= targets) ||
           (m >= 0x1A00 && m < 0x1A20 && (m - 0x1A00) / 4 >= targets) ||
           (m >= 0x1360 && m < 0x1380 && (m - 0x1360) / 4 >= targets);
}

// The registers a state's draw leaves other than the context init does: its blob over the pack's baseline. Which of them
// a record's blob carries and which the baseline does depends on the other states the pack holds (a register all of them
// set alike is baseline), so a pack of fewer states moves both while every state's registers stay
std::vector<uint8_t> registers(const Pack &pack, const Context &c, const PackPipeline &pp, uint32_t targets)
{
    Registers state = c.baseline;
    if (uint64_t(pp.blob + pp.blobDwords) * 4 <= pack.bytes(PackBlob)) {
        const uint32_t *blob = pack.section<uint32_t>(PackBlob) + pp.blob;
        for (uint32_t i = 0; i + 1 < pp.blobDwords; i += 2)
            state[blob[i]] = blob[i + 1];
    }
    std::vector<uint8_t> out;
    for (const auto &[m, v] : state) {
        const auto init = c.init.find(m);
        if (foreign(m, targets) || v == (init == c.init.end() ? 0 : init->second))
            continue;
        put(out, m);
        put(out, v);
    }
    return out;
}

uint32_t traits(const Pack &pack, const PackPipeline &pp)
{
    uint32_t t = 0, rung = 0;
    if (pp.flags & PipelineFetches)
        t |= TraitFetches;
    for (uint32_t e = 0; e < pp.entries && e < kPackEntries; e++)
        if (pp.entry[e].kind == EntryTexture3d)
            t |= TraitTexture3d;
    if (pack.split(pp.fs))
        t |= TraitSplit;
    if (pp.iterated)
        t |= TraitIterated;
    if (pp.variantCount[0] || pp.variantCount[1])
        t |= TraitVariants;
    if (pp.storageMask)
        t |= TraitStorage;
    if (pp.storageMask && pp.storageTests >= 2 && !pp.shaderTests.flags)
        t |= TraitStorageTests;
    if (pp.spillWords[0] || pp.spillWords[1])
        t |= TraitSpill;
    if (pp.heapArrays)
        t |= TraitHeap;
    for (uint16_t format : pp.writtenFormat)
        if (format)
            t |= TraitWritten;
    if ((pp.flags & PipelineTwoPlane) || pp.xrSlots)
        t |= TraitTwoPlane;
    for (uint8_t r : pp.fetchRung)
        rung = std::max<uint32_t>(rung, r & kFetchRungMask);
    return t | rung << TraitRungShift;
}

void spills(std::vector<uint8_t> &out, const uint32_t *words, uint32_t count)
{
    if (words)
        put(out, words, size_t(count) * 4);
}

Record pipeline(const Pack &pack, const Context &c, const PackPipeline &pp)
{
    Record r;
    r.label = pack.string(pp.keyString);
    r.key = r.state = pp.keyHash;
    r.kind = Kind(uint8_t(recordForm(pp)));
    std::vector<uint8_t> key;
    put(key, r.label);
    if (pp.flags & PipelineVariantRecord) {
        const std::string name = pack.string(pp.variantName);
        put(key, name);
        const uint64_t mixed[3] = {pp.keyHash, Pack::fnv(name.data(), name.size()), uint64_t(r.kind)};
        r.kind = Kind::Variant;
        r.key = Pack::fnv(mixed, sizeof mixed);
        r.label += name;
    }
    r.traits = traits(pack, pp);
    r.sections.push_back({"key", key});

    PackPipeline flat = pp;
    flat.keyString = flat.vs = flat.fs = flat.blob = flat.blobDwords = flat.variants = flat.variantName = flat.spillAt =
        0;
    flat.iterated = pp.iterated ? 1 : 0;
    std::vector<uint8_t> samplers;
    for (uint32_t e = 0; e < pp.entries && e < kPackEntries; e++)
        if (handle(pp.entry[e]) && pp.entry[e].samplerKind == SamplerConstexpr) {
            sampler(samplers, pack, pp.entry[e].sampler);
            flat.entry[e].sampler = 0;
        }
    std::vector<uint8_t> record;
    put(record, flat);
    r.sections.push_back({"record", record});
    r.sections.push_back({"samplers", samplers});
    std::vector<uint8_t> spilled;
    spills(spilled, pack.spills(pp), uint32_t(pp.spillWords[0]) + pp.spillWords[1]);
    r.sections.push_back({"spills", spilled});

    for (const auto &[name, index] : {std::pair{"vs", pp.vs}, std::pair{"fs", pp.fs}}) {
        std::vector<uint8_t> bytes;
        program(bytes, pack, index);
        r.sections.push_back({name, bytes});
    }

    std::vector<uint8_t> blob;
    if (uint64_t(pp.blob + pp.blobDwords) * 4 <= pack.bytes(PackBlob))
        put(blob, pack.section<uint32_t>(PackBlob) + pp.blob, size_t(pp.blobDwords) * 4);
    r.sections.push_back({"blob", blob});
    r.sections.push_back({"registers", registers(pack, c, pp, colorTargets(pack.string(pp.keyString)))});

    std::vector<uint8_t> variants;
    for (uint32_t stage = 0; stage < 2; stage++) {
        uint32_t count = 0;
        const PackVariant *v = pack.variants(pp, stage, count);
        put(variants, count);
        for (uint32_t i = 0; i < count; i++) {
            put(variants, v[i].assume);
            program(variants, pack, v[i].program);
        }
    }
    r.sections.push_back({"variants", variants});

    std::vector<uint8_t> iterated;
    if (const PackIteratedBlend *it = pack.iterated(pp))
        put(iterated, *it);
    r.sections.push_back({"iterated", iterated});

    std::vector<uint8_t> split;
    if (const PackSplit *s = pack.split(pp.fs)) {
        PackSplit flatSplit = *s;
        flatSplit.whole = flatSplit.fs = flatSplit.prologue = 0;
        clearDispatchWords(flatSplit.qmd);
        put(split, flatSplit);
        program(split, pack, s->fs);
        program(split, pack, s->prologue);
    }
    r.sections.push_back({"split", split});
    return r;
}

Record kernel(const Pack &pack, const PackKernel &k)
{
    Record r;
    r.label = pack.string(k.keyString);
    r.key = r.state = k.keyHash;
    r.kind = Kind::Kernel;
    r.traits = (k.spillWords ? TraitSpill : 0) | (k.heapArrays ? TraitHeap : 0);
    std::vector<uint8_t> key;
    put(key, r.label);
    r.sections.push_back({"key", key});
    PackKernel flat = k;
    flat.keyString = flat.program = flat.spillAt = 0;
    clearDispatchWords(flat.qmd);
    constexpr uint8_t kConstexprSampler = 0x80;
    std::vector<uint8_t> samplers;
    for (uint32_t e = 0; e < k.entries && e < kKernelEntries; e++)
        if (k.entry[e].kind == KernelSampler && (k.entry[e].slot & kConstexprSampler)) {
            sampler(samplers, pack, k.entry[e].slot & ~kConstexprSampler);
            flat.entry[e].slot = kConstexprSampler;
        }
    std::vector<uint8_t> record;
    put(record, flat);
    r.sections.push_back({"record", record});
    r.sections.push_back({"samplers", samplers});
    std::vector<uint8_t> spilled;
    spills(spilled, pack.spills(k), k.spillWords);
    r.sections.push_back({"spills", spilled});
    std::vector<uint8_t> code;
    program(code, pack, k.program);
    r.sections.push_back({"program", code});
    // its compute state's facts (PackComputeStates), filed under the state's key: the kernel's up to its shape
    std::vector<uint8_t> facts;
    const std::string state = r.label.substr(0, r.label.find(" | tg="));
    if (const PackComputeFacts *f = pack.computeFacts(Pack::fnv(state.data(), state.size())))
        put(facts, *f);
    r.sections.push_back({"facts", facts});
    return r;
}

Record whole(const Pack &pack, uint64_t key, const char *name, PackSectionId id)
{
    Record r;
    r.key = key;
    r.kind = Kind::Pack;
    r.label = name;
    std::vector<uint8_t> bytes;
    if (id == PackInit) {
        const PackInitMethod *m = pack.section<PackInitMethod>(PackInit);
        for (uint64_t i = 0; i < pack.bytes(PackInit) / sizeof(PackInitMethod); i++) {
            PackInitMethod flat = m[i];
            if (flat.reloc != RelocNone)
                flat.data = 0;
            put(bytes, flat);
        }
    } else {
        put(bytes, pack.section<uint8_t>(id), pack.bytes(id));
    }
    r.sections.push_back({name, bytes});
    return r;
}

}  // namespace

std::vector<Record> records(const Pack &pack)
{
    std::vector<Record> out;
    const Context c = context(pack);
    const PackPipeline *p = pack.section<PackPipeline>(PackPipelines);
    for (uint32_t i = 0; i < pack.header()->pipelines; i++)
        out.push_back(pipeline(pack, c, p[i]));
    const PackKernel *k = pack.section<PackKernel>(PackKernels);
    for (uint64_t i = 0; i < pack.bytes(PackKernels) / sizeof(PackKernel); i++)
        out.push_back(kernel(pack, k[i]));
    out.push_back(whole(pack, kBaselineRecord, "baseline", PackBaseline));
    out.push_back(whole(pack, kInitRecord, "init", PackInit));
    out.push_back(whole(pack, kSamplersRecord, "samplers", PackSamplers));
    out.push_back(whole(pack, kArchivesRecord, "archives", PackArchives));
    out.push_back(whole(pack, kComputeStatesRecord, "compute_states", PackComputeStates));
    std::stable_sort(out.begin(), out.end(), [](const Record &a, const Record &b) {
        return a.key != b.key ? a.key < b.key : a.kind < b.kind;
    });
    return out;
}

std::array<uint8_t, 32> sha256(const std::vector<uint8_t> &bytes)
{
    std::array<uint8_t, 32> out;
    CC_SHA256(bytes.data(), CC_LONG(bytes.size()), out.data());
    return out;
}

Digest digest(const std::vector<Section> &sections)
{
    std::vector<uint8_t> all;
    for (const Section &s : sections) {
        put(all, s.name);
        put(all, uint64_t(s.bytes.size()));
        all.insert(all.end(), s.bytes.begin(), s.bytes.end());
    }
    const auto full = sha256(all);
    Digest d;
    std::copy_n(full.begin(), d.size(), d.begin());
    return d;
}

std::string hex(const uint8_t *bytes, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; i++) {
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 15];
    }
    return out;
}

}  // namespace records
